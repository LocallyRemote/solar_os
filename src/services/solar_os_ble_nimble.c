#include "solar_os_ble_nimble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"

typedef enum { OP_NONE, OP_CONNECT, OP_READ, OP_WRITE } operation_t;
typedef struct {
    uint16_t start, end;
    size_t count;
    solar_os_ble_gatt_characteristic_t *chars;
} service_cache_t;

static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
static struct ble_npl_event command_event;
static bool command_ready;
static void command_callback(struct ble_npl_event *event);
static struct {
    uint32_t epoch, request;
    uint16_t conn, handle;
    uint8_t bda[6];
    operation_t op;
    bool retiring, connecting, response;
    uint8_t addr_type;
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t value_len;
    size_t count, discovering;
    size_t including;
    service_cache_t services[SOLAR_OS_BLE_GATT_MAX_SERVICES];
} client;

static void lock(void) { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(mutex); }

esp_err_t solar_os_ble_nimble_error(int status)
{
    switch (status) {
    case 0: return ESP_OK;
    case BLE_HS_ENOMEM: return ESP_ERR_NO_MEM;
    case BLE_HS_EINVAL: return ESP_ERR_INVALID_ARG;
    case BLE_HS_ETIMEOUT: return ESP_ERR_TIMEOUT;
    case BLE_HS_EBUSY:
    case BLE_HS_EALREADY:
    case BLE_HS_ENOTCONN:
    case BLE_HS_ENOTSYNCED: return ESP_ERR_INVALID_STATE;
    default: return ESP_FAIL;
    }
}

void solar_os_ble_nimble_address(ble_addr_t *out, const uint8_t bda[6], uint8_t type)
{
    out->type = type;
    for (size_t i = 0; i < 6; ++i) out->val[i] = bda[5 - i];
}

void solar_os_ble_nimble_display_address(uint8_t out[6], const ble_addr_t *addr)
{
    for (size_t i = 0; i < 6; ++i) out[i] = addr->val[5 - i];
}

static void uuid_string(const ble_uuid_t *uuid, char *out, size_t size)
{
    if (uuid->type == BLE_UUID_TYPE_16) {
        snprintf(out, size, "0x%04x", BLE_UUID16(uuid)->value);
    } else if (uuid->type == BLE_UUID_TYPE_32) {
        snprintf(out, size, "0x%08lx", (unsigned long)BLE_UUID32(uuid)->value);
    } else {
        ble_uuid_to_str(uuid, out);
    }
}

esp_err_t solar_os_ble_backend_register(void)
{
    portENTER_CRITICAL(&init_lock);
    if (!mutex) {
        mutex = xSemaphoreCreateRecursiveMutexStatic(&mutex_storage);
        client.conn = BLE_HS_CONN_HANDLE_NONE;
    }
    portEXIT_CRITICAL(&init_lock);
    lock();
    if (!command_ready) {
        ble_npl_event_init(&command_event, command_callback, NULL);
        command_ready = true;
    }
    unlock();
    return ESP_OK;
}

static void clear_locked(void)
{
    for (size_t i = 0; i < client.count; ++i) free(client.services[i].chars);
    memset(&client, 0, sizeof(client));
    client.conn = BLE_HS_CONN_HANDLE_NONE;
}

void solar_os_ble_backend_reset(void)
{
    if (!mutex) return;
    lock();
    clear_locked(); /* Only after host shutdown has drained callbacks. */
    unlock();
}

bool solar_os_ble_nimble_client_idle(void)
{
    if (!mutex) return true;
    lock();
    bool idle = client.epoch == 0;
    unlock();
    return idle;
}

void solar_os_ble_nimble_host_stopped(void)
{
    if (command_ready) {
        ble_npl_event_deinit(&command_event);
        command_ready = false;
    }
}

static bool matches(uint16_t conn, void *arg)
{
    return client.epoch && client.epoch == (uint32_t)(uintptr_t)arg &&
        client.conn == conn && !client.retiring;
}

static solar_os_ble_backend_event_t event_locked(solar_os_ble_backend_event_type_t type, int status)
{
    solar_os_ble_backend_event_t e = {
        .type = type, .epoch = client.epoch, .request = client.request,
        .conn_id = client.conn, .handle = client.handle,
        .status = status, .result = solar_os_ble_nimble_error(status),
    };
    memcpy(e.bda, client.bda, sizeof(e.bda));
    return e;
}

static void retire_locked(void)
{
    client.retiring = true;
    if (client.conn != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(client.conn, BLE_ERR_REM_USER_CONN_TERM);
    } else if (client.connecting) {
        (void)ble_gap_conn_cancel();
    } else {
        solar_os_ble_backend_event_t e = event_locked(SOLAR_OS_BLE_BACKEND_RETIRED, 0);
        clear_locked();
        solar_os_ble_service_event(&e);
    }
}

