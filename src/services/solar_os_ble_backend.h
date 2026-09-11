#pragma once

/* Private service/backend boundary. Implementations must not expose host-stack
 * types here. Currently implemented by the Bluedroid GATT adapter and keyboard
 * lifecycle/profile code. */
#include "solar_os_ble.h"

typedef enum {
    SOLAR_OS_BLE_BACKEND_REGISTERED,
    SOLAR_OS_BLE_BACKEND_OPENED,
    SOLAR_OS_BLE_BACKEND_MTU,
    SOLAR_OS_BLE_BACKEND_SERVICE,
    SOLAR_OS_BLE_BACKEND_DISCOVERED,
    SOLAR_OS_BLE_BACKEND_READ,
    SOLAR_OS_BLE_BACKEND_WRITTEN,
    SOLAR_OS_BLE_BACKEND_CLOSED,
} solar_os_ble_backend_event_type_t;

typedef struct {
    solar_os_ble_backend_event_type_t type;
    uint16_t conn_id;
    esp_err_t result; /* Host-independent success/failure. */
    uint16_t status; /* Backend diagnostic code, never used for service policy. */
    uint16_t handle;
    uint16_t mtu;
    uint8_t bda[6];
    uint8_t reason;
    solar_os_ble_gatt_service_t service;
    const uint8_t *value;
    size_t value_len;
} solar_os_ble_backend_event_t;

/* Synchronous internal sink: consumes/copies borrowed value bytes before
 * returning. Never calls application or interpreter code. */
void solar_os_ble_service_event(const solar_os_ble_backend_event_t *event);
void solar_os_ble_service_reset(const char *status);
esp_err_t solar_os_ble_service_prepare_runtime(void);
esp_err_t solar_os_ble_service_register(void);

esp_err_t solar_os_ble_backend_init(void);
esp_err_t solar_os_ble_backend_scan(solar_os_ble_scan_result_t *results,
                                   size_t max_results, size_t *found);
esp_err_t solar_os_ble_backend_prepare_sleep(uint32_t timeout_ms);
bool solar_os_ble_backend_sleep_prepare_ready(void);
void solar_os_ble_backend_resume(void);

esp_err_t solar_os_ble_backend_register(void);
void solar_os_ble_backend_reset(void);
esp_err_t solar_os_ble_backend_connect(const uint8_t bda[6], uint8_t addr_type);
esp_err_t solar_os_ble_backend_disconnect(uint16_t conn_id);
esp_err_t solar_os_ble_backend_discover(uint16_t conn_id);
esp_err_t solar_os_ble_backend_characteristics(uint16_t conn_id,
    const solar_os_ble_gatt_service_t *service,
    solar_os_ble_gatt_characteristic_t *characteristics,
    size_t max_characteristics, size_t *count);
esp_err_t solar_os_ble_backend_read(uint16_t conn_id, uint16_t handle);
esp_err_t solar_os_ble_backend_write(uint16_t conn_id, uint16_t handle,
    const uint8_t *value, size_t value_len, bool with_response);
