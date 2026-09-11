#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nimble_test_support.h"
#include "../../src/services/solar_os_ble_nimble.c"

static solar_os_ble_backend_event_t received[128];
static size_t received_count;
static uint8_t received_value[128];
void solar_os_ble_service_event(const solar_os_ble_backend_event_t *e)
{
    assert(received_count < 128);
    received[received_count++] = *e;
    if (e->value_len) memcpy(received_value, e->value, e->value_len);
}
int solar_os_ble_nimble_security(struct ble_gap_event *e) { (void)e; return 0; }
static struct ble_gatt_error ok, done = {.status=BLE_HS_EDONE};
static const uint8_t address[] = {0xa0,2,0xa5,0xcb,0xc6,0xf8};

static void begin(uint32_t epoch)
{
    received_count=0;
    nimble_test_reset();
    assert(solar_os_ble_backend_connect(epoch,epoch+1,address,0)==ESP_OK);
    assert(fake.connect_calls==0); /* Caller never calls into the host. */
    nimble_test_drain();
    assert(fake.connect_calls==1 && fake.address.val[0]==0xf8 && fake.address.val[5]==0xa0);
}
static void connected(uint32_t epoch)
{
    begin(epoch);
    nimble_test_connect(0);
    assert(received[0].type==SOLAR_OS_BLE_BACKEND_OPENED);
    fake.mtu_fn(7,&ok,517,fake.arg);
    struct ble_gatt_svc s={.start_handle=1,.end_handle=20,.uuid.u16={{16},0x1801}};
    fake.svc(7,&ok,&s,fake.arg);
    fake.svc(7,&done,NULL,fake.arg);
    fake.included(7,&done,NULL,fake.arg);
    struct ble_gatt_chr c={.def_handle=2,.val_handle=3,.properties=0x0e,.uuid.u16={{16},0x2222}};
    fake.chr(7,&ok,&c,fake.arg);
    fake.chr(7,&done,NULL,fake.arg);
    assert(received[received_count-1].type==SOLAR_OS_BLE_BACKEND_DISCOVERED);
    assert(received[received_count-1].result==ESP_OK);
}
static void retire(uint32_t epoch)
{
    assert(solar_os_ble_backend_cancel(epoch)==ESP_OK);
    nimble_test_drain();
    nimble_test_disconnect();
    assert(solar_os_ble_nimble_client_idle());
}
int main(void)
{
    solar_os_ble_backend_register();
    /* Cancellation before the queued connect must not cancel the HID attempt. */
    solar_os_ble_backend_connect(1,2,address,0);
    solar_os_ble_backend_cancel(1);
    nimble_test_drain();
    assert(!fake.connect_calls && !fake.cancel_calls && solar_os_ble_nimble_client_idle());
    begin(10);
    solar_os_ble_backend_cancel(10);
    nimble_test_drain();
    assert(fake.cancel_calls==1);
    nimble_test_connect(BLE_HS_EAPP);
    assert(solar_os_ble_nimble_client_idle());
    begin(20);
    nimble_test_connect(BLE_HS_ETIMEOUT);
    assert(received[0].result==ESP_ERR_TIMEOUT && solar_os_ble_nimble_client_idle());

    connected(30);
    solar_os_ble_gatt_service_t service={.start_handle=1,.end_handle=20};
    solar_os_ble_gatt_characteristic_t chars[2]; size_t count;
    assert(solar_os_ble_backend_characteristics(30,&service,chars,2,&count)==ESP_OK);
    assert(count==1 && chars[0].handle==3 && !strcmp(chars[0].uuid,"0x2222"));
    assert(solar_os_ble_backend_read(30,32,3)==ESP_OK);
    nimble_test_drain();
    void *old_request=fake.arg;
    uint8_t data[140]; for(size_t i=0;i<sizeof(data);++i)data[i]=i;
    struct os_mbuf tail={.len=70,.data=data+70}, head={.len=70,.data=data,.next=&tail};
    struct ble_gatt_attr attr={.handle=3,.om=&head};
    fake.attr(7,&ok,&attr,fake.arg);
    assert(received[received_count-1].value_len==128 && !memcmp(received_value,data,128));
    assert(solar_os_ble_backend_read(30,33,3)==ESP_OK);
    nimble_test_drain();
    size_t before=received_count;
    fake.attr(7,&ok,&attr,old_request);
    assert(received_count==before); /* Same connection and handle, older request. */
    fake.attr(7,&ok,&attr,fake.arg);
    assert(received[received_count-1].request==33);
    assert(solar_os_ble_backend_write(30,34,3,data,100,false)==ESP_OK);
    memset(data,0,sizeof(data));
    nimble_test_drain();
    assert(fake.written_len==100 && fake.written[99]==99);
    assert(received[received_count-1].type==SOLAR_OS_BLE_BACKEND_WRITTEN);
    fake.mtu=23;
    before=fake.write_calls;
    assert(solar_os_ble_backend_write(30,35,3,data,21,true)==ESP_OK);
    nimble_test_drain();
    assert(fake.write_calls==(int)before && received[received_count-1].result==ESP_ERR_INVALID_SIZE);
    retire(30);
    connected(40);
    before=received_count;
    value_callback(7,&ok,&attr,old_request);
    assert(received_count==before);
    retire(40);

    /* Explicit bounds failure, never overwrite the characteristic array. */
    begin(50); nimble_test_connect(0); fake.mtu_fn(7,&ok,517,fake.arg);
    struct ble_gatt_svc svc={.start_handle=1,.end_handle=200,.uuid.u16={{16},0x1801}};
    fake.svc(7,&ok,&svc,fake.arg); fake.svc(7,&done,NULL,fake.arg);
    fake.included(7,&done,NULL,fake.arg);
    struct ble_gatt_chr chr={.val_handle=3,.uuid.u16={{16},1}};
    for(size_t i=0;i<64;++i)assert(fake.chr(7,&ok,&chr,fake.arg)==0);
    assert(fake.chr(7,&ok,&chr,fake.arg)==BLE_HS_ENOMEM);
    assert(received[received_count-1].result==ESP_ERR_NO_MEM && fake.terminate_calls==1);
    nimble_test_disconnect();
    assert(solar_os_ble_nimble_client_idle());
    /* A leading service without includes must not skip later includes. Cycles
     * and repeated references do not grow the cache or loop forever. */
    begin(60); nimble_test_connect(0); fake.mtu_fn(7,&ok,517,fake.arg);
    svc.end_handle=20;
    fake.svc(7,&ok,&svc,fake.arg);
    svc.start_handle=21; svc.end_handle=40;
    fake.svc(7,&ok,&svc,fake.arg);
    fake.svc(7,&done,NULL,fake.arg);
    fake.included(7,&done,NULL,fake.arg);
    svc.start_handle=41; svc.end_handle=60;
    fake.included(7,&ok,&svc,fake.arg);
    assert(received[received_count-1].service.primary==false);
    fake.included(7,&ok,&svc,fake.arg);
    fake.included(7,&done,NULL,fake.arg);
    svc.start_handle=1; svc.end_handle=20;
    fake.included(7,&ok,&svc,fake.arg);
    fake.included(7,&done,NULL,fake.arg);
    for(size_t i=0;i<3;++i)fake.chr(7,&done,NULL,fake.arg);
    assert(client.count==3 && client.op==OP_NONE);
    retire(60);
    solar_os_ble_nimble_host_stopped();
    solar_os_ble_backend_reset();
    solar_os_ble_backend_register();
    connected(70); retire(70);
    puts("NimBLE adapter: cancellation, request identity, bounds, MTU and byte-copy tests passed");
}