esp_err_t solar_os_ble_backend_cancel(uint32_t epoch)
{
    lock();
    if (client.epoch == epoch && epoch) {
        client.retiring = true;
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    }
    unlock();
    return ESP_OK;
}

static int chars_callback(uint16_t conn, const struct ble_gatt_error *error,
                          const struct ble_gatt_chr *chr, void *arg);

static int discover_next_locked(void *arg)
{
    service_cache_t *s = &client.services[client.discovering];
    s->chars = calloc(SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS, sizeof(*s->chars));
    if (!s->chars) return BLE_HS_ENOMEM;
    return ble_gattc_disc_all_chrs(client.conn, s->start, s->end, chars_callback, arg);
}

static int included_callback(uint16_t conn, const struct ble_gatt_error *error,
    const struct ble_gatt_svc *svc, void *arg);

static int discover_included_locked(void *arg)
{
    service_cache_t *s = &client.services[client.including];
    return ble_gattc_find_inc_svcs(client.conn, s->start, s->end, included_callback, arg);
}

static int included_callback(uint16_t conn, const struct ble_gatt_error *error,
    const struct ble_gatt_svc *svc, void *arg)
{
    lock();
    if (!matches(conn, arg)) { unlock(); return 0; }
    int rc = error->status;
    if (!rc) {
        for (size_t i = 0; i < client.count; ++i) {
            if (client.services[i].start == svc->start_handle) { unlock(); return 0; }
        }
        if (client.count < SOLAR_OS_BLE_GATT_MAX_SERVICES) {
            service_cache_t *s = &client.services[client.count++];
            s->start = svc->start_handle; s->end = svc->end_handle;
            solar_os_ble_backend_event_t e = event_locked(SOLAR_OS_BLE_BACKEND_SERVICE, 0);
            e.service.start_handle = s->start; e.service.end_handle = s->end;
            e.service.primary = false;
            uuid_string(&svc->uuid.u, e.service.uuid, sizeof(e.service.uuid));
            unlock();
            solar_os_ble_service_event(&e);
            return 0;
        }
        rc = BLE_HS_ENOMEM;
    } else if (rc == BLE_HS_EDONE) {
        rc = ++client.including < client.count ? discover_included_locked(arg) : discover_next_locked(arg);
        if (!rc) { unlock(); return 0; }
    }
    solar_os_ble_backend_event_t e = event_locked(SOLAR_OS_BLE_BACKEND_DISCOVERED, rc);
    retire_locked();
    unlock();
    solar_os_ble_service_event(&e);
    return rc;
}

static int chars_callback(uint16_t conn, const struct ble_gatt_error *error,
                          const struct ble_gatt_chr *chr, void *arg)
{
    lock();
    if (!matches(conn, arg)) { unlock(); return 0; }
    int rc = error->status;
    if (!rc) {
        service_cache_t *s = &client.services[client.discovering];
        if (s->count < SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS) {
            solar_os_ble_gatt_characteristic_t *c = &s->chars[s->count++];
            c->handle = chr->val_handle;
            c->properties = chr->properties;
            uuid_string(&chr->uuid.u, c->uuid, sizeof(c->uuid));
        } else rc = BLE_HS_ENOMEM;
    } else if (rc == BLE_HS_EDONE) {
        service_cache_t *s = &client.services[client.discovering];
        if (!s->count) {
            free(s->chars);
            s->chars = NULL;
        } else {
            void *compact = realloc(s->chars, s->count * sizeof(*s->chars));
            if (compact) s->chars = compact;
        }
        if (++client.discovering < client.count) {
            rc = discover_next_locked(arg);
            if (!rc) { unlock(); return 0; }
        } else rc = 0;
        solar_os_ble_backend_event_t e = event_locked(SOLAR_OS_BLE_BACKEND_DISCOVERED, rc);
        client.op = OP_NONE;
        client.request = 0;
        if (rc) retire_locked();
        unlock();
        solar_os_ble_service_event(&e);
        return 0;
    }
    if (rc) {
        solar_os_ble_backend_event_t e = event_locked(SOLAR_OS_BLE_BACKEND_DISCOVERED, rc);
        retire_locked();
        unlock();
        solar_os_ble_service_event(&e);
        return rc;
    }
    unlock();
    return 0;
}

