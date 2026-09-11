#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_ble.h"
#include "solar_os_ble_backend.h"
#include <freertos/semphr.h>

struct ble_test_semaphore {
    unsigned count;
    bool mutex;
};

static struct ble_test_semaphore semaphores[2];
static unsigned allocated;
static bool initialized;
static uint16_t completion_status;
static esp_err_t submit_result;
static bool withhold_read;
static bool disconnect_on_read;
static bool wrote_with_response;
static unsigned scan_calls;
static unsigned resume_calls;
static size_t discovery_count = 1;
static const uint8_t peer[6] = {1, 2, 3, 4, 5, 6};

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    assert(allocated < 2);
    SemaphoreHandle_t sem = &semaphores[allocated++];
    sem->count = 1;
    sem->mutex = true;
    return sem;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    assert(allocated < 2);
    return &semaphores[allocated++];
}

int xSemaphoreTake(SemaphoreHandle_t sem, unsigned timeout)
{
    (void)timeout;
    assert(sem != NULL);
    /* Reentrant backend calls expose any service lock held across dispatch. */
    if (sem->mutex) {
        assert(sem->count == 1);
    }
    if (sem->count == 0) {
        return pdFALSE;
    }
    sem->count = 0;
    return pdTRUE;
}

int xSemaphoreGive(SemaphoreHandle_t sem)
{
    assert(sem != NULL);
    sem->count = 1;
    return pdTRUE;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size != 0) {
        const size_t copied = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copied);
        dst[copied] = '\0';
    }
    return len;
}

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "test error";
}

esp_err_t solar_os_ble_backend_register(void)
{
    const solar_os_ble_backend_event_t event = {
        .type = SOLAR_OS_BLE_BACKEND_REGISTERED,
        .status = completion_status,
        .result = completion_status == 0 ? ESP_OK : ESP_FAIL,
    };
    solar_os_ble_service_event(&event);
    return ESP_OK;
}

void solar_os_ble_backend_reset(void) {}

esp_err_t solar_os_ble_backend_init(void)
{
    if (initialized) {
        return ESP_OK;
    }
    assert(solar_os_ble_service_prepare_runtime() == ESP_OK);
    const esp_err_t ret = solar_os_ble_service_register();
    initialized = ret == ESP_OK;
    return ret;
}

