#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_ble_backend.h"
#include "solar_os_ble_bluedroid.h"

static unsigned events, opens, unregisters, discovers;
static uint16_t registered_app;
static esp_err_t register_result, unregister_result, open_result, discover_result, read_result;
static solar_os_ble_backend_event_t last;
static uint8_t copied_value[128];
static const uint8_t peer[6] = {1, 2, 3, 4, 5, 6};

void solar_os_ble_service_event(const solar_os_ble_backend_event_t *event)
{
    last = *event;
    events++;
    if (event->value_len != 0) {
        assert(event->value_len <= sizeof(copied_value));
        memcpy(copied_value, event->value, event->value_len);
    }
    if (event->type == SOLAR_OS_BLE_BACKEND_DISCOVERED && event->result == ESP_OK) {
        /* Event delivery must not hold the adapter lock. */
        solar_os_ble_gatt_service_t service = {.start_handle = 1, .end_handle = 9};
        solar_os_ble_gatt_characteristic_t characteristic;
        size_t count;
        assert(solar_os_ble_backend_characteristics(event->epoch, &service, &characteristic, 1, &count) == ESP_OK);
        assert(count == 1 && characteristic.handle == 3);
    }
}

esp_err_t esp_ble_gattc_app_register(uint16_t app_id)
{
    assert(app_id != 0 && app_id != registered_app);
    registered_app = app_id;
    return register_result;
}

esp_err_t esp_ble_gattc_app_unregister(esp_gatt_if_t interface)
{
    assert(interface == 3);
    unregisters++;
    return unregister_result;
}

esp_err_t esp_ble_gattc_enh_open(esp_gatt_if_t interface, esp_ble_gatt_creat_conn_params_t *params)
{
    assert(interface == 3 && params->is_direct && params->remote_addr_type == 1);
    assert(memcmp(params->remote_bda, peer, sizeof(peer)) == 0);
    opens++;
    return open_result;
}

esp_err_t esp_ble_gattc_send_mtu_req(esp_gatt_if_t interface, uint16_t conn_id)
{
    assert(interface == 3 && conn_id == 7);
    return ESP_OK;
}

esp_err_t esp_ble_gattc_search_service(esp_gatt_if_t interface, uint16_t conn_id, void *uuid)
{
    assert(interface == 3 && conn_id == 7 && uuid == NULL);
    discovers++;
    return discover_result;
}

esp_gatt_status_t esp_ble_gattc_get_all_char(esp_gatt_if_t interface, uint16_t conn_id,
    uint16_t start, uint16_t end, esp_gattc_char_elem_t *chars, uint16_t *count, uint16_t offset)
{
    assert(interface == 3 && conn_id == 7 && start == 1 && end == 9 && offset == 0);
    assert(*count != 0);
    chars[0] = (esp_gattc_char_elem_t){.char_handle = 3, .properties = SOLAR_OS_BLE_CHAR_READ};
    chars[0].uuid.len = ESP_UUID_LEN_16;
    chars[0].uuid.uuid.uuid16 = 0x2a19;
    *count = 1;
    return ESP_GATT_OK;
}

esp_err_t esp_ble_gattc_read_char(esp_gatt_if_t interface, uint16_t conn_id, uint16_t handle, int auth)
{
    assert(interface == 3 && conn_id == 7 && handle == 3 && auth == ESP_GATT_AUTH_REQ_NONE);
    return read_result;
}

esp_err_t esp_ble_gattc_write_char(esp_gatt_if_t interface, uint16_t conn_id, uint16_t handle,
    uint16_t len, uint8_t *value, int type, int auth)
{
    assert(interface == 3 && conn_id == 7 && handle == 3 && auth == ESP_GATT_AUTH_REQ_NONE);
    assert(len == 2 && value[0] == 0 && value[1] == 0xff && type == ESP_GATT_WRITE_TYPE_NO_RSP);
    return ESP_OK;
}

static void registered(void)
{
    esp_ble_gattc_cb_param_t param = {.reg = {.app_id = registered_app}};
    solar_os_ble_bluedroid_gatt_event(ESP_GATTC_REG_EVT, 3, &param);
}

static void opened(void)
{
    esp_ble_gattc_cb_param_t param = {.open = {.conn_id = 7, .mtu = 23}};
    memcpy(param.open.remote_bda, peer, sizeof(peer));
    solar_os_ble_bluedroid_gatt_event(ESP_GATTC_OPEN_EVT, 3, &param);
}

static void unregistered(void)
{
    solar_os_ble_bluedroid_gatt_event(ESP_GATTC_UNREG_EVT, 3, NULL);
    assert(last.type == SOLAR_OS_BLE_BACKEND_RETIRED);
}

static void connect_peer(uint32_t epoch)
{
    assert(solar_os_ble_backend_connect(epoch, epoch, peer, 1) == ESP_OK);
    registered();
    opened();
    esp_ble_gattc_cb_param_t param = {.search_cmpl = {.conn_id = 7}};
    solar_os_ble_bluedroid_gatt_event(ESP_GATTC_SEARCH_CMPL_EVT, 3, &param);
    assert(last.type == SOLAR_OS_BLE_BACKEND_DISCOVERED && last.epoch == epoch);
}

