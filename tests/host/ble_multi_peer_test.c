#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "solar_os_ble_backend.h"

static bool fail_allocation;
static void *test_calloc(size_t n, size_t size) { return fail_allocation ? NULL : calloc(n, size); }
#define calloc test_calloc
#include "../../src/services/solar_os_ble.c"
#undef calloc

typedef struct { uint32_t epoch; uint16_t conn; } fake_peer_t;
static fake_peer_t peers[3];
static atomic_uint delayed_epoch, read_request;
static bool defer_retirement;

size_t solar_os_ble_backend_capacity(void) { return 3; }
esp_err_t solar_os_ble_backend_register(void) { return ESP_OK; }
esp_err_t solar_os_ble_backend_init(void) { return solar_os_ble_service_register(); }
void solar_os_ble_backend_reset(void) { memset(peers, 0, sizeof(peers)); }
esp_err_t solar_os_ble_backend_scan(solar_os_ble_scan_result_t *r, size_t n, size_t *found)
{ (void)r; (void)n; *found=0; return ESP_OK; }
esp_err_t solar_os_ble_backend_prepare_sleep(uint32_t timeout)
{ (void)timeout; solar_os_ble_service_reset("sleep"); solar_os_ble_backend_reset(); return ESP_OK; }
bool solar_os_ble_backend_sleep_prepare_ready(void) { return true; }
void solar_os_ble_backend_resume(void) { assert(solar_os_ble_backend_init()==ESP_OK); }

static fake_peer_t *peer_for(uint32_t epoch)
{
    for (size_t i=0;i<3;++i) if (epoch && peers[i].epoch==epoch) return &peers[i];
    return NULL;
}

esp_err_t solar_os_ble_backend_connect(uint32_t epoch, uint32_t request, const uint8_t bda[6], uint8_t type)
{
    (void)type;
    fake_peer_t *p=NULL;
    for (size_t i=0;i<3;++i) if (!peers[i].epoch) { p=&peers[i]; p->conn=i+7; break; }
    if (!p) return SOLAR_OS_BLE_ERR_CAPACITY;
    p->epoch=epoch;
    solar_os_ble_backend_event_t e={.type=SOLAR_OS_BLE_BACKEND_OPENED,
        .epoch=epoch,.request=request,.conn_id=p->conn,.mtu=247};
    memcpy(e.bda,bda,6); solar_os_ble_service_event(&e);
    e.type=SOLAR_OS_BLE_BACKEND_SERVICE; e.service.start_handle=1; e.service.end_handle=9;
    snprintf(e.service.uuid,sizeof(e.service.uuid),"peer-%u",bda[5]);
    solar_os_ble_service_event(&e);
    e.type=SOLAR_OS_BLE_BACKEND_DISCOVERED; solar_os_ble_service_event(&e);
    return ESP_OK;
}

static void retire_peer(uint32_t epoch)
{
    fake_peer_t *p=peer_for(epoch); assert(p);
    solar_os_ble_backend_event_t e={.type=SOLAR_OS_BLE_BACKEND_CLOSED,.epoch=epoch,.conn_id=p->conn};
    p->epoch=0;
    solar_os_ble_service_event(&e); e.type=SOLAR_OS_BLE_BACKEND_RETIRED; solar_os_ble_service_event(&e);
}

esp_err_t solar_os_ble_backend_cancel(uint32_t epoch)
{ if (peer_for(epoch) && !defer_retirement) retire_peer(epoch); return ESP_OK; }
esp_err_t solar_os_ble_backend_characteristics(uint32_t epoch, const solar_os_ble_gatt_service_t *service,
    solar_os_ble_gatt_characteristic_t *chars, size_t max, size_t *count)
{ assert(peer_for(epoch) && service->start_handle==1); *count=max ? 1 : 0; if(max)chars[0].handle=3; return ESP_OK; }
esp_err_t solar_os_ble_backend_read(uint32_t epoch, uint32_t request, uint16_t handle)
{
    fake_peer_t *p=peer_for(epoch); assert(p);
    if (atomic_load(&delayed_epoch)==epoch) { atomic_store(&read_request,request); return ESP_OK; }
    uint8_t value=(uint8_t)p->conn;
    solar_os_ble_backend_event_t e={.type=SOLAR_OS_BLE_BACKEND_READ,.epoch=epoch,.request=request,
        .conn_id=p->conn,.handle=handle,.value=&value,.value_len=1};
    solar_os_ble_service_event(&e); return ESP_OK;
}
esp_err_t solar_os_ble_backend_write(uint32_t epoch,uint32_t request,uint16_t handle,
    const uint8_t *value,size_t n,bool response)
{
    (void)value;(void)n;(void)response;
    solar_os_ble_backend_event_t e={.type=SOLAR_OS_BLE_BACKEND_WRITTEN,.epoch=epoch,.request=request,
        .conn_id=peer_for(epoch)->conn,.handle=handle};
    solar_os_ble_service_event(&e); return ESP_OK;
}