static int services_callback(uint16_t conn, const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *svc, void *arg)
{
    lock();
    if (!matches(conn, arg)) { unlock(); return 0; }
    int rc = error->status;
    solar_os_ble_backend_event_t e = event_locked(SOLAR_OS_BLE_BACKEND_SERVICE, 0);
    if (!rc && client.count < SOLAR_OS_BLE_GATT_MAX_SERVICES) {
        service_cache_t *s = &client.services[client.count++];
        s->start = svc->start_handle;
        s->end = svc->end_handle;
        e.service.start_handle = s->start;
        e.service.end_handle = s->end;
        e.service.primary = true;
        uuid_string(&svc->uuid.u, e.service.uuid, sizeof(e.service.uuid));
        unlock();
        solar_os_ble_service_event(&e);
        return 0;
    }
    if (rc == BLE_HS_EDONE) {
        rc = client.count ? discover_included_locked(arg) : BLE_HS_ENOENT;
        if (!rc) { unlock(); return 0; }
    } else if (!rc) rc = BLE_HS_ENOMEM;
    e = event_locked(SOLAR_OS_BLE_BACKEND_DISCOVERED, rc);
    retire_locked();
    unlock();
    solar_os_ble_service_event(&e);
    return rc;
}

static int mtu_callback(uint16_t conn, const struct ble_gatt_error *error,
                        uint16_t mtu, void *arg)
{
    lock();
    if (!matches(conn, arg)) { unlock(); return 0; }
    solar_os_ble_backend_event_t e = event_locked(SOLAR_OS_BLE_BACKEND_MTU, error->status);
    e.mtu = ble_att_mtu(conn);
    unlock();
    solar_os_ble_service_event(&e);
    lock();
    if (!matches(conn, arg)) { unlock(); return 0; }
    int rc = ble_gattc_disc_all_svcs(conn, services_callback, arg);
    if (rc) {
        e = event_locked(SOLAR_OS_BLE_BACKEND_DISCOVERED, rc);
        retire_locked();
    }
    unlock();
    if (rc) solar_os_ble_service_event(&e);
    return 0;
}

static int gap_callback(struct ble_gap_event *event, void *arg)
{
    lock();
    if (!client.epoch || client.epoch != (uint32_t)(uintptr_t)arg) { unlock(); return 0; }
    solar_os_ble_backend_event_t e;
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        client.connecting = false;
        int rc = event->connect.status;
        if (!rc) client.conn = event->connect.conn_handle;
        if (client.retiring && !rc) { retire_locked(); unlock(); return 0; }
        e = event_locked(SOLAR_OS_BLE_BACKEND_OPENED, rc);
        e.mtu = rc ? 23 : ble_att_mtu(client.conn);
        if (rc) {
            uint32_t epoch = client.epoch;
            clear_locked();
            unlock();
            solar_os_ble_service_event(&e);
            e.type = SOLAR_OS_BLE_BACKEND_RETIRED;
            e.epoch = epoch;
            solar_os_ble_service_event(&e);
            return 0;
        }
        unlock();
        solar_os_ble_service_event(&e);
        lock();
        if (matches(event->connect.conn_handle, arg)) {
            rc = ble_gattc_exchange_mtu(client.conn, mtu_callback, arg);
            if (rc == BLE_HS_EALREADY) {
                struct ble_gatt_error complete = {.status = 0};
                (void)mtu_callback(client.conn, &complete, ble_att_mtu(client.conn), arg);
                rc = 0;
            }
            if (rc) {
                e = event_locked(SOLAR_OS_BLE_BACKEND_DISCOVERED, rc);
                retire_locked();
            }
        }
        unlock();
        if (rc) solar_os_ble_service_event(&e);
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        e = event_locked(SOLAR_OS_BLE_BACKEND_CLOSED, 0);
        e.reason = event->disconnect.reason;
        clear_locked();
        unlock();
        solar_os_ble_service_event(&e);
        /* NimBLE aborts outstanding ATT procedures before GAP disconnect. All
         * callbacks additionally carry the immutable epoch, never a slot ptr. */
        e.type = SOLAR_OS_BLE_BACKEND_RETIRED;
        solar_os_ble_service_event(&e);
        return 0;
    }
    unlock();
    return solar_os_ble_nimble_security(event);
}

