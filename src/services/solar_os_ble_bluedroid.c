#include "solar_os_ble_backend.h"
#include "solar_os_ble_bluedroid.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef enum { CLIENT_NONE, CLIENT_CONNECT, CLIENT_READ, CLIENT_WRITE } client_op_t;

static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
static uint16_t next_app_id; /* Never reused within a boot, including after sleep. */
static struct {
    uint32_t epoch;
    uint32_t request;
    uint16_t app_id;
    esp_gatt_if_t interface;
    uint16_t conn_id;
    uint16_t handle;
    uint8_t bda[6];
    uint8_t addr_type;
    client_op_t op;
    bool retiring;
    bool unregister_pending;
} client;

static void gatt_uuid_to_string(const esp_bt_uuid_t *uuid, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    if (uuid == NULL) {
        strlcpy(buffer, "-", buffer_len);
        return;
    }

    switch (uuid->len) {
    case ESP_UUID_LEN_16:
        snprintf(buffer, buffer_len, "0x%04x", (unsigned)uuid->uuid.uuid16);
        break;
    case ESP_UUID_LEN_32:
        snprintf(buffer, buffer_len, "0x%08" PRIx32, uuid->uuid.uuid32);
        break;
    case ESP_UUID_LEN_128:
        snprintf(buffer,
                 buffer_len,
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 uuid->uuid.uuid128[15],
                 uuid->uuid.uuid128[14],
                 uuid->uuid.uuid128[13],
                 uuid->uuid.uuid128[12],
                 uuid->uuid.uuid128[11],
                 uuid->uuid.uuid128[10],
                 uuid->uuid.uuid128[9],
                 uuid->uuid.uuid128[8],
                 uuid->uuid.uuid128[7],
                 uuid->uuid.uuid128[6],
                 uuid->uuid.uuid128[5],
                 uuid->uuid.uuid128[4],
                 uuid->uuid.uuid128[3],
                 uuid->uuid.uuid128[2],
                 uuid->uuid.uuid128[1],
                 uuid->uuid.uuid128[0]);
        break;
    default:
        snprintf(buffer, buffer_len, "uuid-len-%u", (unsigned)uuid->len);
        break;
    }
}



esp_err_t solar_os_ble_backend_register(void)
{
    portENTER_CRITICAL(&init_lock);
    if (mutex == NULL) {
        mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
        client.interface = ESP_GATT_IF_NONE;
        client.conn_id = SOLAR_OS_BLE_CONNECTION_INVALID;
    }
    portEXIT_CRITICAL(&init_lock);
    return ESP_OK;
}

static void clear_client_locked(void)
{
    memset(&client, 0, sizeof(client));
    client.interface = ESP_GATT_IF_NONE;
    client.conn_id = SOLAR_OS_BLE_CONNECTION_INVALID;
}

void solar_os_ble_backend_reset(void)
{
    solar_os_ble_backend_register();
    xSemaphoreTake(mutex, portMAX_DELAY);
    clear_client_locked();
    xSemaphoreGive(mutex);
}

/* Bluedroid calls below enqueue work; it never invokes callbacks inline.
 * The adapter mutex orders submission against retirement. It is ALWAYS released
 * before the service event sink runs. UNREG is the host's retirement barrier. */
static esp_err_t retire_client_locked(void)
{
    client.retiring = true;
    if (client.interface == ESP_GATT_IF_NONE || client.unregister_pending) {
        return ESP_OK; /* A pending registration is retired from its REG event. */
    }
    const esp_err_t ret = esp_ble_gattc_app_unregister(client.interface);
    client.unregister_pending = ret == ESP_OK;
    return ret;
}

esp_err_t solar_os_ble_backend_cancel(uint32_t epoch)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    const esp_err_t ret = client.epoch == epoch && epoch != 0 ?
        retire_client_locked() : ESP_OK;
    xSemaphoreGive(mutex);
    return ret;
}

