#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "solar_os_ble.h"
#include "solar_os_ble_backend.h"

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_changed = PTHREAD_COND_INITIALIZER;
static solar_os_ble_backend_event_t submitted;
static unsigned submissions, cancellations;
static uint32_t fake_epoch;
static bool initialized, defer_connect, defer_read;
static bool write_response;
static esp_err_t submit_result;
static const uint8_t peer[6] = {1, 2, 3, 4, 5, 6};

static void record(solar_os_ble_backend_event_t event)
{
    pthread_mutex_lock(&fake_lock);
    submitted = event;
    submissions++;
    pthread_cond_broadcast(&fake_changed);
    pthread_mutex_unlock(&fake_lock);
}

static unsigned submission_count(void)
{
    pthread_mutex_lock(&fake_lock);
    const unsigned n = submissions;
    pthread_mutex_unlock(&fake_lock);
    return n;
}

static solar_os_ble_backend_event_t await_submission(unsigned after)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    pthread_mutex_lock(&fake_lock);
    while (submissions == after) {
        assert(pthread_cond_timedwait(&fake_changed, &fake_lock, &deadline) == 0);
    }
    solar_os_ble_backend_event_t event = submitted;
    pthread_mutex_unlock(&fake_lock);
    return event;
}

static void retired(void)
{
    solar_os_ble_backend_event_t event = {
        .type = SOLAR_OS_BLE_BACKEND_RETIRED, .epoch = fake_epoch,
    };
    fake_epoch = 0;
    solar_os_ble_service_event(&event);
}

esp_err_t solar_os_ble_backend_register(void) { return ESP_OK; }
void solar_os_ble_backend_reset(void) { fake_epoch = 0; }

