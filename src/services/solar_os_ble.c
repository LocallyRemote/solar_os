#include "solar_os_ble.h"
#include "solar_os_ble_backend.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BLE_CONNECT_TIMEOUT_MS 12000U
#define BLE_OPERATION_TIMEOUT_MS 5000U
#define BLE_SLOT_COUNT (SOLAR_OS_BLE_SESSION_MAX + 1)

typedef enum { BLE_OP_NONE, BLE_OP_CONNECT, BLE_OP_READ, BLE_OP_WRITE } ble_operation_t;

typedef struct {
    solar_os_ble_session_t id;
    char owner[SOLAR_OS_BLE_OWNER_MAX];
    bool closing;
    bool busy; /* Pins this slot and its result until the calling task returns. */
    bool pending;
    uint32_t request;
    ble_operation_t op;
    uint16_t handle;
    esp_err_t result;
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t value_len;
    StaticSemaphore_t wake_storage;
    SemaphoreHandle_t wake;
} ble_session_t;

static ble_session_t sessions[BLE_SLOT_COUNT];
static StaticSemaphore_t state_storage, dispatch_storage;
static SemaphoreHandle_t state_mutex, dispatch_mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t ticket;
static bool online;
static bool sleeping;
static struct {
    uint32_t epoch;
    solar_os_ble_session_t owner;
    bool retiring;
    solar_os_ble_gatt_status_t info;
    solar_os_ble_gatt_service_t services[SOLAR_OS_BLE_GATT_MAX_SERVICES];
} link_state;

esp_err_t solar_os_ble_service_prepare_runtime(void)
{
    portENTER_CRITICAL(&init_lock);
    if (state_mutex == NULL) {
        state_mutex = xSemaphoreCreateMutexStatic(&state_storage);
        dispatch_mutex = xSemaphoreCreateMutexStatic(&dispatch_storage);
        for (size_t i = 0; i < BLE_SLOT_COUNT; i++) {
            sessions[i].wake = xSemaphoreCreateBinaryStatic(&sessions[i].wake_storage);
        }
        link_state.info.conn_id = SOLAR_OS_BLE_CONNECTION_INVALID;
        strlcpy(link_state.info.status, "idle", sizeof(link_state.info.status));
    }
    portEXIT_CRITICAL(&init_lock);
    return ESP_OK;
}

static void lock_state(void) { xSemaphoreTake(state_mutex, portMAX_DELAY); }
static void unlock_state(void) { xSemaphoreGive(state_mutex); }
static void lock_dispatch(void)
{
    solar_os_ble_service_prepare_runtime();
    xSemaphoreTake(dispatch_mutex, portMAX_DELAY);
}
static void unlock_dispatch(void) { xSemaphoreGive(dispatch_mutex); }

/* Never wrap: exhaustion fails closed instead of aliasing a historical handle. */
static uint32_t next_ticket_locked(void)
{
    return ticket == UINT32_MAX ? 0 : ++ticket;
}

static ble_session_t *find_locked(solar_os_ble_session_t id)
{
    if (id != SOLAR_OS_BLE_SESSION_INVALID) {
        for (size_t i = 0; i < BLE_SLOT_COUNT; i++) {
            if (sessions[i].id == id) {
                return &sessions[i];
            }
        }
    }
    return NULL;
}

static ble_session_t *live_locked(solar_os_ble_session_t id)
{
    ble_session_t *s = find_locked(id);
    return s != NULL && !s->closing ? s : NULL;
}

static void finish_locked(ble_session_t *s, esp_err_t result)
{
    if (s != NULL && s->busy) {
        s->pending = false;
        s->result = result;
        xSemaphoreGive(s->wake);
    }
}

static void clear_link_locked(const char *status)
{
    memset(&link_state, 0, sizeof(link_state));
    link_state.info.conn_id = SOLAR_OS_BLE_CONNECTION_INVALID;
    strlcpy(link_state.info.status, status, sizeof(link_state.info.status));
}