typedef struct { solar_os_ble_session_t owner; solar_os_ble_peer_t peer; esp_err_t result; } call_t;
static void *reader(void *arg)
{ call_t *c=arg; uint8_t value; size_t n; c->result=solar_os_ble_peer_read(c->owner,c->peer,3,&value,1,&n,5000); return NULL; }
static size_t entries(void) { size_t n=0;for(ble_session_t *s=sessions;s;s=s->next)++n;return n; }
static void check_read(solar_os_ble_session_t owner,solar_os_ble_peer_t peer,uint8_t expected)
{ uint8_t value=0;size_t n=0;assert(solar_os_ble_peer_read(owner,peer,3,&value,1,&n,100)==ESP_OK);assert(n==1 && value==expected); }

int main(void)
{
    solar_os_ble_session_t owner,other;
    assert(solar_os_ble_session_create("two-batteries",&owner)==ESP_OK);
    assert(solar_os_ble_session_create("other",&other)==ESP_OK);
    uint8_t bda[6]={1,2,3,4,5,1};solar_os_ble_peer_t a,b,c,extra;
    fail_allocation=true;
    assert(solar_os_ble_peer_connect(owner,bda,0,100,&a)==ESP_ERR_NO_MEM && !a);
    fail_allocation=false;assert(entries()==2);
    assert(solar_os_ble_peer_connect(owner,bda,0,100,&a)==ESP_OK);
    bda[5]=2;assert(solar_os_ble_peer_connect(owner,bda,0,100,&b)==ESP_OK && b!=a);
    bda[5]=3;assert(solar_os_ble_peer_connect(other,bda,0,100,&c)==ESP_OK);
    assert(solar_os_ble_peer_connect(owner,bda,0,100,&extra)==SOLAR_OS_BLE_ERR_CAPACITY && !extra);
    assert(entries()==5);check_read(owner,a,7);check_read(owner,b,8);check_read(other,c,9);
    solar_os_ble_session_info_t info;
    assert(solar_os_ble_peer_get_info(other,a,&info)==ESP_ERR_INVALID_STATE);
    assert(solar_os_ble_peer_disconnect(other,a)==ESP_ERR_INVALID_STATE);
    solar_os_ble_gatt_service_t svc;size_t count;
    assert(solar_os_ble_peer_services(owner,b,&svc,1,&count)==ESP_OK && !strcmp(svc.uuid,"peer-2"));
    const uint32_t epoch_a=find_locked(a)->link.epoch;
    atomic_store(&delayed_epoch,epoch_a);
    call_t call={.owner=owner,.peer=a};pthread_t thread;
    assert(pthread_create(&thread,NULL,reader,&call)==0);
    for(unsigned i=0;!atomic_load(&read_request) && i<2000;++i) {
        struct timespec t={.tv_nsec=1000000};nanosleep(&t,NULL);
    }
    assert(atomic_load(&read_request));check_read(owner,b,8);
    assert(solar_os_ble_peer_disconnect(owner,a)==ESP_OK);
    assert(pthread_join(thread,NULL)==0 && call.result==SOLAR_OS_BLE_ERR_CANCELLED);
    assert(solar_os_ble_peer_get_info(owner,a,&info)==ESP_ERR_INVALID_STATE);
    check_read(owner,b,8);check_read(other,c,9);
    atomic_store(&delayed_epoch,0);
    bda[5]=4;assert(solar_os_ble_peer_connect(owner,bda,0,100,&extra)==ESP_OK && extra!=a);
    solar_os_ble_backend_event_t stale={.type=SOLAR_OS_BLE_BACKEND_RETIRED,.epoch=epoch_a};
    solar_os_ble_service_event(&stale);check_read(owner,extra,7);
    /* Timeout retires only the affected peer. */
    atomic_store(&delayed_epoch,find_locked(b)->link.epoch);
    uint8_t value;
    assert(solar_os_ble_peer_read(owner,b,3,&value,1,&count,1)==ESP_ERR_TIMEOUT);
    check_read(other,c,9);check_read(owner,extra,7);
    assert(solar_os_ble_peer_disconnect(owner,b)==ESP_OK);
    atomic_store(&delayed_epoch,0);
    /* Close immediately invalidates all owned handles, not another owner's. */
    defer_retirement=true;
    uint32_t last_epoch=find_locked(extra)->link.epoch;
    assert(solar_os_ble_session_close(owner)==ESP_OK);
    assert(solar_os_ble_peer_get_info(owner,extra,&info)==ESP_ERR_INVALID_STATE);
    assert(entries()==3);check_read(other,c,9);
    retire_peer(last_epoch);assert(entries()==2);defer_retirement=false;
    assert(solar_os_ble_prepare_sleep(1500)==ESP_OK);
    assert(solar_os_ble_peer_get_info(other,c,&info)==ESP_OK && !info.gatt.connected);
    solar_os_ble_resume();
    assert(solar_os_ble_peer_disconnect(other,c)==ESP_OK);
    assert(solar_os_ble_session_close(other)==ESP_OK && entries()==0);
    puts("BLE multi-peer: dynamic allocation, ownership, independent I/O, capacity, timeout, close and sleep OK");
}
