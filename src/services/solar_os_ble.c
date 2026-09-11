#include "solar_os_ble.h"
#include "solar_os_ble_backend.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BLE_GATT_CONNECT_TIMEOUT_MS 12000U
#define BLE_GATT_OPERATION_TIMEOUT_MS 5000U
#define BLE_GATT_INVALID_CONN_ID SOLAR_OS_BLE_CONNECTION_INVALID

typedef enum {
    BLE_GATT_OP_NONE,
    BLE_GATT_OP_CONNECT,
    BLE_GATT_OP_READ,
    BLE_GATT_OP_WRITE,
} ble_gatt_operation_t;

typedef struct {
    bool connected;
    bool registered;
    bool connecting;
    uint16_t conn_id;
    uint16_t mtu;
    uint8_t bda[6];
    uint8_t addr_type;
    solar_os_ble_gatt_service_t services[SOLAR_OS_BLE_GATT_MAX_SERVICES];
    size_t service_count;
    ble_gatt_operation_t op;
    esp_err_t op_result;
    uint8_t op_value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t op_value_len;
    char status[80];
} ble_gatt_state_t;

static SemaphoreHandle_t gatt_mutex;
static SemaphoreHandle_t gatt_op_sem;
static ble_gatt_state_t gatt_state = {
    .conn_id = BLE_GATT_INVALID_CONN_ID,
    .status = "idle",
};

static void gatt_lock(void)
{
    if (gatt_mutex != NULL) {
        xSemaphoreTake(gatt_mutex, portMAX_DELAY);
    }
}

static void gatt_unlock(void)
{
    if (gatt_mutex != NULL) {
        xSemaphoreGive(gatt_mutex);
    }
}

static void gatt_set_status_locked(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(gatt_state.status, sizeof(gatt_state.status), fmt, args);
    va_end(args);
}

void solar_os_ble_service_reset(const char *status)
{
    gatt_lock();
    gatt_state.connected = false;
    gatt_state.registered = false;
    gatt_state.connecting = false;
    gatt_state.conn_id = BLE_GATT_INVALID_CONN_ID;
    gatt_state.mtu = 0;
    gatt_state.service_count = 0;
    gatt_state.op = BLE_GATT_OP_NONE;
    gatt_state.op_result = ESP_OK;
    gatt_state.op_value_len = 0;
    memset(gatt_state.bda, 0, sizeof(gatt_state.bda));
    gatt_set_status_locked("%s", status != NULL ? status : "idle");
    gatt_unlock();
}

