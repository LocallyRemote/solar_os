/* Reuse the threaded fake radio and run the service suite before exercising
 * the actual Lua bindings against the real session service. */
#define main ble_service_suite
#include "ble_service_test.c"
#undef main

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static void *solua_runner_control;
static atomic_bool stopped;
static bool solua_should_cancel(void *user) { (void)user; return atomic_load(&stopped); }
static int solua_check_esp(lua_State *L, esp_err_t err)
{
    return err == ESP_OK ? 0 : luaL_error(L, "BLE error %d", err);
}
static void solua_set_str(lua_State *L, int table, const char *key, const char *value)
{
    table = lua_absindex(L, table);
    lua_pushstring(L, value);
    lua_setfield(L, table, key);
}
static void solua_set_int(lua_State *L, int table, const char *key, lua_Integer value)
{
    table = lua_absindex(L, table);
    lua_pushinteger(L, value);
    lua_setfield(L, table, key);
}
static void solua_set_bool(lua_State *L, int table, const char *key, bool value)
{
    table = lua_absindex(L, table);
    lua_pushboolean(L, value);
    lua_setfield(L, table, key);
}

#include "solar_os_lua_ble.inc"

static lua_State *new_vm(void)
{
    lua_State *L = luaL_newstate();
    assert(L != NULL);
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    const luaL_Reg methods[] = {
        {"connect", solua_ble_gatt_connect}, {"disconnect", solua_ble_gatt_disconnect},
        {"status", solua_ble_gatt_status}, {"services", solua_ble_gatt_services},
        {"characteristics", solua_ble_gatt_characteristics}, {"read", solua_ble_gatt_read},
        {"write", solua_ble_gatt_write}, {NULL, NULL},
    };
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setglobal(L, "gatt");
    return L;
}

static void run(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK) {
        fprintf(stderr, "Lua test: %s\n", lua_tostring(L, -1));
        assert(false);
    }
}

static void *stop_waiting_vm(void *user)
{
    const unsigned *after = user;
    (void)await_submission(*after);
    atomic_store(&stopped, true);
    return NULL;
}

int main(void)
{
    assert(ble_service_suite() == 0);
    lua_State *L = new_vm();
    run(L,
        "gatt.disconnect(); "
        "assert(not pcall(gatt.connect, '01:02:03:04:05:06x', 1)); "
        "assert(not pcall(gatt.connect, '01:02:03:04:05:06' .. string.char(0), 1)); "
        "assert(not pcall(gatt.connect, '01:02:03:04:05:06', 4294967297)); "
        "assert(not pcall(gatt.connect, '01:02:03:04:05:06', 1, 4294967296)); "
        "gatt.connect('01:02:03:04:05:06', 1); "
        "local s = gatt.status(); "
        "assert(s.connected and not s.retiring and s.mtu == 247); "
        "assert(s.owner == 'lua.app' and s.address == '01:02:03:04:05:06'); "
        "assert(s.addr_type == 1 and s.max_value_bytes == 128); "
        "local services = gatt.services(); "
        "assert(#services == 24 and services[1].index == 0); "
        "assert(services[1].uuid == '0x180f' and services[1].primary); "
        "local chars = gatt.characteristics(services[1].index); "
        "assert(#chars == 1 and chars[1].handle == 3 and chars[1].properties == 2); "
        "assert(chars[1].uuid == '0x2a19'); "
        "assert(gatt.read(3) == string.rep('B', 128)); "
        "gatt.write(3, string.char(0, 255)); "
        "assert(not pcall(gatt.read, 0)); "
        "assert(not pcall(gatt.read, 65536)); "
        "assert(not pcall(gatt.read, -1)); "
        "assert(not pcall(gatt.read, 3, -1)); "
        "assert(not pcall(gatt.read, 3, 60001)); "
        "assert(not pcall(gatt.characteristics, -1)); "
        "assert(not pcall(gatt.characteristics, 4294967296)); "
        "assert(not pcall(gatt.write, 3, 123)); "
        "assert(not pcall(gatt.write, 3, '')); "
        "assert(not pcall(gatt.write, 3, string.rep('x', 129))); "
        "assert(not pcall(gatt.write, 3, 'xx', 0));");
    assert(write_response);
    run(L, "gatt.write(3, string.char(0, 255), false)");
    assert(!write_response);
    uint8_t value[2];
    size_t len;
    assert(solar_os_ble_gatt_read(3, value, sizeof(value), &len, 10) == ESP_ERR_INVALID_STATE);
    assert(solar_os_ble_gatt_disconnect() == ESP_OK);
    run(L, "assert(gatt.status().connected)");
    /* Stop interrupts a blocking native call, then blocks new work. */
    defer_read = true;
    const unsigned after = submission_count();
    pthread_t stopper;
    assert(pthread_create(&stopper, NULL, stop_waiting_vm, (void *)&after) == 0);
    run(L, "local ok, err = pcall(gatt.read, 3); assert(not ok and string.find(err, 'cancelled'))");
    assert(pthread_join(stopper, NULL) == 0);
    defer_read = false;
    run(L, "local ok, err = pcall(gatt.read, 3); assert(not ok and string.find(err, 'cancelled'))");
    const solar_os_ble_session_t old = solua_ble_session;
    solua_ble_destroy();
    solua_ble_destroy();
    lua_close(L);
    solar_os_ble_session_info_t info;
    assert(solar_os_ble_session_get_info(old, &info) == ESP_ERR_INVALID_STATE);
    retired();
    stopped = false;

    /* A new VM owns a new session; an uncaught exception can be cleaned up. */
    L = new_vm();
    run(L, "gatt.connect('01:02:03:04:05:06', 1)");
    assert(solua_ble_session != old);
    assert(luaL_dostring(L, "error('intentional')") != LUA_OK);
    solua_ble_destroy();
    lua_close(L);
    retired();
    assert(solar_os_ble_gatt_connect(peer, SOLAR_OS_BLE_ADDR_RANDOM, 100) == ESP_OK);

    /* A different owner cannot read or cancel the shell peer. */
    L = new_vm();
    run(L, "assert(not pcall(gatt.read, 3)); gatt.disconnect()");
    solua_ble_destroy();
    lua_close(L);
    assert(solar_os_ble_gatt_read(3, value, sizeof(value), &len, 100) == ESP_OK);
    assert(solar_os_ble_gatt_disconnect() == ESP_OK);
    retired();
    puts("Lua BLE bindings: real VM, binary I/O, validation, ownership and cleanup OK");
    return 0;
}