int main(void)
{
    assert(solar_os_ble_backend_register() == ESP_OK);
    /* Cancel before registration completes: no open may be issued. */
    assert(solar_os_ble_backend_connect(10, 10, peer, 1) == ESP_OK);
    const uint16_t old_app = registered_app;
    assert(solar_os_ble_backend_cancel(10) == ESP_OK);
    assert(solar_os_ble_backend_connect(11, 11, peer, 1) == ESP_ERR_INVALID_STATE);
    registered();
    assert(opens == 0 && unregisters == 1);
    unregistered();
    assert(last.epoch == 10);

    connect_peer(20);
    unsigned n = events;
    esp_ble_gattc_cb_param_t param = {.reg = {.app_id = old_app}};
    solar_os_ble_bluedroid_gatt_event(ESP_GATTC_REG_EVT, 3, &param);
    assert(events == n && opens == 1);
    /* A stale/foreign interface never reaches the service. */
    param = (esp_ble_gattc_cb_param_t){.cfg_mtu = {.conn_id = 7, .mtu = 247}};
    solar_os_ble_bluedroid_gatt_event(ESP_GATTC_CFG_MTU_EVT, 4, &param);
    assert(events == n);
    solar_os_ble_bluedroid_gatt_event(ESP_GATTC_CFG_MTU_EVT, 3, &param);
    assert(last.mtu == 247 && last.epoch == 20);

    assert(solar_os_ble_backend_read(20, 21, 3) == ESP_OK);
    const uint8_t value[2] = {0, 0xff};
    assert(solar_os_ble_backend_write(20, 22, 3, value, 2, false) == ESP_ERR_INVALID_STATE);
    uint8_t incoming[2] = {0x42, 0x43};
    param = (esp_ble_gattc_cb_param_t){.read = {.conn_id = 7, .handle = 4, .value = incoming, .value_len = 2}};
    n = events;
    solar_os_ble_bluedroid_gatt_event(ESP_GATTC_READ_CHAR_EVT, 3, &param);
    assert(events == n);
    param.read.handle = 3;
    solar_os_ble_bluedroid_gatt_event(ESP_GATTC_READ_CHAR_EVT, 3, &param);
    assert(last.request == 21 && copied_value[0] == 0x42);
    n = events;
    solar_os_ble_bluedroid_gatt_event(ESP_GATTC_READ_CHAR_EVT, 3, &param);
    assert(events == n); /* No outstanding read: discard duplicate completion. */

    assert(solar_os_ble_backend_write(20, 22, 3, value, 2, false) == ESP_OK);
    param = (esp_ble_gattc_cb_param_t){.write = {.conn_id = 7, .handle = 3}};
    solar_os_ble_bluedroid_gatt_event(ESP_GATTC_WRITE_CHAR_EVT, 3, &param);
    assert(last.type == SOLAR_OS_BLE_BACKEND_WRITTEN && last.request == 22);
    read_result = ESP_ERR_NO_MEM;
    assert(solar_os_ble_backend_read(20, 23, 3) == ESP_ERR_NO_MEM);
    read_result = ESP_OK;
    assert(solar_os_ble_backend_read(20, 24, 3) == ESP_OK);
    unregister_result = ESP_ERR_NO_MEM;
    assert(solar_os_ble_backend_cancel(20) == ESP_ERR_NO_MEM);
    assert(solar_os_ble_backend_connect(30, 30, peer, 1) == ESP_ERR_INVALID_STATE);
    unregister_result = ESP_OK;
    assert(solar_os_ble_backend_cancel(20) == ESP_OK);
    n = events;
    param = (esp_ble_gattc_cb_param_t){.read = {.conn_id = 7, .handle = 3}};
    solar_os_ble_bluedroid_gatt_event(ESP_GATTC_READ_CHAR_EVT, 3, &param);
    assert(events == n); /* Cancellation never relabels a late read. */
    unregistered();
    assert(last.epoch == 20);

    /* Cancel an already submitted OPEN, then deliver a late success. */
    assert(solar_os_ble_backend_connect(30, 30, peer, 1) == ESP_OK);
    registered();
    assert(solar_os_ble_backend_cancel(30) == ESP_OK);
    n = discovers;
    opened();
    assert(discovers == n);
    unregistered();

    connect_peer(40); /* Reuse interface and conn_id only after UNREG. */
    assert(solar_os_ble_backend_read(20, 25, 3) == ESP_ERR_INVALID_STATE);
    param = (esp_ble_gattc_cb_param_t){.close = {.conn_id = 8}};
    n = events;
    solar_os_ble_bluedroid_gatt_event(ESP_GATTC_CLOSE_EVT, 3, &param);
    assert(events == n);
    param.close.conn_id = 7;
    solar_os_ble_bluedroid_gatt_event(ESP_GATTC_CLOSE_EVT, 3, &param);
    assert(last.type == SOLAR_OS_BLE_BACKEND_CLOSED && last.epoch == 40);
    unregistered();

    register_result = ESP_ERR_NO_MEM;
    assert(solar_os_ble_backend_connect(50, 50, peer, 1) == ESP_ERR_NO_MEM);
    register_result = ESP_OK;
    open_result = ESP_ERR_NO_MEM;
    assert(solar_os_ble_backend_connect(51, 51, peer, 1) == ESP_OK);
    registered();
    assert(last.type == SOLAR_OS_BLE_BACKEND_OPENED && last.result == ESP_ERR_NO_MEM);
    unregistered();
    open_result = ESP_OK;
    discover_result = ESP_FAIL;
    assert(solar_os_ble_backend_connect(52, 52, peer, 1) == ESP_OK);
    registered();
    opened();
    assert(last.type == SOLAR_OS_BLE_BACKEND_OPENED && last.result == ESP_FAIL);
    unregistered();
    puts("Bluedroid adapter: request correlation and registration retirement OK");
    return 0;
}