esp_err_t solar_os_ble_backend_scan(solar_os_ble_scan_result_t *results,
                                   size_t max_results, size_t *found)
{
    assert(max_results == 1);
    memcpy(results[0].bda, peer, sizeof(peer));
    *found = 1;
    scan_calls++;
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_prepare_sleep(uint32_t timeout_ms)
{
    assert(timeout_ms == 1500);
    solar_os_ble_service_reset("sleep");
    initialized = false;
    return ESP_OK;
}

bool solar_os_ble_backend_sleep_prepare_ready(void) { return true; }
void solar_os_ble_backend_resume(void) { resume_calls++; }

esp_err_t solar_os_ble_backend_connect(const uint8_t bda[6], uint8_t addr_type)
{
    assert(memcmp(bda, peer, sizeof(peer)) == 0);
    assert(addr_type == SOLAR_OS_BLE_ADDR_RANDOM);
    if (submit_result != ESP_OK) {
        return submit_result;
    }
    solar_os_ble_backend_event_t event = {
        .type = SOLAR_OS_BLE_BACKEND_OPENED,
        .conn_id = 7,
        .mtu = 23,
        .status = completion_status,
        .result = completion_status == 0 ? ESP_OK : ESP_FAIL,
    };
    memcpy(event.bda, bda, sizeof(event.bda));
    solar_os_ble_service_event(&event);
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_discover(uint16_t conn_id)
{
    assert(conn_id == 7);
    solar_os_ble_backend_event_t event = {
        .type = SOLAR_OS_BLE_BACKEND_SERVICE,
        .conn_id = conn_id,
        .service = {.start_handle = 1, .end_handle = 9, .primary = true},
    };
    strcpy(event.service.uuid, "0x180f");
    for (size_t i = 0; i < discovery_count; i++) {
        solar_os_ble_service_event(&event);
    }
    event.type = SOLAR_OS_BLE_BACKEND_MTU;
    event.mtu = 247;
    solar_os_ble_service_event(&event);
    event.type = SOLAR_OS_BLE_BACKEND_DISCOVERED;
    solar_os_ble_service_event(&event);
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_disconnect(uint16_t conn_id)
{
    assert(conn_id == 7);
    return submit_result;
}

esp_err_t solar_os_ble_backend_characteristics(uint16_t conn_id,
    const solar_os_ble_gatt_service_t *service,
    solar_os_ble_gatt_characteristic_t *characteristics,
    size_t max_characteristics, size_t *count)
{
    assert(conn_id == 7 && service->start_handle == 1);
    if (max_characteristics > 0) {
        characteristics[0].handle = 3;
        characteristics[0].properties = SOLAR_OS_BLE_CHAR_READ;
        strcpy(characteristics[0].uuid, "0x2a19");
        *count = 1;
    }
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_read(uint16_t conn_id, uint16_t handle)
{
    assert(conn_id == 7 && handle == 3);
    if (submit_result != ESP_OK || withhold_read) {
        return submit_result;
    }
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX + 1];
    memset(value, 0x42, sizeof(value));
    solar_os_ble_backend_event_t event = {
        .type = disconnect_on_read ? SOLAR_OS_BLE_BACKEND_CLOSED : SOLAR_OS_BLE_BACKEND_READ,
        .conn_id = conn_id,
        .handle = handle,
        .status = completion_status,
        .result = completion_status == 0 ? ESP_OK : ESP_FAIL,
        .value = value,
        .value_len = sizeof(value),
    };
    solar_os_ble_service_event(&event);
    /* The service must have copied the borrowed buffer before returning. */
    memset(value, 0xee, sizeof(value));
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_write(uint16_t conn_id, uint16_t handle,
    const uint8_t *value, size_t value_len, bool with_response)
{
    assert(conn_id == 7 && handle == 3 && value_len == 2);
    assert(value[0] == 0 && value[1] == 0xff);
    wrote_with_response = with_response;
    const solar_os_ble_backend_event_t event = {
        .type = SOLAR_OS_BLE_BACKEND_WRITTEN,
        .conn_id = conn_id,
        .handle = handle,
        .status = completion_status,
        .result = completion_status == 0 ? ESP_OK : ESP_FAIL,
    };
    if (with_response) {
        solar_os_ble_service_event(&event);
    }
    return ESP_OK;
}

int main(void)
{
    solar_os_ble_gatt_status_t status;
    solar_os_ble_gatt_get_status(&status);
    assert(!status.connected && status.conn_id == SOLAR_OS_BLE_CONNECTION_INVALID);
    assert(solar_os_ble_gatt_connect(NULL, 0, 0) == ESP_ERR_INVALID_ARG);
    completion_status = 0x85;
    assert(solar_os_ble_init() == ESP_FAIL);
    completion_status = 0;
    assert(solar_os_ble_init() == ESP_OK);
    assert(solar_os_ble_init() == ESP_OK && allocated == 2);
    submit_result = ESP_ERR_NO_MEM;
    assert(solar_os_ble_gatt_connect(peer, SOLAR_OS_BLE_ADDR_RANDOM, 0) == ESP_ERR_NO_MEM);
    submit_result = ESP_OK;
    completion_status = 0x85;
    assert(solar_os_ble_gatt_connect(peer, SOLAR_OS_BLE_ADDR_RANDOM, 0) == ESP_FAIL);
    completion_status = 0;
    assert(solar_os_ble_gatt_connect(peer, SOLAR_OS_BLE_ADDR_RANDOM, 0) == ESP_OK);
    solar_os_ble_gatt_get_status(&status);
    assert(status.connected && status.conn_id == 7 && status.mtu == 247);
    assert(status.addr_type == SOLAR_OS_BLE_ADDR_RANDOM && status.service_count == 1);
    assert(memcmp(status.bda, peer, sizeof(peer)) == 0);
    assert(solar_os_ble_gatt_connect(peer, SOLAR_OS_BLE_ADDR_RANDOM, 0) == ESP_ERR_INVALID_STATE);

    solar_os_ble_gatt_service_t service;
    size_t count = 0;
    assert(solar_os_ble_gatt_services(&service, 1, &count) == ESP_OK && count == 1);
    assert(strcmp(service.uuid, "0x180f") == 0 && service.end_handle == 9);
    solar_os_ble_gatt_characteristic_t characteristic;
    assert(solar_os_ble_gatt_characteristics(1, &characteristic, 1, &count) == ESP_ERR_NOT_FOUND);
    assert(solar_os_ble_gatt_characteristics(0, &characteristic, 1, &count) == ESP_OK);
    assert(count == 1 && characteristic.handle == 3);

    uint8_t value[2] = {0};
    assert(solar_os_ble_gatt_read(3, value, sizeof(value), &count, 0) == ESP_OK);
    assert(value[0] == 0x42 && value[1] == 0x42 && count == SOLAR_OS_BLE_GATT_VALUE_MAX);
    completion_status = 5;
    assert(solar_os_ble_gatt_read(3, value, sizeof(value), &count, 0) == ESP_FAIL && count == 0);
    completion_status = 0;
    withhold_read = true;
    assert(solar_os_ble_gatt_read(3, value, sizeof(value), &count, 1) == ESP_ERR_TIMEOUT);
    withhold_read = false;
    value[0] = 0;
    value[1] = 0xff;
    assert(solar_os_ble_gatt_write(3, value, 2, true, 0) == ESP_OK && wrote_with_response);
    assert(solar_os_ble_gatt_write(3, value, 2, false, 0) == ESP_OK && !wrote_with_response);
    assert(solar_os_ble_gatt_write(3, value, SOLAR_OS_BLE_GATT_VALUE_MAX + 1, true, 0) == ESP_ERR_INVALID_ARG);

    disconnect_on_read = true;
    assert(solar_os_ble_gatt_read(3, value, sizeof(value), &count, 0) == ESP_FAIL);
    solar_os_ble_gatt_get_status(&status);
    assert(!status.connected && status.service_count == 0);
    disconnect_on_read = false;
    discovery_count = SOLAR_OS_BLE_GATT_MAX_SERVICES + 1;
    assert(solar_os_ble_gatt_connect(peer, SOLAR_OS_BLE_ADDR_RANDOM, 0) == ESP_OK);
    assert(solar_os_ble_gatt_services(&service, 1, &count) == ESP_OK);
    assert(count == SOLAR_OS_BLE_GATT_MAX_SERVICES);
    assert(solar_os_ble_gatt_disconnect() == ESP_OK);
    assert(solar_os_ble_gatt_services(&service, 1, &count) == ESP_ERR_INVALID_STATE);

    solar_os_ble_scan_result_t scan;
    assert(solar_os_ble_scan(&scan, 1, &count) == ESP_OK && count == 1 && scan_calls == 1);
    assert(solar_os_ble_prepare_sleep(1500) == ESP_OK);
    solar_os_ble_gatt_get_status(&status);
    assert(!status.connected && strcmp(status.status, "sleep") == 0);
    assert(solar_os_ble_sleep_prepare_ready());
    solar_os_ble_resume();
    assert(resume_calls == 1);
    assert(solar_os_ble_init() == ESP_OK && allocated == 2);
    puts("BLE service backend contract: OK");
    return 0;
}
