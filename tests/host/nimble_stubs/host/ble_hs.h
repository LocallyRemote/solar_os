#pragma once
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 4
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define BLE_HS_EALREADY 2
#define BLE_HS_EINVAL 3
#define BLE_HS_EMSGSIZE 4
#define BLE_HS_ENOENT 5
#define BLE_HS_ENOMEM 6
#define BLE_HS_ENOTCONN 7
#define BLE_HS_EAPP 9
#define BLE_HS_EBUSY 15
#define BLE_HS_EDONE 14
#define BLE_HS_ETIMEOUT 13
#define BLE_HS_ENOTSYNCED 22
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_OWN_ADDR_PUBLIC 0
#define BLE_UUID_TYPE_16 16
#define BLE_UUID_TYPE_32 32
#define BLE_UUID_TYPE_128 128
#define BLE_GATT_CHR_PROP_NOTIFY 0x10
#define BLE_GATT_CHR_PROP_INDICATE 0x20
typedef struct { uint8_t type; uint8_t val[6]; } ble_addr_t;
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
typedef struct { ble_uuid_t u; uint32_t value; } ble_uuid32_t;
typedef union { ble_uuid_t u; ble_uuid16_t u16; ble_uuid32_t u32; uint8_t raw[20]; } ble_uuid_any_t;
#define BLE_UUID16(u) ((const ble_uuid16_t *)(u))
#define BLE_UUID32(u) ((const ble_uuid32_t *)(u))
static inline uint16_t ble_uuid_u16(const ble_uuid_t *u) { return u->type == 16 ? BLE_UUID16(u)->value : 0; }
char *ble_uuid_to_str(const ble_uuid_t *uuid, char *out);
struct ble_gatt_svc { uint16_t start_handle, end_handle; ble_uuid_any_t uuid; };
struct ble_gatt_chr { uint16_t def_handle, val_handle; uint8_t properties; ble_uuid_any_t uuid; };
struct ble_gatt_dsc { uint16_t handle; ble_uuid_any_t uuid; };
struct ble_gatt_error { int status; };
struct os_mbuf { size_t len; uint8_t *data; struct os_mbuf *next; };
size_t nimble_test_mbuf_len(const struct os_mbuf *om);
#define OS_MBUF_PKTLEN(om) nimble_test_mbuf_len(om)
int os_mbuf_copydata(const struct os_mbuf *om, int offset, int len, void *out);
struct ble_gatt_attr { uint16_t handle; struct os_mbuf *om; };
struct ble_gap_conn_desc { ble_addr_t peer_id_addr; };
int ble_gap_conn_find(uint16_t conn, struct ble_gap_conn_desc *desc);
enum { BLE_GAP_EVENT_CONNECT, BLE_GAP_EVENT_DISCONNECT, BLE_GAP_EVENT_ENC_CHANGE, BLE_GAP_EVENT_NOTIFY_RX };
struct ble_gap_event {
    int type;
    union {
        struct { int status; uint16_t conn_handle; } connect;
        struct { int reason; struct { uint16_t conn_handle; } conn; } disconnect;
        struct { int status; uint16_t conn_handle; } enc_change;
        struct { uint16_t conn_handle, attr_handle; struct os_mbuf *om; bool indication; } notify_rx;
    };
};
typedef int ble_gap_event_fn(struct ble_gap_event *, void *);
typedef int ble_gatt_disc_svc_fn(uint16_t, const struct ble_gatt_error *, const struct ble_gatt_svc *, void *);
typedef int ble_gatt_chr_fn(uint16_t, const struct ble_gatt_error *, const struct ble_gatt_chr *, void *);
typedef int ble_gatt_dsc_fn(uint16_t, const struct ble_gatt_error *, uint16_t, const struct ble_gatt_dsc *, void *);
typedef int ble_gatt_attr_fn(uint16_t, const struct ble_gatt_error *, struct ble_gatt_attr *, void *);
typedef int ble_gatt_mtu_fn(uint16_t, const struct ble_gatt_error *, uint16_t, void *);
int ble_gap_connect(uint8_t, const ble_addr_t *, int, const void *, ble_gap_event_fn *, void *);
int ble_gap_conn_cancel(void);
int ble_gap_terminate(uint16_t, uint8_t);
int ble_gap_security_initiate(uint16_t);
int ble_gattc_disc_all_svcs(uint16_t, ble_gatt_disc_svc_fn *, void *);
int ble_gattc_find_inc_svcs(uint16_t, uint16_t, uint16_t, ble_gatt_disc_svc_fn *, void *);
int ble_gattc_disc_all_chrs(uint16_t, uint16_t, uint16_t, ble_gatt_chr_fn *, void *);
int ble_gattc_disc_all_dscs(uint16_t, uint16_t, uint16_t, ble_gatt_dsc_fn *, void *);
int ble_gattc_exchange_mtu(uint16_t, ble_gatt_mtu_fn *, void *);
uint16_t ble_att_mtu(uint16_t);
int ble_gattc_read(uint16_t, uint16_t, ble_gatt_attr_fn *, void *);
int ble_gattc_read_long(uint16_t, uint16_t, uint16_t, ble_gatt_attr_fn *, void *);
int ble_gattc_write_flat(uint16_t, uint16_t, const void *, uint16_t, ble_gatt_attr_fn *, void *);
int ble_gattc_write_no_rsp_flat(uint16_t, uint16_t, const void *, uint16_t);
