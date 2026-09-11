#pragma once

/* Minimal IDF surface for running the real adapter with a controlled event
 * stream. Firmware builds also compile it against the installed IDF headers. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef uint8_t esp_gatt_if_t;
typedef uint16_t esp_gatt_status_t;
typedef uint8_t esp_ble_addr_type_t;
#define ESP_GATT_IF_NONE 0xff
#define ESP_GATT_OK 0
#define ESP_UUID_LEN_16 2
#define ESP_UUID_LEN_32 4
#define ESP_UUID_LEN_128 16
#define BLE_ADDR_TYPE_PUBLIC 0
#define ESP_GATT_AUTH_REQ_NONE 0
#define ESP_GATT_WRITE_TYPE_RSP 1
#define ESP_GATT_WRITE_TYPE_NO_RSP 2

typedef enum {
    ESP_GATTC_REG_EVT, ESP_GATTC_UNREG_EVT, ESP_GATTC_OPEN_EVT,
    ESP_GATTC_CFG_MTU_EVT, ESP_GATTC_SEARCH_RES_EVT, ESP_GATTC_SEARCH_CMPL_EVT,
    ESP_GATTC_READ_CHAR_EVT, ESP_GATTC_WRITE_CHAR_EVT,
    ESP_GATTC_CLOSE_EVT, ESP_GATTC_DISCONNECT_EVT,
} esp_gattc_cb_event_t;

typedef struct {
    uint16_t len;
    union { uint16_t uuid16; uint32_t uuid32; uint8_t uuid128[16]; } uuid;
} esp_bt_uuid_t;

typedef union {
    struct { uint16_t status, app_id; } reg;
    struct { uint16_t status, conn_id, mtu; uint8_t remote_bda[6]; } open;
    struct { uint16_t status, conn_id, mtu; } cfg_mtu;
    struct {
        uint16_t conn_id, start_handle, end_handle;
        bool is_primary;
        struct { esp_bt_uuid_t uuid; } srvc_id;
    } search_res;
    struct { uint16_t status, conn_id; } search_cmpl;
    struct { uint16_t status, conn_id, handle, value_len; uint8_t *value; } read;
    struct { uint16_t status, conn_id, handle; } write;
    struct { uint16_t conn_id, reason; } close;
    struct { uint16_t conn_id, reason; } disconnect;
} esp_ble_gattc_cb_param_t;

typedef struct {
    uint8_t remote_bda[6];
    esp_ble_addr_type_t remote_addr_type, own_addr_type;
    bool is_direct;
} esp_ble_gatt_creat_conn_params_t;

typedef struct { uint16_t char_handle; uint8_t properties; esp_bt_uuid_t uuid; } esp_gattc_char_elem_t;

esp_err_t esp_ble_gattc_app_register(uint16_t app_id);
esp_err_t esp_ble_gattc_app_unregister(esp_gatt_if_t interface);
esp_err_t esp_ble_gattc_enh_open(esp_gatt_if_t interface, esp_ble_gatt_creat_conn_params_t *params);
esp_err_t esp_ble_gattc_send_mtu_req(esp_gatt_if_t interface, uint16_t conn_id);
esp_err_t esp_ble_gattc_search_service(esp_gatt_if_t interface, uint16_t conn_id, void *uuid);
esp_gatt_status_t esp_ble_gattc_get_all_char(esp_gatt_if_t interface, uint16_t conn_id,
    uint16_t start, uint16_t end, esp_gattc_char_elem_t *chars, uint16_t *count, uint16_t offset);
esp_err_t esp_ble_gattc_read_char(esp_gatt_if_t interface, uint16_t conn_id, uint16_t handle, int auth);
esp_err_t esp_ble_gattc_write_char(esp_gatt_if_t interface, uint16_t conn_id, uint16_t handle,
    uint16_t len, uint8_t *value, int type, int auth);
