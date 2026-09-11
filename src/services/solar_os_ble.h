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

/* Lifecycle calls retain the current boot policy and the OS keyboard profile.
 * Scan results include keyboard hints for compatibility with the shell.
 * Callers must serialize lifecycle transitions and GATT use. */
esp_err_t solar_os_ble_init(void);
esp_err_t solar_os_ble_scan(solar_os_ble_scan_result_t *results, size_t max_results, size_t *found);
esp_err_t solar_os_ble_prepare_sleep(uint32_t timeout_ms);
bool solar_os_ble_sleep_prepare_ready(void);
void solar_os_ble_resume(void);

/* Compatibility client: one shared connection, one operation at a time.
 * conn_id is a diagnostic transport identifier, not an app-owned handle.
 * UUIDs are formatted as 0xNNNN, 0xNNNNNNNN, or canonical 128-bit strings.
 * These synchronous calls must run outside the Bluetooth callback task.
 * A zero timeout selects the existing service default. */
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
