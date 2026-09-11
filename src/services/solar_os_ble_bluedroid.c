#include "solar_os_ble_backend.h"
#include "solar_os_ble_bluedroid.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define BLE_GATT_APP_ID 1U

static esp_gatt_if_t client_if = ESP_GATT_IF_NONE;

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


void solar_os_ble_backend_reset(void)
{
    client_if = ESP_GATT_IF_NONE;
}

esp_err_t solar_os_ble_backend_register(void)
{
    solar_os_ble_backend_reset();
    return esp_ble_gattc_app_register(BLE_GATT_APP_ID);
}

void solar_os_ble_bluedroid_gatt_event(esp_gattc_cb_event_t event,
                                      esp_gatt_if_t gattc_if,
                                      esp_ble_gattc_cb_param_t *param)
{
    if (param == NULL) {
        return;
    }
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.app_id != BLE_GATT_APP_ID) {
            return;
        }
    } else if (client_if == ESP_GATT_IF_NONE || gattc_if != client_if) {
        return;
    }

    solar_os_ble_backend_event_t translated = {0};
    switch (event) {
    case ESP_GATTC_REG_EVT:
        client_if = param->reg.status == ESP_GATT_OK ? gattc_if : ESP_GATT_IF_NONE;
        translated.type = SOLAR_OS_BLE_BACKEND_REGISTERED;
        translated.status = param->reg.status;
        break;
    case ESP_GATTC_OPEN_EVT:
        translated.type = SOLAR_OS_BLE_BACKEND_OPENED;
        translated.status = param->open.status;
        translated.conn_id = param->open.conn_id;
        translated.mtu = param->open.mtu;
        memcpy(translated.bda, param->open.remote_bda, sizeof(translated.bda));
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        translated.type = SOLAR_OS_BLE_BACKEND_MTU;
        translated.status = param->cfg_mtu.status;
        translated.conn_id = param->cfg_mtu.conn_id;
        translated.mtu = param->cfg_mtu.mtu;
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        translated.type = SOLAR_OS_BLE_BACKEND_SERVICE;
        translated.conn_id = param->search_res.conn_id;
        translated.service.start_handle = param->search_res.start_handle;
        translated.service.end_handle = param->search_res.end_handle;
        translated.service.primary = param->search_res.is_primary;
        gatt_uuid_to_string(&param->search_res.srvc_id.uuid,
                            translated.service.uuid, sizeof(translated.service.uuid));
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        translated.type = SOLAR_OS_BLE_BACKEND_DISCOVERED;
        translated.status = param->search_cmpl.status;
        translated.conn_id = param->search_cmpl.conn_id;
        break;
    case ESP_GATTC_READ_CHAR_EVT:
        translated.type = SOLAR_OS_BLE_BACKEND_READ;
        translated.status = param->read.status;
        translated.conn_id = param->read.conn_id;
        translated.handle = param->read.handle;
        translated.value = param->read.value;
        translated.value_len = param->read.value_len;
        break;
    case ESP_GATTC_WRITE_CHAR_EVT:
        translated.type = SOLAR_OS_BLE_BACKEND_WRITTEN;
        translated.status = param->write.status;
        translated.conn_id = param->write.conn_id;
        translated.handle = param->write.handle;
        break;
    case ESP_GATTC_CLOSE_EVT:
        translated.type = SOLAR_OS_BLE_BACKEND_CLOSED;
        translated.conn_id = param->close.conn_id;
        translated.reason = (uint8_t)param->close.reason;
        memcpy(translated.bda, param->close.remote_bda, sizeof(translated.bda));
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        translated.type = SOLAR_OS_BLE_BACKEND_CLOSED;
        translated.conn_id = param->disconnect.conn_id;
        translated.reason = (uint8_t)param->disconnect.reason;
        memcpy(translated.bda, param->disconnect.remote_bda, sizeof(translated.bda));
        break;
    default:
        return;
    }

    translated.result = translated.status == ESP_GATT_OK ? ESP_OK : ESP_FAIL;
    solar_os_ble_service_event(&translated);
}

esp_err_t solar_os_ble_backend_connect(const uint8_t bda[6], uint8_t addr_type)
{
    esp_ble_gatt_creat_conn_params_t params = {0};
    memcpy(params.remote_bda, bda, ESP_BD_ADDR_LEN);
    params.remote_addr_type = (esp_ble_addr_type_t)addr_type;
    params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    params.is_direct = true;
    params.is_aux = false;
    params.phy_mask = 0x0;
    return esp_ble_gattc_enh_open(client_if, &params);
}

esp_err_t solar_os_ble_backend_disconnect(uint16_t conn_id)
{
    return esp_ble_gattc_close(client_if, conn_id);
}

esp_err_t solar_os_ble_backend_discover(uint16_t conn_id)
{
    (void)esp_ble_gattc_send_mtu_req(client_if, conn_id);
    return esp_ble_gattc_search_service(client_if, conn_id, NULL);
}

esp_err_t solar_os_ble_backend_characteristics(uint16_t conn_id,
    const solar_os_ble_gatt_service_t *service,
    solar_os_ble_gatt_characteristic_t *characteristics,
    size_t max_characteristics, size_t *count)
{
    esp_gattc_char_elem_t chars[SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS] = {0};
    uint16_t char_count = max_characteristics > SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS ?
        SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS : (uint16_t)max_characteristics;
    if (char_count == 0) {
        return ESP_OK;
    }

    const esp_gatt_status_t status = esp_ble_gattc_get_all_char(client_if,
        conn_id, service->start_handle, service->end_handle, chars, &char_count, 0);
    if (status != ESP_GATT_OK) {
        return ESP_FAIL;
    }
    for (uint16_t i = 0; i < char_count; i++) {
        characteristics[i].handle = chars[i].char_handle;
        characteristics[i].properties = chars[i].properties;
        gatt_uuid_to_string(&chars[i].uuid,
                            characteristics[i].uuid, sizeof(characteristics[i].uuid));
    }
    if (count != NULL) {
        *count = char_count;
    }
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_read(uint16_t conn_id, uint16_t handle)
{
    return esp_ble_gattc_read_char(client_if, conn_id, handle, ESP_GATT_AUTH_REQ_NONE);
}

esp_err_t solar_os_ble_backend_write(uint16_t conn_id, uint16_t handle,
    const uint8_t *value, size_t value_len, bool with_response)
{
    /* Bluedroid copies the payload when enqueueing the request. */
    uint8_t buffer[SOLAR_OS_BLE_GATT_VALUE_MAX];
    memcpy(buffer, value, value_len);
    return esp_ble_gattc_write_char(client_if, conn_id, handle, (uint16_t)value_len,
        buffer, with_response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
        ESP_GATT_AUTH_REQ_NONE);
}