esp_err_t solar_os_ble_service_prepare_runtime(void)
{
    if (gatt_mutex == NULL) {
        gatt_mutex = xSemaphoreCreateMutex();
    }
    if (gatt_op_sem == NULL) {
        gatt_op_sem = xSemaphoreCreateBinary();
    }
    return gatt_mutex != NULL && gatt_op_sem != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void gatt_clear_services_locked(void)
{
    memset(gatt_state.services, 0, sizeof(gatt_state.services));
    gatt_state.service_count = 0;
}

static void gatt_drain_op_sem(void)
{
    if (gatt_op_sem == NULL) {
        return;
    }
    while (xSemaphoreTake(gatt_op_sem, 0) == pdTRUE) {
    }
}

static void gatt_complete_operation(ble_gatt_operation_t op, esp_err_t result)
{
    bool should_signal = false;

    gatt_lock();
    if (gatt_state.op == op || op == BLE_GATT_OP_NONE) {
        gatt_state.op_result = result;
        gatt_state.op = BLE_GATT_OP_NONE;
        should_signal = true;
    }
    gatt_unlock();

    if (should_signal && gatt_op_sem != NULL) {
        xSemaphoreGive(gatt_op_sem);
    }
}

static esp_err_t gatt_begin_operation(ble_gatt_operation_t op)
{
    gatt_lock();
    if (gatt_state.op != BLE_GATT_OP_NONE) {
        gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    gatt_state.op = op;
    gatt_state.op_result = ESP_OK;
    gatt_state.op_value_len = 0;
    gatt_unlock();
    gatt_drain_op_sem();
    return ESP_OK;
}

static esp_err_t gatt_wait_operation(ble_gatt_operation_t op, uint32_t timeout_ms)
{
    if (gatt_op_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(gatt_op_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        gatt_lock();
        if (gatt_state.op == op) {
            gatt_state.op = BLE_GATT_OP_NONE;
            gatt_set_status_locked("timeout");
        }
        gatt_unlock();
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result;
    gatt_lock();
    result = gatt_state.op_result;
    gatt_unlock();
    return result;
}

esp_err_t solar_os_ble_service_register(void)
{
    gatt_drain_op_sem();
    solar_os_ble_service_reset("idle");
    const esp_err_t ret = solar_os_ble_backend_register();
    if (ret != ESP_OK) {
        return ret;
    }
    if (xSemaphoreTake(gatt_op_sem, pdMS_TO_TICKS(BLE_GATT_OPERATION_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    gatt_lock();
    const bool registered = gatt_state.registered;
    gatt_unlock();
    return registered ? ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_ble_init(void)
{
    return solar_os_ble_backend_init();
}

esp_err_t solar_os_ble_scan(solar_os_ble_scan_result_t *results,
                            size_t max_results, size_t *found)
{
    return solar_os_ble_backend_scan(results, max_results, found);
}

esp_err_t solar_os_ble_prepare_sleep(uint32_t timeout_ms)
{
    return solar_os_ble_backend_prepare_sleep(timeout_ms);
}

bool solar_os_ble_sleep_prepare_ready(void)
{
    return solar_os_ble_backend_sleep_prepare_ready();
}

void solar_os_ble_resume(void)
{
    solar_os_ble_backend_resume();
}

static void gatt_handle_disconnect(uint16_t conn_id, const uint8_t *bda, uint8_t reason)
{
    bool active = false;

    gatt_lock();
    active = gatt_state.connected &&
        (gatt_state.conn_id == conn_id ||
         (bda != NULL && memcmp(gatt_state.bda, bda, sizeof(gatt_state.bda)) == 0));
    if (active) {
        gatt_state.connected = false;
        gatt_state.connecting = false;
        gatt_state.conn_id = BLE_GATT_INVALID_CONN_ID;
        gatt_state.mtu = 0;
        gatt_clear_services_locked();
        gatt_set_status_locked("disconnected 0x%02x", reason);
    }
    gatt_unlock();

    if (active) {
        gatt_complete_operation(BLE_GATT_OP_NONE, ESP_FAIL);
    }
}

void solar_os_ble_service_event(const solar_os_ble_backend_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case SOLAR_OS_BLE_BACKEND_REGISTERED:
        gatt_lock();
        if (event->result == ESP_OK) {
            gatt_state.registered = true;
            gatt_set_status_locked("idle");
        } else {
            gatt_state.registered = false;
            gatt_set_status_locked("register failed 0x%02x", event->status);
        }
        gatt_unlock();
        if (gatt_op_sem != NULL) {
            xSemaphoreGive(gatt_op_sem);
        }
        break;

    case SOLAR_OS_BLE_BACKEND_OPENED:
        gatt_lock();
        if (event->result != ESP_OK) {
            gatt_state.connecting = false;
            gatt_state.connected = false;
            gatt_state.conn_id = BLE_GATT_INVALID_CONN_ID;
            gatt_set_status_locked("open failed 0x%02x", event->status);
            gatt_unlock();
            gatt_complete_operation(BLE_GATT_OP_CONNECT, event->result);
            break;
        }

        gatt_state.connected = true;
        gatt_state.connecting = false;
        gatt_state.conn_id = event->conn_id;
        gatt_state.mtu = event->mtu;
        memcpy(gatt_state.bda, event->bda, sizeof(gatt_state.bda));
        gatt_clear_services_locked();
        gatt_set_status_locked("discovering");
        gatt_unlock();

        if (solar_os_ble_backend_discover(event->conn_id) != ESP_OK) {
            gatt_lock();
            gatt_set_status_locked("service discovery failed");
            gatt_unlock();
            gatt_complete_operation(BLE_GATT_OP_CONNECT, ESP_FAIL);
        }
        break;

    case SOLAR_OS_BLE_BACKEND_MTU:
        gatt_lock();
        if (gatt_state.connected && gatt_state.conn_id == event->conn_id &&
            event->result == ESP_OK) {
            gatt_state.mtu = event->mtu;
        }
        gatt_unlock();
        break;

    case SOLAR_OS_BLE_BACKEND_SERVICE:
        gatt_lock();
        if (gatt_state.connected &&
            gatt_state.conn_id == event->conn_id &&
            gatt_state.service_count < SOLAR_OS_BLE_GATT_MAX_SERVICES) {
            solar_os_ble_gatt_service_t *service =
                &gatt_state.services[gatt_state.service_count++];
            *service = event->service;
        }
        gatt_unlock();
        break;

    case SOLAR_OS_BLE_BACKEND_DISCOVERED:
        gatt_lock();
        if (gatt_state.connected && gatt_state.conn_id == event->conn_id) {
            if (event->result == ESP_OK) {
                gatt_set_status_locked("connected");
            } else {
                gatt_set_status_locked("search failed 0x%02x", event->status);
            }
        }
        gatt_unlock();
        gatt_complete_operation(BLE_GATT_OP_CONNECT, event->result);
        break;

    case SOLAR_OS_BLE_BACKEND_READ:
        gatt_lock();
        if (gatt_state.connected && gatt_state.conn_id == event->conn_id &&
            gatt_state.op == BLE_GATT_OP_READ) {
            gatt_state.op_result = event->result;
            gatt_state.op_value_len = 0;
            if (event->result == ESP_OK && event->value != NULL) {
                const size_t copy_len =
                    event->value_len < SOLAR_OS_BLE_GATT_VALUE_MAX ?
                    event->value_len :
                    SOLAR_OS_BLE_GATT_VALUE_MAX;
                memcpy(gatt_state.op_value, event->value, copy_len);
                gatt_state.op_value_len = copy_len;
                gatt_set_status_locked("read handle 0x%04x", event->handle);
            } else {
                gatt_set_status_locked("read failed 0x%02x", event->status);
            }
        }
        gatt_unlock();
        gatt_complete_operation(BLE_GATT_OP_READ, event->result);
        break;

    case SOLAR_OS_BLE_BACKEND_WRITTEN:
        gatt_lock();
        if (gatt_state.connected && gatt_state.conn_id == event->conn_id &&
            gatt_state.op == BLE_GATT_OP_WRITE) {
            gatt_state.op_result = event->result;
            if (event->result == ESP_OK) {
                gatt_set_status_locked("wrote handle 0x%04x", event->handle);
            } else {
                gatt_set_status_locked("write failed 0x%02x", event->status);
            }
        }
        gatt_unlock();
        gatt_complete_operation(BLE_GATT_OP_WRITE, event->result);
        break;

    case SOLAR_OS_BLE_BACKEND_CLOSED:
        gatt_handle_disconnect(event->conn_id, event->bda, (uint8_t)event->reason);
        break;

    default:
        break;
    }
}

esp_err_t solar_os_ble_gatt_connect(const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms)
{
    if (bda == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_ble_init();
    if (ret != ESP_OK) {
        return ret;
    }

    gatt_lock();
    if (!gatt_state.registered) {
        gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (gatt_state.connected || gatt_state.connecting) {
        gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    gatt_unlock();

    ret = gatt_begin_operation(BLE_GATT_OP_CONNECT);
    if (ret != ESP_OK) {
        return ret;
    }

    gatt_lock();
    gatt_state.connecting = true;
    gatt_state.connected = false;
    gatt_state.conn_id = BLE_GATT_INVALID_CONN_ID;
    gatt_state.addr_type = addr_type;
    memcpy(gatt_state.bda, bda, sizeof(gatt_state.bda));
    gatt_clear_services_locked();
    gatt_set_status_locked("connecting");
    gatt_unlock();

    ret = solar_os_ble_backend_connect(bda, addr_type);
    if (ret != ESP_OK) {
        gatt_lock();
        gatt_state.connecting = false;
        gatt_set_status_locked("connect failed %s", esp_err_to_name(ret));
        gatt_unlock();
        gatt_complete_operation(BLE_GATT_OP_CONNECT, ESP_FAIL);
        return ret;
    }

    return gatt_wait_operation(BLE_GATT_OP_CONNECT,
                               timeout_ms != 0 ? timeout_ms : BLE_GATT_CONNECT_TIMEOUT_MS);
}

esp_err_t solar_os_ble_gatt_disconnect(void)
{
    esp_err_t ret = solar_os_ble_init();
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t conn_id = BLE_GATT_INVALID_CONN_ID;

    gatt_lock();
    if (!gatt_state.connected) {
        gatt_state.connecting = false;
        gatt_set_status_locked("idle");
        gatt_unlock();
        return ESP_OK;
    }
    conn_id = gatt_state.conn_id;
    gatt_unlock();

    ret = solar_os_ble_backend_disconnect(conn_id);
    if (ret != ESP_OK) {
        return ret;
    }

    gatt_lock();
    gatt_state.connected = false;
    gatt_state.connecting = false;
    gatt_state.conn_id = BLE_GATT_INVALID_CONN_ID;
    gatt_state.mtu = 0;
    gatt_clear_services_locked();
    gatt_set_status_locked("disconnected");
    gatt_unlock();
    return ESP_OK;
}

void solar_os_ble_gatt_get_status(solar_os_ble_gatt_status_t *status)
{
    if (status == NULL) {
        return;
    }

    gatt_lock();
    *status = (solar_os_ble_gatt_status_t){
        .connected = gatt_state.connected,
        .addr_type = (uint8_t)gatt_state.addr_type,
        .conn_id = gatt_state.conn_id,
        .mtu = gatt_state.mtu,
        .service_count = gatt_state.service_count,
    };
    memcpy(status->bda, gatt_state.bda, sizeof(status->bda));
    strlcpy(status->status, gatt_state.status, sizeof(status->status));
    gatt_unlock();
}

esp_err_t solar_os_ble_gatt_services(solar_os_ble_gatt_service_t *services,
                                     size_t max_services,
                                     size_t *count)
{
    if (count != NULL) {
        *count = 0;
    }
    if (max_services > 0 && services == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gatt_lock();
    if (!gatt_state.connected) {
        gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const size_t copy_count =
        gatt_state.service_count < max_services ? gatt_state.service_count : max_services;
    if (copy_count > 0) {
        memcpy(services, gatt_state.services, copy_count * sizeof(services[0]));
    }
    if (count != NULL) {
        *count = gatt_state.service_count;
    }
    gatt_unlock();
    return ESP_OK;
}

esp_err_t solar_os_ble_gatt_characteristics(size_t service_index,
                                            solar_os_ble_gatt_characteristic_t *characteristics,
                                            size_t max_characteristics,
                                            size_t *count)
{
    if (count != NULL) {
        *count = 0;
    }
    if (max_characteristics > 0 && characteristics == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_ble_gatt_service_t service = {0};
    uint16_t conn_id = BLE_GATT_INVALID_CONN_ID;

    gatt_lock();
    if (!gatt_state.connected) {
        gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (service_index >= gatt_state.service_count) {
        gatt_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    service = gatt_state.services[service_index];
    conn_id = gatt_state.conn_id;
    gatt_unlock();

    return solar_os_ble_backend_characteristics(conn_id, &service, characteristics,
                                                 max_characteristics, count);
}

esp_err_t solar_os_ble_gatt_read(uint16_t handle,
                                 uint8_t *value,
                                 size_t max_len,
                                 size_t *value_len,
                                 uint32_t timeout_ms)
{
    if (value_len != NULL) {
        *value_len = 0;
    }
    if (handle == 0 || (max_len > 0 && value == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    gatt_lock();
    const bool can_read = gatt_state.connected;
    const uint16_t conn_id = gatt_state.conn_id;
    gatt_unlock();
    if (!can_read) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = gatt_begin_operation(BLE_GATT_OP_READ);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = solar_os_ble_backend_read(conn_id, handle);
    if (ret != ESP_OK) {
        gatt_complete_operation(BLE_GATT_OP_READ, ESP_FAIL);
        return ret;
    }

    ret = gatt_wait_operation(BLE_GATT_OP_READ,
                              timeout_ms != 0 ? timeout_ms : BLE_GATT_OPERATION_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    gatt_lock();
    const size_t copy_len = gatt_state.op_value_len < max_len ? gatt_state.op_value_len : max_len;
    if (copy_len > 0) {
        memcpy(value, gatt_state.op_value, copy_len);
    }
    if (value_len != NULL) {
        *value_len = gatt_state.op_value_len;
    }
    gatt_unlock();
    return ESP_OK;
}

esp_err_t solar_os_ble_gatt_write(uint16_t handle,
                                  const uint8_t *value,
                                  size_t value_len,
                                  bool with_response,
                                  uint32_t timeout_ms)
{
    if (handle == 0 || value == NULL || value_len == 0 || value_len > SOLAR_OS_BLE_GATT_VALUE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    gatt_lock();
    const bool can_write = gatt_state.connected;
    const uint16_t conn_id = gatt_state.conn_id;
    gatt_unlock();
    if (!can_write) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = gatt_begin_operation(BLE_GATT_OP_WRITE);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = solar_os_ble_backend_write(conn_id, handle, value, value_len, with_response);
    if (ret != ESP_OK) {
        gatt_complete_operation(BLE_GATT_OP_WRITE, ESP_FAIL);
        return ret;
    }

    if (!with_response) {
        gatt_complete_operation(BLE_GATT_OP_WRITE, ESP_OK);
        return ESP_OK;
    }

    return gatt_wait_operation(BLE_GATT_OP_WRITE,
                               timeout_ms != 0 ? timeout_ms : BLE_GATT_OPERATION_TIMEOUT_MS);
}