void solar_os_ble_bluedroid_gatt_event(esp_gattc_cb_event_t event,
                                      esp_gatt_if_t gattc_if,
                                      esp_ble_gattc_cb_param_t *param)
{
    /* HID registration can arrive before generic service initialization. */
    solar_os_ble_backend_register();
    xSemaphoreTake(mutex, portMAX_DELAY);
    solar_os_ble_backend_event_t translated = {.epoch = client.epoch};
    bool emit_retired = false;
    if (client.epoch == 0) {
        xSemaphoreGive(mutex);
        return;
    }
    /* IDF 5.5 delivers UNREG with a NULL parameter. Handle it first. */
    if (event == ESP_GATTC_UNREG_EVT) {
        if (gattc_if != client.interface || !client.unregister_pending) {
            xSemaphoreGive(mutex);
            return;
        }
        translated.type = SOLAR_OS_BLE_BACKEND_RETIRED;
        clear_client_locked();
        xSemaphoreGive(mutex);
        solar_os_ble_service_event(&translated);
        return;
    }
    if (param == NULL) {
        xSemaphoreGive(mutex);
        return;
    }
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.app_id != client.app_id || client.interface != ESP_GATT_IF_NONE) {
            xSemaphoreGive(mutex);
            return;
        }
        if (param->reg.status != ESP_GATT_OK) {
            translated.type = SOLAR_OS_BLE_BACKEND_OPENED;
            translated.request = client.request;
            translated.result = ESP_FAIL;
            translated.status = param->reg.status;
            emit_retired = true;
            clear_client_locked();
        } else {
            client.interface = gattc_if;
            if (client.retiring) {
                (void)retire_client_locked();
                xSemaphoreGive(mutex);
                return;
            }
            esp_ble_gatt_creat_conn_params_t params = {0};
            memcpy(params.remote_bda, client.bda, sizeof(client.bda));
            params.remote_addr_type = (esp_ble_addr_type_t)client.addr_type;
            params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
            params.is_direct = true;
            const esp_err_t ret = esp_ble_gattc_enh_open(gattc_if, &params);
            if (ret == ESP_OK) {
                xSemaphoreGive(mutex);
                return;
            }
            translated.type = SOLAR_OS_BLE_BACKEND_OPENED;
            translated.request = client.request;
            translated.result = ret;
            (void)retire_client_locked();
        }
        xSemaphoreGive(mutex);
        solar_os_ble_service_event(&translated);
        if (emit_retired) {
            translated.type = SOLAR_OS_BLE_BACKEND_RETIRED;
            solar_os_ble_service_event(&translated);
        }
        return;
    }
    if (client.interface != gattc_if || client.retiring) {
        xSemaphoreGive(mutex);
        return;
    }
    translated.request = client.request;
    switch (event) {
    case ESP_GATTC_OPEN_EVT:
        if (client.op != CLIENT_CONNECT ||
            memcmp(param->open.remote_bda, client.bda, sizeof(client.bda)) != 0) {
            xSemaphoreGive(mutex);
            return;
        }
        translated.type = SOLAR_OS_BLE_BACKEND_OPENED;
        translated.status = param->open.status;
        translated.conn_id = param->open.conn_id;
        translated.mtu = param->open.mtu;
        memcpy(translated.bda, param->open.remote_bda, sizeof(translated.bda));
        if (param->open.status == ESP_GATT_OK) {
            client.conn_id = param->open.conn_id;
            (void)esp_ble_gattc_send_mtu_req(gattc_if, client.conn_id);
            translated.result = esp_ble_gattc_search_service(gattc_if, client.conn_id, NULL);
            if (translated.result != ESP_OK) {
                (void)retire_client_locked();
            }
        } else {
            (void)retire_client_locked();
        }
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        translated.type = SOLAR_OS_BLE_BACKEND_MTU;
        translated.status = param->cfg_mtu.status;
        translated.conn_id = param->cfg_mtu.conn_id;
        translated.mtu = param->cfg_mtu.mtu;
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        if (client.op != CLIENT_CONNECT) {
            xSemaphoreGive(mutex);
            return;
        }
        translated.type = SOLAR_OS_BLE_BACKEND_SERVICE;
        translated.conn_id = param->search_res.conn_id;
        translated.service.start_handle = param->search_res.start_handle;
        translated.service.end_handle = param->search_res.end_handle;
        translated.service.primary = param->search_res.is_primary;
        gatt_uuid_to_string(&param->search_res.srvc_id.uuid,
                            translated.service.uuid, sizeof(translated.service.uuid));
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (client.op != CLIENT_CONNECT || param->search_cmpl.conn_id != client.conn_id) {
            xSemaphoreGive(mutex);
            return;
        }
        translated.type = SOLAR_OS_BLE_BACKEND_DISCOVERED;
        translated.status = param->search_cmpl.status;
        translated.conn_id = param->search_cmpl.conn_id;
        client.op = CLIENT_NONE;
        client.request = 0;
        if (translated.status != ESP_GATT_OK) {
            (void)retire_client_locked();
        }
        break;
    case ESP_GATTC_READ_CHAR_EVT:
        if (client.op != CLIENT_READ || param->read.handle != client.handle ||
            param->read.conn_id != client.conn_id) {
            xSemaphoreGive(mutex);
            return;
        }
        translated.type = SOLAR_OS_BLE_BACKEND_READ;
        translated.status = param->read.status;
        translated.conn_id = param->read.conn_id;
        translated.handle = param->read.handle;
        translated.value = param->read.value;
        translated.value_len = param->read.value_len;
        client.op = CLIENT_NONE;
        client.request = 0;
        break;
    case ESP_GATTC_WRITE_CHAR_EVT:
        if (client.op != CLIENT_WRITE || param->write.handle != client.handle ||
            param->write.conn_id != client.conn_id) {
            xSemaphoreGive(mutex);
            return;
        }
        translated.type = SOLAR_OS_BLE_BACKEND_WRITTEN;
        translated.status = param->write.status;
        translated.conn_id = param->write.conn_id;
        translated.handle = param->write.handle;
        client.op = CLIENT_NONE;
        client.request = 0;
        break;
    case ESP_GATTC_CLOSE_EVT:
    case ESP_GATTC_DISCONNECT_EVT:
        translated.type = SOLAR_OS_BLE_BACKEND_CLOSED;
        translated.conn_id = event == ESP_GATTC_CLOSE_EVT ? param->close.conn_id : param->disconnect.conn_id;
        translated.reason = event == ESP_GATTC_CLOSE_EVT ? (uint8_t)param->close.reason : (uint8_t)param->disconnect.reason;
        if (translated.conn_id != client.conn_id) {
            xSemaphoreGive(mutex);
            return;
        }
        (void)retire_client_locked();
        break;
    default:
        xSemaphoreGive(mutex);
        return;
    }
    if (event != ESP_GATTC_OPEN_EVT && translated.conn_id != client.conn_id) {
        xSemaphoreGive(mutex);
        return;
    }
    if (translated.status != ESP_GATT_OK) {
        translated.result = ESP_FAIL;
    }
    xSemaphoreGive(mutex);
    solar_os_ble_service_event(&translated);
}