esp_err_t solar_os_ble_backend_init(void)
{
    if (!initialized) {
        initialized = solar_os_ble_service_register() == ESP_OK;
    }
    return initialized ? ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_ble_backend_scan(solar_os_ble_scan_result_t *results,
                                   size_t max_results, size_t *found)
{
    assert(max_results == 1);
    memcpy(results[0].bda, peer, sizeof(peer));
    *found = 1;
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_prepare_sleep(uint32_t timeout_ms)
{
    assert(timeout_ms == 1500);
    solar_os_ble_service_reset("sleep");
    solar_os_ble_backend_reset();
    initialized = false;
    return ESP_OK;
}

bool solar_os_ble_backend_sleep_prepare_ready(void) { return true; }
void solar_os_ble_backend_resume(void) { assert(solar_os_ble_backend_init() == ESP_OK); }

esp_err_t solar_os_ble_backend_connect(uint32_t epoch, uint32_t request,
    const uint8_t bda[6], uint8_t addr_type)
{
    assert(fake_epoch == 0 && epoch != 0 && request != 0);
    assert(memcmp(bda, peer, sizeof(peer)) == 0 && addr_type == SOLAR_OS_BLE_ADDR_RANDOM);
    if (submit_result != ESP_OK) {
        return submit_result;
    }
    fake_epoch = epoch;
    solar_os_ble_backend_event_t event = {
        .type = SOLAR_OS_BLE_BACKEND_OPENED, .epoch = epoch, .request = request,
        .conn_id = 7, .mtu = 23,
    };
    memcpy(event.bda, peer, sizeof(peer));
    record(event);
    if (defer_connect) {
        return ESP_OK;
    }
    solar_os_ble_service_event(&event);
    event.type = SOLAR_OS_BLE_BACKEND_SERVICE;
    event.service.start_handle = 1;
    event.service.end_handle = 9;
    event.service.primary = true;
    strcpy(event.service.uuid, "0x180f");
    for (unsigned i = 0; i < SOLAR_OS_BLE_GATT_MAX_SERVICES + 1; i++) {
        solar_os_ble_service_event(&event);
    }
    event.type = SOLAR_OS_BLE_BACKEND_MTU;
    event.mtu = 247;
    solar_os_ble_service_event(&event);
    event.type = SOLAR_OS_BLE_BACKEND_DISCOVERED;
    solar_os_ble_service_event(&event);
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_cancel(uint32_t epoch)
{
    assert(epoch == fake_epoch && epoch != 0);
    cancellations++;
    /* Logical cancellation is immediate; transport retirement is controlled
     * separately by the test, like an asynchronous stack unregister event. */
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_characteristics(uint32_t epoch,
    const solar_os_ble_gatt_service_t *service,
    solar_os_ble_gatt_characteristic_t *characteristics,
    size_t max_characteristics, size_t *count)
{
    assert(epoch == fake_epoch && service->start_handle == 1);
    if (max_characteristics != 0) {
        characteristics[0].handle = 3;
        characteristics[0].properties = SOLAR_OS_BLE_CHAR_READ;
        strcpy(characteristics[0].uuid, "0x2a19");
        *count = 1;
    }
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_read(uint32_t epoch, uint32_t request, uint16_t handle)
{
    assert(epoch == fake_epoch && handle == 3);
    if (submit_result != ESP_OK) {
        return submit_result;
    }
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX + 1];
    memset(value, 0x42, sizeof(value));
    solar_os_ble_backend_event_t event = {
        .type = SOLAR_OS_BLE_BACKEND_READ, .epoch = epoch, .request = request,
        .conn_id = 7, .handle = handle,
    };
    record(event);
    if (!defer_read) {
        event.value = value;
        event.value_len = sizeof(value);
        solar_os_ble_service_event(&event);
        memset(value, 0xee, sizeof(value)); /* service must copy before returning */
    }
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_write(uint32_t epoch, uint32_t request, uint16_t handle,
    const uint8_t *value, size_t value_len, bool with_response)
{
    assert(epoch == fake_epoch && handle == 3 && value_len == 2);
    assert(value[0] == 0 && value[1] == 0xff);
    write_response = with_response;
    const solar_os_ble_backend_event_t event = {
        .type = SOLAR_OS_BLE_BACKEND_WRITTEN, .epoch = epoch, .request = request,
        .conn_id = 7, .handle = handle,
    };
    solar_os_ble_service_event(&event);
    return ESP_OK;
}

typedef struct {
    solar_os_ble_session_t id;
    bool connect;
    esp_err_t result;
    uint8_t value[2];
    size_t len;
} call_t;

static void *run_call(void *arg)
{
    call_t *call = arg;
    call->result = call->connect ?
        solar_os_ble_session_connect(call->id, peer, SOLAR_OS_BLE_ADDR_RANDOM, 2000) :
        solar_os_ble_session_read(call->id, 3, call->value, sizeof(call->value), &call->len, 2000);
    return NULL;
}

static void connect_session(solar_os_ble_session_t id)
{
    assert(solar_os_ble_session_connect(id, peer, SOLAR_OS_BLE_ADDR_RANDOM, 100) == ESP_OK);
}

static void assert_busy(solar_os_ble_session_t id)
{
    solar_os_ble_session_info_t info;
    assert(solar_os_ble_session_get_info(id, &info) == ESP_OK && info.busy);
}

int main(void)
{
    solar_os_ble_session_t ids[SOLAR_OS_BLE_SESSION_MAX], extra;
    assert(solar_os_ble_session_create("", &extra) == ESP_ERR_INVALID_ARG);
    for (unsigned i = 0; i < SOLAR_OS_BLE_SESSION_MAX; i++) {
        assert(solar_os_ble_session_create("test", &ids[i]) == ESP_OK);
    }
    assert(solar_os_ble_session_create("overflow", &extra) == ESP_ERR_NO_MEM);
    const solar_os_ble_session_t stale = ids[3];
    assert(solar_os_ble_session_close(stale) == ESP_OK);
    assert(solar_os_ble_session_create("replacement", &ids[3]) == ESP_OK && ids[3] != stale);
    assert(solar_os_ble_session_cancel(stale) == ESP_ERR_INVALID_STATE);
    assert(solar_os_ble_session_close(stale) == ESP_ERR_INVALID_STATE);

    submit_result = ESP_ERR_NO_MEM;
    assert(solar_os_ble_session_connect(ids[0], peer, SOLAR_OS_BLE_ADDR_RANDOM, 100) == ESP_ERR_NO_MEM);
    submit_result = ESP_OK;
    connect_session(ids[0]);
    solar_os_ble_session_info_t info;
    assert(solar_os_ble_session_get_info(ids[0], &info) == ESP_OK);
    assert(info.gatt.connected && info.gatt.mtu == 247 && info.gatt.conn_id == 7);
    assert(info.gatt.service_count == SOLAR_OS_BLE_GATT_MAX_SERVICES);
    solar_os_ble_scan_result_t scan;
    size_t found = 123;
    assert(solar_os_ble_scan(&scan, 1, &found) == ESP_ERR_INVALID_STATE && found == 0);
    assert(solar_os_ble_session_connect(ids[1], peer, SOLAR_OS_BLE_ADDR_RANDOM, 1) == ESP_ERR_INVALID_STATE);
    uint8_t value[2];
    size_t count = 0;
    assert(solar_os_ble_session_read(ids[1], 3, value, sizeof(value), &count, 1) == ESP_ERR_INVALID_STATE);
    const unsigned before = cancellations;
    assert(solar_os_ble_session_cancel(ids[1]) == ESP_OK && cancellations == before);
    assert(solar_os_ble_gatt_read(3, value, sizeof(value), &count, 1) == ESP_ERR_INVALID_STATE);
    assert(solar_os_ble_gatt_disconnect() == ESP_OK && cancellations == before);

    solar_os_ble_gatt_service_t service;
    assert(solar_os_ble_session_services(ids[0], &service, 1, &count) == ESP_OK);
    assert(count == SOLAR_OS_BLE_GATT_MAX_SERVICES && strcmp(service.uuid, "0x180f") == 0);
    solar_os_ble_gatt_characteristic_t characteristic;
    assert(solar_os_ble_session_characteristics(ids[0], 0, &characteristic, 1, &count) == ESP_OK);
    assert(characteristic.handle == 3);
    assert(solar_os_ble_session_read(ids[0], 3, value, sizeof(value), &count, 100) == ESP_OK);
    assert(count == SOLAR_OS_BLE_GATT_VALUE_MAX && value[0] == 0x42);
    value[0] = 0; value[1] = 0xff;
    assert(solar_os_ble_session_write(ids[0], 3, value, 2, false, 100) == ESP_OK && !write_response);
    assert(solar_os_ble_session_write(ids[0], 3, value, 2, true, 100) == ESP_OK && write_response);

    /* Close wakes an in-flight reader. Its result slot cannot belong to a new owner. */
    defer_read = true;
    unsigned n = submission_count();
    call_t call = {.id = ids[0]};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, run_call, &call) == 0);
    solar_os_ble_backend_event_t old_read = await_submission(n);
    assert_busy(ids[0]);
    assert(solar_os_ble_session_read(ids[0], 3, value, sizeof(value), &count, 1) == ESP_ERR_INVALID_STATE);
    assert(solar_os_ble_session_close(ids[0]) == ESP_OK);
    assert(pthread_join(thread, NULL) == 0 && call.result == SOLAR_OS_BLE_ERR_CANCELLED);
    assert(solar_os_ble_session_get_info(ids[0], &info) == ESP_ERR_INVALID_STATE);
    assert(solar_os_ble_session_connect(ids[1], peer, SOLAR_OS_BLE_ADDR_RANDOM, 1) == ESP_ERR_INVALID_STATE);
    retired();
    connect_session(ids[1]); /* Same backend conn_id and characteristic handles. */

    n = submission_count();
    call = (call_t){.id = ids[1]};
    assert(pthread_create(&thread, NULL, run_call, &call) == 0);
    solar_os_ble_backend_event_t current = await_submission(n);
    old_read.value = (const uint8_t *)"old";
    old_read.value_len = 3;
    solar_os_ble_service_event(&old_read);
    old_read.type = SOLAR_OS_BLE_BACKEND_CLOSED;
    solar_os_ble_service_event(&old_read);
    assert_busy(ids[1]);
    solar_os_ble_backend_event_t wrong = current;
    wrong.request--;
    solar_os_ble_service_event(&wrong);
    wrong = current; wrong.handle++;
    solar_os_ble_service_event(&wrong);
    wrong = current; wrong.conn_id++;
    solar_os_ble_service_event(&wrong);
    assert_busy(ids[1]);
    current.value = (const uint8_t *)"OK";
    current.value_len = 2;
    solar_os_ble_service_event(&current);
    assert(pthread_join(thread, NULL) == 0 && call.result == ESP_OK);
    assert(call.len == 2 && memcmp(call.value, "OK", 2) == 0);

    /* A timeout quarantines its connection until retirement, not just its waiter. */
    assert(solar_os_ble_session_read(ids[1], 3, value, 2, &count, 1) == ESP_ERR_TIMEOUT);
    assert(solar_os_ble_session_get_info(ids[1], &info) == ESP_OK && info.retiring);
    assert(solar_os_ble_session_read(ids[1], 3, value, 2, &count, 1) == ESP_ERR_INVALID_STATE);
    retired();

    /* Cancel while open is pending; a late successful OPEN cannot resurrect it. */
    defer_connect = true;
    n = submission_count();
    call = (call_t){.id = ids[1], .connect = true};
    assert(pthread_create(&thread, NULL, run_call, &call) == 0);
    solar_os_ble_backend_event_t old_open = await_submission(n);
    assert(solar_os_ble_session_cancel(ids[1]) == ESP_OK);
    solar_os_ble_service_event(&old_open);
    assert(pthread_join(thread, NULL) == 0 && call.result == SOLAR_OS_BLE_ERR_CANCELLED);
    assert(solar_os_ble_session_get_info(ids[1], &info) == ESP_OK && !info.gatt.connected);
    retired();
    defer_connect = false;
    connect_session(ids[1]);

    /* Sleep cancels a reader, preserves its session, and invalidates old epochs. */
    n = submission_count();
    call = (call_t){.id = ids[1]};
    assert(pthread_create(&thread, NULL, run_call, &call) == 0);
    old_read = await_submission(n);
    assert(solar_os_ble_prepare_sleep(1500) == ESP_OK);
    assert(pthread_join(thread, NULL) == 0 && call.result == SOLAR_OS_BLE_ERR_CANCELLED);
    assert(solar_os_ble_session_connect(ids[1], peer, SOLAR_OS_BLE_ADDR_RANDOM, 1) == ESP_ERR_INVALID_STATE);
    assert(solar_os_ble_sleep_prepare_ready());
    solar_os_ble_resume();
    connect_session(ids[1]);
    solar_os_ble_service_event(&old_read);
    solar_os_ble_service_event(&old_open);
    assert(solar_os_ble_session_get_info(ids[1], &info) == ESP_OK && info.gatt.connected);
    assert(solar_os_ble_session_cancel(ids[1]) == ESP_OK);
    retired();
    for (unsigned i = 1; i < SOLAR_OS_BLE_SESSION_MAX; i++) {
        assert(solar_os_ble_session_close(ids[i]) == ESP_OK);
    }

    /* The reserved compatibility client still works when app slots are released. */
    defer_read = false;
    assert(solar_os_ble_gatt_connect(peer, SOLAR_OS_BLE_ADDR_RANDOM, 100) == ESP_OK);
    assert(solar_os_ble_gatt_read(3, value, 2, &count, 100) == ESP_OK);
    assert(solar_os_ble_gatt_disconnect() == ESP_OK);
    retired();
    assert(solar_os_ble_scan(&scan, 1, &found) == ESP_OK && found == 1);
    puts("BLE sessions: ownership, cancellation, timeout, stale events, sleep and compatibility OK");
    return 0;
}