void solar_os_ble_service_reset(const char *status)
{
    solar_os_ble_service_prepare_runtime();
    lock_state();
    online = false;
    for (size_t i = 0; i < BLE_SLOT_COUNT; i++) {
        finish_locked(&sessions[i], SOLAR_OS_BLE_ERR_CANCELLED);
    }
    clear_link_locked(status != NULL ? status : "idle");
    unlock_state();
}

esp_err_t solar_os_ble_service_register(void)
{
    const esp_err_t ret = solar_os_ble_backend_register();
    lock_state();
    online = ret == ESP_OK;
    unlock_state();
    return ret;
}

esp_err_t solar_os_ble_init(void)
{
    lock_dispatch();
    lock_state();
    const bool blocked = sleeping;
    unlock_state();
    const esp_err_t ret = blocked ? ESP_ERR_INVALID_STATE : solar_os_ble_backend_init();
    unlock_dispatch();
    return ret;
}

esp_err_t solar_os_ble_scan(solar_os_ble_scan_result_t *results, size_t max_results, size_t *found)
{
    lock_dispatch();
    lock_state();
    /* The legacy discovery scan blocks for seconds. Do not let it delay
     * cancellation of a live generic client's request. */
    const bool blocked = sleeping || link_state.epoch != 0;
    unlock_state();
    if (found != NULL) {
        *found = 0;
    }
    const esp_err_t ret = blocked ? ESP_ERR_INVALID_STATE :
        solar_os_ble_backend_scan(results, max_results, found);
    unlock_dispatch();
    return ret;
}

/* Caller holds dispatch. Wake a waiter before submitting asynchronous teardown. */
static uint32_t retire_locked(solar_os_ble_session_t owner, esp_err_t result)
{
    finish_locked(find_locked(owner), result);
    if (link_state.epoch == 0 || link_state.owner != owner) {
        return 0;
    }
    link_state.retiring = true;
    link_state.info.connected = false;
    link_state.info.service_count = 0;
    strlcpy(link_state.info.status, "retiring", sizeof(link_state.info.status));
    return link_state.epoch;
}

esp_err_t solar_os_ble_prepare_sleep(uint32_t timeout_ms)
{
    lock_dispatch();
    lock_state();
    sleeping = true;
    const uint32_t epoch = retire_locked(link_state.owner, SOLAR_OS_BLE_ERR_CANCELLED);
    unlock_state();
    if (epoch != 0) {
        (void)solar_os_ble_backend_cancel(epoch);
    }
    const esp_err_t ret = solar_os_ble_backend_prepare_sleep(timeout_ms);
    lock_state();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) {
        sleeping = false;
    }
    unlock_state();
    unlock_dispatch();
    return ret;
}

bool solar_os_ble_sleep_prepare_ready(void)
{
    lock_dispatch();
    const bool ready = solar_os_ble_backend_sleep_prepare_ready();
    unlock_dispatch();
    return ready;
}

void solar_os_ble_resume(void)
{
    lock_dispatch();
    lock_state();
    sleeping = false;
    unlock_state();
    solar_os_ble_backend_resume();
    unlock_dispatch();
}