esp_err_t solar_os_ble_backend_connect(uint32_t epoch, uint32_t request,
    const uint8_t bda[6], uint8_t addr_type)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (client.epoch != 0 || next_app_id == 0x7fff) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    clear_client_locked();
    client.epoch = epoch;
    client.request = request;
    client.app_id = ++next_app_id;
    client.op = CLIENT_CONNECT;
    client.addr_type = addr_type;
    memcpy(client.bda, bda, sizeof(client.bda));
    const esp_err_t ret = esp_ble_gattc_app_register(client.app_id);
    if (ret != ESP_OK) {
        clear_client_locked();
    }
    xSemaphoreGive(mutex);
    return ret;
}

static bool connected_locked(uint32_t epoch)
{
    return epoch != 0 && client.epoch == epoch && !client.retiring &&
        client.conn_id != SOLAR_OS_BLE_CONNECTION_INVALID && client.op == CLIENT_NONE;
}

esp_err_t solar_os_ble_backend_characteristics(uint32_t epoch,
    const solar_os_ble_gatt_service_t *service,
    solar_os_ble_gatt_characteristic_t *characteristics,
    size_t max_characteristics, size_t *count)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!connected_locked(epoch)) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_gattc_char_elem_t chars[SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS] = {0};
    uint16_t char_count = max_characteristics > SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS ?
        SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS : (uint16_t)max_characteristics;
    esp_gatt_status_t status = ESP_GATT_OK;
    if (char_count != 0) {
        status = esp_ble_gattc_get_all_char(client.interface, client.conn_id,
            service->start_handle, service->end_handle, chars, &char_count, 0);
    }
    xSemaphoreGive(mutex);
    if (status != ESP_GATT_OK) {
        return ESP_FAIL;
    }
    for (uint16_t i = 0; i < char_count; i++) {
        characteristics[i].handle = chars[i].char_handle;
        characteristics[i].properties = chars[i].properties;
        gatt_uuid_to_string(&chars[i].uuid, characteristics[i].uuid, sizeof(characteristics[i].uuid));
    }
    if (count != NULL) {
        *count = char_count;
    }
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_read(uint32_t epoch, uint32_t request, uint16_t handle)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!connected_locked(epoch)) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    client.op = CLIENT_READ;
    client.request = request;
    client.handle = handle;
    const esp_err_t ret = esp_ble_gattc_read_char(client.interface, client.conn_id,
                                                handle, ESP_GATT_AUTH_REQ_NONE);
    if (ret != ESP_OK) {
        client.op = CLIENT_NONE;
        client.request = 0;
    }
    xSemaphoreGive(mutex);
    return ret;
}

esp_err_t solar_os_ble_backend_write(uint32_t epoch, uint32_t request, uint16_t handle,
    const uint8_t *value, size_t value_len, bool with_response)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!connected_locked(epoch)) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    client.op = CLIENT_WRITE;
    client.request = request;
    client.handle = handle;
    uint8_t buffer[SOLAR_OS_BLE_GATT_VALUE_MAX];
    memcpy(buffer, value, value_len);
    const esp_err_t ret = esp_ble_gattc_write_char(client.interface, client.conn_id,
        handle, (uint16_t)value_len, buffer,
        with_response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
    if (ret != ESP_OK) {
        client.op = CLIENT_NONE;
        client.request = 0;
    }
    xSemaphoreGive(mutex);
    return ret;
}
