/* Included inside the BLE package gate, after common interpreter helpers. */
static solar_os_ble_session_t solua_ble_session;

static int solua_ble_check(lua_State *L, esp_err_t err)
{
    if (err == SOLAR_OS_BLE_ERR_CANCELLED) {
        return luaL_error(L, "BLE operation cancelled");
    }
    if (err == SOLAR_OS_BLE_ERR_CAPACITY) return luaL_error(L, "BLE connection capacity exhausted");
    return solua_check_esp(L, err);
}

static solar_os_ble_session_t solua_ble_get(lua_State *L)
{
    if (solua_should_cancel(NULL)) {
        solua_ble_check(L, SOLAR_OS_BLE_ERR_CANCELLED);
    }
    if (solua_ble_session == SOLAR_OS_BLE_SESSION_INVALID) {
        solua_ble_check(L, solar_os_ble_session_create(
            solua_runner_control != NULL ? "lua.runner" : "lua.app", &solua_ble_session));
        solua_ble_check(L, solar_os_ble_session_set_cancel_check(
            solua_ble_session, solua_should_cancel, NULL));
    }
    return solua_ble_session;
}

static void solua_ble_destroy(void)
{
    if (solua_ble_session != SOLAR_OS_BLE_SESSION_INVALID) {
        (void)solar_os_ble_session_close(solua_ble_session);
        solua_ble_session = SOLAR_OS_BLE_SESSION_INVALID;
    }
}

static uint32_t solua_ble_timeout(lua_State *L, int index)
{
    const lua_Integer timeout = luaL_optinteger(L, index, 0);
    if (timeout < 0 || timeout > 60000) {
        luaL_error(L, "timeout must be 0..60000 ms");
    }
    return (uint32_t)timeout;
}

static uint16_t solua_ble_handle(lua_State *L, int index)
{
    const lua_Integer handle = luaL_checkinteger(L, index);
    if (handle < 1 || handle > UINT16_MAX) {
        luaL_error(L, "handle must be 1..65535");
    }
    return (uint16_t)handle;
}

static solar_os_ble_peer_t solua_ble_peer(lua_State *L)
{
    const lua_Integer peer = luaL_checkinteger(L, 1);
    if (peer <= 0 || (uint64_t)peer > UINT32_MAX) luaL_error(L, "invalid BLE peer handle");
    return (solar_os_ble_peer_t)peer;
}

static int solua_ble_gatt_connect(lua_State *L)
{
    size_t len;
    luaL_checktype(L, 1, LUA_TSTRING);
    const char *address = luaL_checklstring(L, 1, &len);
    uint8_t bda[6];
    if (!solar_os_ble_parse_address(address, len, bda)) {
        return luaL_error(L, "expected aa:bb:cc:dd:ee:ff");
    }
    const lua_Integer type = luaL_optinteger(L, 2, 0);
    if (type < 0 || type > SOLAR_OS_BLE_ADDR_RANDOM_IDENTITY) {
        return luaL_error(L, "address type must be 0..3");
    }
    const uint32_t timeout = solua_ble_timeout(L, 3);
    solar_os_ble_peer_t peer;
    solua_ble_check(L, solar_os_ble_peer_connect(solua_ble_get(L), bda, type, timeout, &peer));
    lua_pushinteger(L, peer);
    return 1;
}

static int solua_ble_gatt_disconnect(lua_State *L)
{
    return solua_ble_check(L, solar_os_ble_peer_disconnect(solua_ble_get(L), solua_ble_peer(L)));
}

static int solua_ble_gatt_capacity(lua_State *L)
{
    lua_pushinteger(L, solar_os_ble_peer_capacity());
    return 1;
}

static int solua_ble_gatt_status(lua_State *L)
{
    solar_os_ble_session_info_t info;
    solua_ble_check(L, solar_os_ble_peer_get_info(solua_ble_get(L), solua_ble_peer(L), &info));
    char address[18];
    snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x",
        info.gatt.bda[0], info.gatt.bda[1], info.gatt.bda[2],
        info.gatt.bda[3], info.gatt.bda[4], info.gatt.bda[5]);
    lua_createtable(L, 0, 10);
    solua_set_str(L, -1, "owner", info.owner);
    solua_set_str(L, -1, "address", address);
    solua_set_str(L, -1, "status", info.gatt.status);
    solua_set_bool(L, -1, "connected", info.gatt.connected);
    solua_set_bool(L, -1, "busy", info.busy);
    solua_set_bool(L, -1, "retiring", info.retiring);
    solua_set_int(L, -1, "addr_type", info.gatt.addr_type);
    solua_set_int(L, -1, "mtu", info.gatt.mtu);
    solua_set_int(L, -1, "service_count", info.gatt.service_count);
    solua_set_int(L, -1, "max_value_bytes", SOLAR_OS_BLE_GATT_VALUE_MAX);
    solua_set_int(L, -1, "event_capacity", info.event_capacity);
    solua_set_int(L, -1, "event_count", info.event_count);
    solua_set_int(L, -1, "events_dropped", info.events_dropped);
    return 1;
}