esp_err_t solar_os_ble_backend_connect(uint32_t epoch, uint32_t request,
                                       const uint8_t bda[6], uint8_t type)
{
    lock();
    if (client.epoch) { unlock(); return ESP_ERR_INVALID_STATE; }
    clear_locked();
    client.epoch = epoch;
    client.request = request;
    client.op = OP_CONNECT;
    memcpy(client.bda, bda, sizeof(client.bda));
    client.addr_type = type;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    unlock();
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_characteristics(uint32_t epoch,
    const solar_os_ble_gatt_service_t *service,
    solar_os_ble_gatt_characteristic_t *chars, size_t max, size_t *count)
{
    lock();
    if (client.epoch != epoch || client.retiring || client.op != OP_NONE) {
        unlock(); return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < client.count; ++i) {
        service_cache_t *s = &client.services[i];
        if (s->start == service->start_handle && s->end == service->end_handle) {
            size_t n = s->count < max ? s->count : max;
            if (n) memcpy(chars, s->chars, n * sizeof(*chars));
            if (count) *count = n;
            unlock(); return ESP_OK;
        }
    }
    unlock();
    return ESP_ERR_NOT_FOUND;
}

static int value_callback(uint16_t conn, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    lock();
    if (!client.epoch || client.retiring || client.conn != conn ||
        client.request != (uint32_t)(uintptr_t)arg ||
        (client.op != OP_READ && client.op != OP_WRITE)) {
        unlock(); return 0;
    }
    solar_os_ble_backend_event_t e = event_locked(client.op == OP_READ ?
        SOLAR_OS_BLE_BACKEND_READ : SOLAR_OS_BLE_BACKEND_WRITTEN, error->status);
    if (!error->status && client.op == OP_READ && attr && attr->om) {
        size_t n = OS_MBUF_PKTLEN(attr->om);
        if (n > sizeof(value)) n = sizeof(value);
        if (os_mbuf_copydata(attr->om, 0, n, value) == 0) {
            e.value = value; e.value_len = n;
        } else e.result = ESP_FAIL;
    }
    client.op = OP_NONE;
    client.request = 0;
    unlock();
    solar_os_ble_service_event(&e);
    return 0;
}

static bool ready(uint32_t epoch)
{
    return epoch && client.epoch == epoch && !client.retiring &&
        client.conn != BLE_HS_CONN_HANDLE_NONE && client.op == OP_NONE;
}

esp_err_t solar_os_ble_backend_read(uint32_t epoch, uint32_t request, uint16_t handle)
{
    lock();
    if (!ready(epoch)) { unlock(); return ESP_ERR_INVALID_STATE; }
    client.op = OP_READ; client.request = request; client.handle = handle;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    unlock();
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_write(uint32_t epoch, uint32_t request, uint16_t handle,
    const uint8_t *value, size_t len, bool response)
{
    lock();
    if (!ready(epoch)) { unlock(); return ESP_ERR_INVALID_STATE; }
    if (len > sizeof(client.value)) { unlock(); return ESP_ERR_INVALID_SIZE; }
    client.op = OP_WRITE; client.request = request; client.handle = handle;
    memcpy(client.value, value, len);
    client.value_len = len;
    client.response = response;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    unlock();
    return ESP_OK;
}

/* Submission never acquires NimBLE's host lock from an application task while
 * holding our mutex. All stack calls run on the host queue, avoiding AB/BA
 * deadlocks with a callback arriving during cancel/read/write. */
static void command_callback(struct ble_npl_event *event)
{
    lock();
    if (!client.epoch) { unlock(); return; }
    if (client.retiring) { retire_locked(); unlock(); return; }
    int rc = 0;
    void *arg = (void *)(uintptr_t)client.epoch;
    solar_os_ble_backend_event_type_t type;
    if (client.op == OP_CONNECT) {
        if (client.connecting || client.conn != BLE_HS_CONN_HANDLE_NONE) { unlock(); return; }
        ble_addr_t addr;
        solar_os_ble_nimble_address(&addr, client.bda, client.addr_type);
        rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 3000, NULL, gap_callback, arg);
        client.connecting = rc == 0;
        if (!rc) { unlock(); return; }
        solar_os_ble_backend_event_t e = event_locked(SOLAR_OS_BLE_BACKEND_OPENED, rc);
        clear_locked();
        unlock();
        solar_os_ble_service_event(&e);
        e.type = SOLAR_OS_BLE_BACKEND_RETIRED;
        solar_os_ble_service_event(&e);
        return;
    } else if (client.op == OP_READ) {
        type = SOLAR_OS_BLE_BACKEND_READ;
        rc = ble_gattc_read(client.conn, client.handle, value_callback,
                            (void *)(uintptr_t)client.request);
        if (!rc) { unlock(); return; }
    } else if (client.op == OP_WRITE) {
        type = SOLAR_OS_BLE_BACKEND_WRITTEN;
        if (client.value_len > (size_t)(ble_att_mtu(client.conn) - 3)) rc = BLE_HS_EMSGSIZE;
        else if (client.response) rc = ble_gattc_write_flat(client.conn, client.handle,
            client.value, client.value_len, value_callback, (void *)(uintptr_t)client.request);
        else rc = ble_gattc_write_no_rsp_flat(client.conn, client.handle, client.value, client.value_len);
        if (!rc && client.response) { unlock(); return; }
    } else { unlock(); return; }
    solar_os_ble_backend_event_t e = event_locked(type, rc);
    if (rc == BLE_HS_EMSGSIZE) e.result = ESP_ERR_INVALID_SIZE;
    client.op = OP_NONE;
    client.request = 0;
    unlock();
    solar_os_ble_service_event(&e);
}