static esp_err_t create_locked(size_t first, size_t end, const char *owner,
                               solar_os_ble_session_t *id)
{
    for (size_t i = first; i < end; i++) {
        ble_session_t *s = &sessions[i];
        if (s->id == 0 && !s->busy) {
            const uint32_t handle = next_ticket_locked();
            if (handle == 0) {
                return ESP_ERR_NO_MEM;
            }
            s->id = handle;
            s->closing = false;
            s->pending = false;
            s->op = BLE_OP_NONE;
            strlcpy(s->owner, owner, sizeof(s->owner));
            *id = handle;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t solar_os_ble_session_create(const char *owner, solar_os_ble_session_t *session)
{
    if (session != NULL) {
        *session = SOLAR_OS_BLE_SESSION_INVALID;
    }
    if (owner == NULL || owner[0] == '\0' || strlen(owner) >= SOLAR_OS_BLE_OWNER_MAX ||
        session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_ble_service_prepare_runtime();
    lock_state();
    const esp_err_t ret = create_locked(1, BLE_SLOT_COUNT, owner, session);
    unlock_state();
    return ret;
}

static esp_err_t cancel_session(solar_os_ble_session_t id, bool close)
{
    lock_dispatch();
    lock_state();
    ble_session_t *s = live_locked(id);
    if (s == NULL) {
        unlock_state();
        unlock_dispatch();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t epoch = retire_locked(id, SOLAR_OS_BLE_ERR_CANCELLED);
    if (close) {
        s->closing = true;
        if (!s->busy) {
            s->id = 0;
        }
    }
    unlock_state();
    const esp_err_t ret = epoch == 0 ? ESP_OK : solar_os_ble_backend_cancel(epoch);
    unlock_dispatch();
    return ret;
}

esp_err_t solar_os_ble_session_cancel(solar_os_ble_session_t session)
{
    return cancel_session(session, false);
}

esp_err_t solar_os_ble_session_close(solar_os_ble_session_t session)
{
    return cancel_session(session, true);
}

esp_err_t solar_os_ble_session_get_info(solar_os_ble_session_t session,
                                      solar_os_ble_session_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_ble_service_prepare_runtime();
    lock_state();
    ble_session_t *s = live_locked(session);
    if (s == NULL) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    memset(info, 0, sizeof(*info));
    strlcpy(info->owner, s->owner, sizeof(info->owner));
    info->busy = s->busy;
    info->gatt.conn_id = SOLAR_OS_BLE_CONNECTION_INVALID;
    strlcpy(info->gatt.status, online ? "idle" : "sleep", sizeof(info->gatt.status));
    if (link_state.owner == session) {
        info->retiring = link_state.retiring;
        info->gatt = link_state.info;
    }
    unlock_state();
    return ESP_OK;
}

static esp_err_t begin_locked(ble_session_t *s, ble_operation_t op, uint16_t handle)
{
    if (s == NULL || s->busy || sleeping || !online) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t request = next_ticket_locked();
    if (request == 0) {
        return ESP_ERR_NO_MEM;
    }
    while (xSemaphoreTake(s->wake, 0) == pdTRUE) {}
    s->busy = true;
    s->pending = true;
    s->request = request;
    s->op = op;
    s->handle = handle;
    s->result = ESP_OK;
    s->value_len = 0;
    return ESP_OK;
}

static esp_err_t wait_operation(solar_os_ble_session_t id, uint32_t request,
    uint32_t timeout_ms, uint8_t *value, size_t max_len, size_t *value_len)
{
    lock_state();
    ble_session_t *s = find_locked(id); /* busy pins the slot, including after close */
    SemaphoreHandle_t wake = s->wake;
    unlock_state();
    if (xSemaphoreTake(wake, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        lock_dispatch();
        lock_state();
        uint32_t epoch = 0;
        if (s->request == request && s->pending) {
            epoch = retire_locked(id, ESP_ERR_TIMEOUT);
        }
        unlock_state();
        if (epoch != 0) {
            (void)solar_os_ble_backend_cancel(epoch);
        }
        unlock_dispatch();
    }
    lock_state();
    const esp_err_t ret = s->result;
    if (ret == ESP_OK && value_len != NULL) {
        const size_t copied = s->value_len < max_len ? s->value_len : max_len;
        if (copied != 0) {
            memcpy(value, s->value, copied);
        }
        *value_len = s->value_len;
    }
    s->busy = false;
    s->pending = false;
    s->op = BLE_OP_NONE;
    if (s->closing) {
        s->id = 0;
    }
    unlock_state();
    return ret;
}

esp_err_t solar_os_ble_session_connect(solar_os_ble_session_t id,
    const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms)
{
    if (bda == NULL || addr_type > SOLAR_OS_BLE_ADDR_RANDOM_IDENTITY) {
        return ESP_ERR_INVALID_ARG;
    }
    lock_dispatch();
    lock_state();
    const bool valid = live_locked(id) != NULL && !sleeping;
    unlock_state();
    esp_err_t ret = valid ? solar_os_ble_backend_init() : ESP_ERR_INVALID_STATE;
    if (ret != ESP_OK) {
        unlock_dispatch();
        return ret;
    }
    lock_state();
    const uint32_t retiring_epoch = link_state.retiring ? link_state.epoch : 0;
    unlock_state();
    if (retiring_epoch != 0) {
        /* Retry an earlier teardown enqueue failure, including after its
         * owning session was closed. Never reclaim merely because time passed. */
        (void)solar_os_ble_backend_cancel(retiring_epoch);
    }
    lock_state();
    ble_session_t *s = live_locked(id);
    if (link_state.epoch != 0) {
        unlock_state();
        unlock_dispatch();
        return ESP_ERR_INVALID_STATE;
    }
    ret = begin_locked(s, BLE_OP_CONNECT, 0);
    if (ret != ESP_OK) {
        unlock_state();
        unlock_dispatch();
        return ret;
    }
    const uint32_t request = s->request;
    /* A connect request is also the unique lifetime token for its transport. */
    clear_link_locked("connecting");
    link_state.epoch = request;
    link_state.owner = id;
    link_state.info.addr_type = addr_type;
    memcpy(link_state.info.bda, bda, sizeof(link_state.info.bda));
    unlock_state();
    ret = solar_os_ble_backend_connect(request, request, bda, addr_type);
    if (ret != ESP_OK) {
        lock_state();
        finish_locked(s, ret);
        clear_link_locked("connect failed");
        unlock_state();
    }
    unlock_dispatch();
    return wait_operation(id, request, timeout_ms != 0 ? timeout_ms : BLE_CONNECT_TIMEOUT_MS,
                          NULL, 0, NULL);
}

esp_err_t solar_os_ble_session_services(solar_os_ble_session_t id,
    solar_os_ble_gatt_service_t *services, size_t max_services, size_t *count)
{
    if (count != NULL) {
        *count = 0;
    }
    if (max_services != 0 && services == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_ble_service_prepare_runtime();
    lock_state();
    if (live_locked(id) == NULL || link_state.owner != id ||
        !link_state.info.connected || link_state.retiring) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    const size_t copied = link_state.info.service_count < max_services ?
        link_state.info.service_count : max_services;
    if (copied != 0) {
        memcpy(services, link_state.services, copied * sizeof(*services));
    }
    if (count != NULL) {
        *count = link_state.info.service_count;
    }
    unlock_state();
    return ESP_OK;
}

esp_err_t solar_os_ble_session_characteristics(solar_os_ble_session_t id,
    size_t service_index, solar_os_ble_gatt_characteristic_t *characteristics,
    size_t max_characteristics, size_t *count)
{
    if (count != NULL) {
        *count = 0;
    }
    if (max_characteristics != 0 && characteristics == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock_dispatch();
    lock_state();
    ble_session_t *s = live_locked(id);
    if (s == NULL || s->busy || link_state.owner != id || !link_state.info.connected ||
        link_state.retiring || sleeping) {
        unlock_state();
        unlock_dispatch();
        return ESP_ERR_INVALID_STATE;
    }
    if (service_index >= link_state.info.service_count) {
        unlock_state();
        unlock_dispatch();
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t epoch = link_state.epoch;
    const solar_os_ble_gatt_service_t service = link_state.services[service_index];
    unlock_state();
    esp_err_t ret = solar_os_ble_backend_characteristics(epoch, &service, characteristics,
                                                         max_characteristics, count);
    lock_state();
    if (link_state.epoch != epoch || link_state.retiring) {
        ret = ESP_ERR_INVALID_STATE;
        if (count != NULL) {
            *count = 0;
        }
    }
    unlock_state();
    unlock_dispatch();
    return ret;
}

static esp_err_t transfer(solar_os_ble_session_t id, uint16_t handle,
    const uint8_t *write_value, size_t write_len, bool with_response,
    uint8_t *read_value, size_t read_max, size_t *read_len, uint32_t timeout_ms, bool write)
{
    lock_dispatch();
    lock_state();
    ble_session_t *s = live_locked(id);
    if (link_state.owner != id || !link_state.info.connected || link_state.retiring) {
        unlock_state();
        unlock_dispatch();
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t begin = begin_locked(s, write ? BLE_OP_WRITE : BLE_OP_READ, handle);
    if (begin != ESP_OK) {
        unlock_state();
        unlock_dispatch();
        return begin;
    }
    const uint32_t epoch = link_state.epoch, request = s->request;
    unlock_state();
    const esp_err_t ret = write ?
        solar_os_ble_backend_write(epoch, request, handle, write_value, write_len, with_response) :
        solar_os_ble_backend_read(epoch, request, handle);
    if (ret != ESP_OK) {
        lock_state();
        if (s->pending) {
            finish_locked(s, ret);
        }
        unlock_state();
    }
    unlock_dispatch();
    return wait_operation(id, request, timeout_ms != 0 ? timeout_ms : BLE_OPERATION_TIMEOUT_MS,
                          read_value, read_max, read_len);
}

esp_err_t solar_os_ble_session_read(solar_os_ble_session_t id,
    uint16_t handle, uint8_t *value, size_t max_len, size_t *value_len, uint32_t timeout_ms)
{
    if (value_len != NULL) {
        *value_len = 0;
    }
    if (handle == 0 || (max_len != 0 && value == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t ignored;
    return transfer(id, handle, NULL, 0, false, value, max_len,
                    value_len != NULL ? value_len : &ignored, timeout_ms, false);
}

esp_err_t solar_os_ble_session_write(solar_os_ble_session_t id,
    uint16_t handle, const uint8_t *value, size_t value_len, bool with_response, uint32_t timeout_ms)
{
    if (handle == 0 || value == NULL || value_len == 0 || value_len > SOLAR_OS_BLE_GATT_VALUE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return transfer(id, handle, value, value_len, with_response, NULL, 0, NULL, timeout_ms, true);
}

void solar_os_ble_service_event(const solar_os_ble_backend_event_t *event)
{
    if (event == NULL) {
        return;
    }
    lock_state();
    if (event->epoch == 0 || event->epoch != link_state.epoch) {
        unlock_state();
        return;
    }
    ble_session_t *s = find_locked(link_state.owner);
    if (event->type == SOLAR_OS_BLE_BACKEND_RETIRED) {
        if (s != NULL && s->pending) {
            finish_locked(s, ESP_FAIL);
        }
        clear_link_locked("disconnected");
        unlock_state();
        return;
    }
    if (link_state.retiring || s == NULL || s->closing) {
        unlock_state();
        return;
    }
    const bool matched = s->busy && s->pending && s->request == event->request;
    switch (event->type) {
    case SOLAR_OS_BLE_BACKEND_OPENED:
        if (!matched || s->op != BLE_OP_CONNECT) {
            break;
        }
        if (event->result != ESP_OK) {
            link_state.retiring = true;
            finish_locked(s, event->result);
        } else {
            link_state.info.conn_id = event->conn_id;
            link_state.info.mtu = event->mtu;
            strlcpy(link_state.info.status, "discovering", sizeof(link_state.info.status));
        }
        break;
    case SOLAR_OS_BLE_BACKEND_MTU:
        if (event->conn_id == link_state.info.conn_id && event->result == ESP_OK) {
            link_state.info.mtu = event->mtu;
        }
        break;
    case SOLAR_OS_BLE_BACKEND_SERVICE:
        if (matched && s->op == BLE_OP_CONNECT && event->conn_id == link_state.info.conn_id &&
            link_state.info.service_count < SOLAR_OS_BLE_GATT_MAX_SERVICES) {
            link_state.services[link_state.info.service_count++] = event->service;
        }
        break;
    case SOLAR_OS_BLE_BACKEND_DISCOVERED:
        if (matched && s->op == BLE_OP_CONNECT && event->conn_id == link_state.info.conn_id) {
            link_state.info.connected = event->result == ESP_OK;
            link_state.retiring = event->result != ESP_OK;
            strlcpy(link_state.info.status, event->result == ESP_OK ? "connected" : "discovery failed",
                    sizeof(link_state.info.status));
            finish_locked(s, event->result);
        }
        break;
    case SOLAR_OS_BLE_BACKEND_READ:
    case SOLAR_OS_BLE_BACKEND_WRITTEN:
        if (matched && event->conn_id == link_state.info.conn_id && s->handle == event->handle &&
            s->op == (event->type == SOLAR_OS_BLE_BACKEND_READ ? BLE_OP_READ : BLE_OP_WRITE)) {
            if (s->op == BLE_OP_READ && event->result == ESP_OK && event->value != NULL) {
                s->value_len = event->value_len < sizeof(s->value) ? event->value_len : sizeof(s->value);
                memcpy(s->value, event->value, s->value_len);
            }
            finish_locked(s, event->result);
        }
        break;
    case SOLAR_OS_BLE_BACKEND_CLOSED:
        if (event->conn_id == link_state.info.conn_id) {
            link_state.retiring = true;
            link_state.info.connected = false;
            link_state.info.service_count = 0;
            strlcpy(link_state.info.status, "disconnected", sizeof(link_state.info.status));
            if (s->pending) {
                finish_locked(s, ESP_FAIL);
            }
        }
        break;
    default:
        break;
    }
    unlock_state();
}

static solar_os_ble_session_t compatibility_session(void)
{
    solar_os_ble_service_prepare_runtime();
    lock_state();
    solar_os_ble_session_t id = sessions[0].id;
    if (id == 0) {
        (void)create_locked(0, 1, "ble.shell", &id);
    }
    unlock_state();
    return id;
}

esp_err_t solar_os_ble_gatt_connect(const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms)
{
    return solar_os_ble_session_connect(compatibility_session(), bda, addr_type, timeout_ms);
}

esp_err_t solar_os_ble_gatt_disconnect(void)
{
    return solar_os_ble_session_cancel(compatibility_session());
}

void solar_os_ble_gatt_get_status(solar_os_ble_gatt_status_t *status)
{
    if (status != NULL) {
        solar_os_ble_session_info_t info = {0};
        if (solar_os_ble_session_get_info(compatibility_session(), &info) != ESP_OK) {
            info.gatt.conn_id = SOLAR_OS_BLE_CONNECTION_INVALID;
        }
        *status = info.gatt;
    }
}

esp_err_t solar_os_ble_gatt_services(solar_os_ble_gatt_service_t *services, size_t max_services, size_t *count)
{
    return solar_os_ble_session_services(compatibility_session(), services, max_services, count);
}

esp_err_t solar_os_ble_gatt_characteristics(size_t service_index,
    solar_os_ble_gatt_characteristic_t *characteristics, size_t max_characteristics, size_t *count)
{
    return solar_os_ble_session_characteristics(compatibility_session(), service_index,
                                               characteristics, max_characteristics, count);
}

esp_err_t solar_os_ble_gatt_read(uint16_t handle, uint8_t *value, size_t max_len,
                               size_t *value_len, uint32_t timeout_ms)
{
    return solar_os_ble_session_read(compatibility_session(), handle, value, max_len, value_len, timeout_ms);
}

esp_err_t solar_os_ble_gatt_write(uint16_t handle, const uint8_t *value, size_t value_len,
                                bool with_response, uint32_t timeout_ms)
{
    return solar_os_ble_session_write(compatibility_session(), handle, value, value_len, with_response, timeout_ms);
}
