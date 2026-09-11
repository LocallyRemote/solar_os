#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Host-independent BLE API. Numeric addresses/properties use Bluetooth wire
 * values; no Bluedroid or NimBLE headers are part of this contract. */
#define SOLAR_OS_BLE_NAME_MAX 64
#define SOLAR_OS_BLE_SCAN_MAX_RESULTS 32
#define SOLAR_OS_BLE_GATT_UUID_MAX 37
#define SOLAR_OS_BLE_GATT_MAX_SERVICES 24
#define SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS 64
#define SOLAR_OS_BLE_GATT_VALUE_MAX 128
#define SOLAR_OS_BLE_CONNECTION_INVALID UINT16_MAX
#define SOLAR_OS_BLE_SESSION_MAX 4
#define SOLAR_OS_BLE_OWNER_MAX 32
#define SOLAR_OS_BLE_SESSION_INVALID 0U
#define SOLAR_OS_BLE_ERR_CANCELLED ((esp_err_t)0xB1E0)

typedef uint32_t solar_os_ble_session_t;

typedef enum {
    SOLAR_OS_BLE_ADDR_PUBLIC = 0,
    SOLAR_OS_BLE_ADDR_RANDOM = 1,
    SOLAR_OS_BLE_ADDR_PUBLIC_IDENTITY = 2,
    SOLAR_OS_BLE_ADDR_RANDOM_IDENTITY = 3,
} solar_os_ble_addr_type_t;

typedef enum {
    SOLAR_OS_BLE_CHAR_BROADCAST = 0x01,
    SOLAR_OS_BLE_CHAR_READ = 0x02,
    SOLAR_OS_BLE_CHAR_WRITE_NO_RESPONSE = 0x04,
    SOLAR_OS_BLE_CHAR_WRITE = 0x08,
    SOLAR_OS_BLE_CHAR_NOTIFY = 0x10,
    SOLAR_OS_BLE_CHAR_INDICATE = 0x20,
    SOLAR_OS_BLE_CHAR_SIGNED_WRITE = 0x40,
    SOLAR_OS_BLE_CHAR_EXTENDED = 0x80,
} solar_os_ble_characteristic_property_t;

typedef struct {
    uint8_t bda[6];
    uint8_t addr_type;
    int8_t rssi;
    uint16_t appearance;
    bool hid_service;
    bool keyboard_like;
    bool remembered;
    bool connected;
    char name[SOLAR_OS_BLE_NAME_MAX];
} solar_os_ble_scan_result_t;

typedef struct {
    bool connected;
    uint8_t bda[6];
    uint8_t addr_type;
    uint16_t conn_id;
    uint16_t mtu;
    size_t service_count;
    char status[80];
} solar_os_ble_gatt_status_t;

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    bool primary;
    char uuid[SOLAR_OS_BLE_GATT_UUID_MAX];
} solar_os_ble_gatt_service_t;

typedef struct {
    uint16_t handle;
    uint8_t properties;
    char uuid[SOLAR_OS_BLE_GATT_UUID_MAX];
} solar_os_ble_gatt_characteristic_t;

typedef struct {
    char owner[SOLAR_OS_BLE_OWNER_MAX];
    bool busy;
    bool retiring;
    solar_os_ble_gatt_status_t gatt;
} solar_os_ble_session_info_t;

/* Owners retain a session handle and close it on exit (including errors).
 * Handles are generation checked; owner names are diagnostic, not credentials.
 * Four app sessions plus the reserved legacy shell session share ONE peer slot.
 * One blocking operation per session; cancel/close may run from another task.
 * Cancellation aborts the connection and wakes a waiter with CANCELLED; it
 * cannot undo a write already sent. Close invalidates the handle immediately.
 * Transport reuse waits for backend retirement, even after close has returned.
 * All calls except the internal event sink run outside the Bluetooth task. */
esp_err_t solar_os_ble_session_create(const char *owner, solar_os_ble_session_t *session);
esp_err_t solar_os_ble_session_close(solar_os_ble_session_t session);
esp_err_t solar_os_ble_session_cancel(solar_os_ble_session_t session);
esp_err_t solar_os_ble_session_get_info(solar_os_ble_session_t session,
                                      solar_os_ble_session_info_t *info);
esp_err_t solar_os_ble_session_connect(solar_os_ble_session_t session,
    const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms);
esp_err_t solar_os_ble_session_services(solar_os_ble_session_t session,
    solar_os_ble_gatt_service_t *services, size_t max_services, size_t *count);
esp_err_t solar_os_ble_session_characteristics(solar_os_ble_session_t session,
    size_t service_index, solar_os_ble_gatt_characteristic_t *characteristics,
    size_t max_characteristics, size_t *count);
esp_err_t solar_os_ble_session_read(solar_os_ble_session_t session,
    uint16_t handle, uint8_t *value, size_t max_len, size_t *value_len, uint32_t timeout_ms);
esp_err_t solar_os_ble_session_write(solar_os_ble_session_t session,
    uint16_t handle, const uint8_t *value, size_t value_len, bool with_response, uint32_t timeout_ms);

/* Lifecycle calls retain the current boot policy and the OS keyboard profile.
 * Scan results include keyboard hints for compatibility with the shell.
 * Lifecycle transitions and backend submissions are serialized by the service.
 * Sleep cancels generic operations; session handles survive sleep until closed. */
esp_err_t solar_os_ble_init(void);
esp_err_t solar_os_ble_scan(solar_os_ble_scan_result_t *results, size_t max_results, size_t *found);
esp_err_t solar_os_ble_prepare_sleep(uint32_t timeout_ms);
bool solar_os_ble_sleep_prepare_ready(void);
void solar_os_ble_resume(void);

/* Compatibility client: reserved shell session, one operation at a time.
 * conn_id is a diagnostic transport identifier, not an app-owned handle.
 * UUIDs are formatted as 0xNNNN, 0xNNNNNNNN, or canonical 128-bit strings.
 * These synchronous calls must run outside the Bluetooth callback task.
 * A zero timeout selects the existing service default. Timeouts retire the
 * connection. Writes without response wait for local completion, not peer ACK.
 * Reconnect may return INVALID_STATE until backend retirement completes. */
esp_err_t solar_os_ble_gatt_connect(const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms);
esp_err_t solar_os_ble_gatt_disconnect(void);
void solar_os_ble_gatt_get_status(solar_os_ble_gatt_status_t *status);
esp_err_t solar_os_ble_gatt_services(solar_os_ble_gatt_service_t *services,
                                     size_t max_services,
                                     size_t *count);
esp_err_t solar_os_ble_gatt_characteristics(size_t service_index,
                                            solar_os_ble_gatt_characteristic_t *characteristics,
                                            size_t max_characteristics,
                                            size_t *count);
esp_err_t solar_os_ble_gatt_read(uint16_t handle,
                                 uint8_t *value,
                                 size_t max_len,
                                 size_t *value_len,
                                 uint32_t timeout_ms);
esp_err_t solar_os_ble_gatt_write(uint16_t handle,
                                  const uint8_t *value,
                                  size_t value_len,
                                  bool with_response,
                                  uint32_t timeout_ms);