static int solua_ble_gatt_services(lua_State *L)
{
    solar_os_ble_gatt_service_t services[SOLAR_OS_BLE_GATT_MAX_SERVICES];
    size_t count = 0;
    solua_ble_check(L, solar_os_ble_peer_services(solua_ble_get(L), solua_ble_peer(L), services,
        SOLAR_OS_BLE_GATT_MAX_SERVICES, &count));
    lua_createtable(L, count, 0);
    for (size_t i = 0; i < count && i < SOLAR_OS_BLE_GATT_MAX_SERVICES; i++) {
        lua_createtable(L, 0, 5);
        solua_set_int(L, -1, "index", i);
        solua_set_str(L, -1, "uuid", services[i].uuid);
        solua_set_bool(L, -1, "primary", services[i].primary);
        solua_set_int(L, -1, "start_handle", services[i].start_handle);
        solua_set_int(L, -1, "end_handle", services[i].end_handle);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int solua_ble_gatt_characteristics(lua_State *L)
{
    const lua_Integer index = luaL_checkinteger(L, 2);
    if (index < 0 || index >= SOLAR_OS_BLE_GATT_MAX_SERVICES) {
        return luaL_error(L, "service index out of range");
    }
    solar_os_ble_gatt_characteristic_t chars[SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS];
    size_t count = 0;
    solua_ble_check(L, solar_os_ble_peer_characteristics(solua_ble_get(L), solua_ble_peer(L), index,
        chars, SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS, &count));
    lua_createtable(L, count, 0);
    for (size_t i = 0; i < count && i < SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS; i++) {
        lua_createtable(L, 0, 3);
        solua_set_str(L, -1, "uuid", chars[i].uuid);
        solua_set_int(L, -1, "handle", chars[i].handle);
        solua_set_int(L, -1, "properties", chars[i].properties);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int solua_ble_gatt_read(lua_State *L)
{
    const uint16_t handle = solua_ble_handle(L, 2);
    const uint32_t timeout = solua_ble_timeout(L, 3);
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t len = 0;
    solua_ble_check(L, solar_os_ble_peer_read(solua_ble_get(L), solua_ble_peer(L), handle,
        value, sizeof(value), &len, timeout));
    lua_pushlstring(L, (const char *)value, len);
    return 1;
}

static int solua_ble_gatt_write(lua_State *L)
{
    const uint16_t handle = solua_ble_handle(L, 2);
    size_t len;
    luaL_checktype(L, 3, LUA_TSTRING);
    const char *value = luaL_checklstring(L, 3, &len);
    bool response = true;
    if (!lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TBOOLEAN);
        response = lua_toboolean(L, 4);
    }
    const uint32_t timeout = solua_ble_timeout(L, 5);
    return solua_ble_check(L, solar_os_ble_peer_write(solua_ble_get(L), solua_ble_peer(L), handle,
        (const uint8_t *)value, len, response, timeout));
}

static int solua_ble_gatt_subscribe(lua_State *L)
{
    const uint16_t handle = solua_ble_handle(L, 2);
    bool indication = false;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TBOOLEAN);
        indication = lua_toboolean(L, 3);
    }
    const uint32_t timeout = solua_ble_timeout(L, 4);
    return solua_ble_check(L, solar_os_ble_peer_subscribe(solua_ble_get(L), solua_ble_peer(L),
        handle, indication ? 2 : 1, timeout));
}

static int solua_ble_gatt_unsubscribe(lua_State *L)
{
    const uint16_t handle = solua_ble_handle(L, 2);
    const uint32_t timeout = solua_ble_timeout(L, 3);
    return solua_ble_check(L, solar_os_ble_peer_subscribe(solua_ble_get(L), solua_ble_peer(L), handle, 0, timeout));
}

static int solua_ble_gatt_configure_queue(lua_State *L)
{
    const lua_Integer capacity = luaL_checkinteger(L, 2);
    if (capacity <= 0 || (uint64_t)capacity > UINT32_MAX) return luaL_error(L, "invalid queue capacity");
    return solua_ble_check(L, solar_os_ble_peer_configure_queue(solua_ble_get(L), solua_ble_peer(L), capacity));
}

static int solua_ble_gatt_poll(lua_State *L)
{
    solar_os_ble_notification_t event;
    esp_err_t err = solar_os_ble_peer_poll(solua_ble_get(L), solua_ble_peer(L), &event);
    if (err == ESP_ERR_NOT_FOUND) { lua_pushnil(L); return 1; }
    solua_ble_check(L, err);
    lua_createtable(L, 0, 3);
    solua_set_int(L, -1, "handle", event.handle);
    solua_set_bool(L, -1, "indication", event.indication);
    lua_pushlstring(L, (const char *)event.value, event.value_len);
    lua_setfield(L, -2, "data");
    return 1;
}
