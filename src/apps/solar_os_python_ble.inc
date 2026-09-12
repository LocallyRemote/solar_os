/* Included inside the BLE package gate, after common interpreter helpers. */
static solar_os_ble_session_t python_ble_session;

static void python_ble_check(esp_err_t err)
{
    if (err == SOLAR_OS_BLE_ERR_CANCELLED) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("BLE operation cancelled"));
    }
    if (err == SOLAR_OS_BLE_ERR_CAPACITY) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("BLE connection capacity exhausted"));
    }
    python_check_esp(err);
}

static solar_os_ble_session_t python_ble_get(void)
{
    if (python_should_cancel(NULL)) {
        python_ble_check(SOLAR_OS_BLE_ERR_CANCELLED);
    }
    if (python_ble_session == SOLAR_OS_BLE_SESSION_INVALID) {
        python_ble_check(solar_os_ble_session_create(
            python_runner_control != NULL ? "python.runner" : "python.app",
            &python_ble_session));
        python_ble_check(solar_os_ble_session_set_cancel_check(
            python_ble_session, python_should_cancel, NULL));
    }
    return python_ble_session;
}

static void python_ble_destroy(void)
{
    if (python_ble_session != SOLAR_OS_BLE_SESSION_INVALID) {
        (void)solar_os_ble_session_close(python_ble_session);
        python_ble_session = SOLAR_OS_BLE_SESSION_INVALID;
    }
}

static uint32_t python_ble_timeout(size_t n_args, const mp_obj_t *args, size_t index)
{
    const uint32_t timeout = python_optional_u32(n_args, args, index, 0);
    if (timeout > 60000U) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout must be 0..60000 ms"));
    }
    return timeout;
}

static uint16_t python_ble_handle(mp_obj_t arg)
{
    const mp_int_t handle = mp_obj_get_int(arg);
    if (handle < 1 || handle > UINT16_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("handle must be 1..65535"));
    }
    return (uint16_t)handle;
}

static mp_obj_t solaros_ble_gatt_connect(size_t n_args, const mp_obj_t *args)
{
    size_t len;
    const char *address = mp_obj_str_get_data(args[0], &len);
    uint8_t bda[6];
    if (!solar_os_ble_parse_address(address, len, bda)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected aa:bb:cc:dd:ee:ff"));
    }
    const uint32_t type = python_optional_u32(n_args, args, 1, 0);
    if (type > SOLAR_OS_BLE_ADDR_RANDOM_IDENTITY) {
        mp_raise_ValueError(MP_ERROR_TEXT("address type must be 0..3"));
    }
    const uint32_t timeout = python_ble_timeout(n_args, args, 2);
    solar_os_ble_peer_t peer;
    python_ble_check(solar_os_ble_peer_connect(python_ble_get(), bda, type, timeout, &peer));
    return mp_obj_new_int_from_uint(peer);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_gatt_connect_obj, 1, 3, solaros_ble_gatt_connect);

static mp_obj_t solaros_ble_gatt_disconnect(mp_obj_t peer_obj)
{
    python_ble_check(solar_os_ble_peer_disconnect(python_ble_get(), python_u32_from_obj(peer_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_ble_gatt_disconnect_obj, solaros_ble_gatt_disconnect);

static mp_obj_t solaros_ble_gatt_capacity(void)
{
    return mp_obj_new_int_from_uint(solar_os_ble_peer_capacity());
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_gatt_capacity_obj, solaros_ble_gatt_capacity);

static mp_obj_t solaros_ble_gatt_status(mp_obj_t peer_obj)
{
    solar_os_ble_session_info_t info;
    python_ble_check(solar_os_ble_peer_get_info(python_ble_get(), python_u32_from_obj(peer_obj), &info));
    char address[18];
    snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x",
        info.gatt.bda[0], info.gatt.bda[1], info.gatt.bda[2],
        info.gatt.bda[3], info.gatt.bda[4], info.gatt.bda[5]);
    mp_obj_t result = mp_obj_new_dict(10);
    python_dict_store_cstr(result, "owner", info.owner);
    python_dict_store_cstr(result, "address", address);
    python_dict_store_cstr(result, "status", info.gatt.status);
    python_dict_store_bool(result, "connected", info.gatt.connected);
    python_dict_store_bool(result, "busy", info.busy);
    python_dict_store_bool(result, "retiring", info.retiring);
    python_dict_store_uint(result, "addr_type", info.gatt.addr_type);
    python_dict_store_uint(result, "mtu", info.gatt.mtu);
    python_dict_store_uint(result, "service_count", info.gatt.service_count);
    python_dict_store_uint(result, "max_value_bytes", SOLAR_OS_BLE_GATT_VALUE_MAX);
    python_dict_store_uint(result, "event_capacity", info.event_capacity);
    python_dict_store_uint(result, "event_count", info.event_count);
    python_dict_store_uint(result, "events_dropped", info.events_dropped);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_ble_gatt_status_obj, solaros_ble_gatt_status);

static mp_obj_t solaros_ble_gatt_services(mp_obj_t peer_obj)
{
    solar_os_ble_gatt_service_t services[SOLAR_OS_BLE_GATT_MAX_SERVICES];
    size_t count = 0;
    python_ble_check(solar_os_ble_peer_services(python_ble_get(), python_u32_from_obj(peer_obj), services,
        SOLAR_OS_BLE_GATT_MAX_SERVICES, &count));
    mp_obj_t result = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < count && i < SOLAR_OS_BLE_GATT_MAX_SERVICES; i++) {
        mp_obj_t item = mp_obj_new_dict(5);
        python_dict_store_uint(item, "index", i);
        python_dict_store_cstr(item, "uuid", services[i].uuid);
        python_dict_store_bool(item, "primary", services[i].primary);
        python_dict_store_uint(item, "start_handle", services[i].start_handle);
        python_dict_store_uint(item, "end_handle", services[i].end_handle);
        mp_obj_list_append(result, item);
    }
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_ble_gatt_services_obj, solaros_ble_gatt_services);

static mp_obj_t solaros_ble_gatt_characteristics(mp_obj_t peer_obj, mp_obj_t index_obj)
{
    const uint32_t index = python_u32_from_obj(index_obj);
    if (index >= SOLAR_OS_BLE_GATT_MAX_SERVICES) {
        mp_raise_ValueError(MP_ERROR_TEXT("service index out of range"));
    }
    solar_os_ble_gatt_characteristic_t chars[SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS];
    size_t count = 0;
    python_ble_check(solar_os_ble_peer_characteristics(python_ble_get(), python_u32_from_obj(peer_obj), index,
        chars, SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS, &count));
    mp_obj_t result = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < count && i < SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS; i++) {
        mp_obj_t item = mp_obj_new_dict(3);
        python_dict_store_cstr(item, "uuid", chars[i].uuid);
        python_dict_store_uint(item, "handle", chars[i].handle);
        python_dict_store_uint(item, "properties", chars[i].properties);
        mp_obj_list_append(result, item);
    }
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_ble_gatt_characteristics_obj, solaros_ble_gatt_characteristics);

static mp_obj_t solaros_ble_gatt_read(size_t n_args, const mp_obj_t *args)
{
    const uint16_t handle = python_ble_handle(args[1]);
    const uint32_t timeout = python_ble_timeout(n_args, args, 2);
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t len = 0;
    python_ble_check(solar_os_ble_peer_read(python_ble_get(), python_u32_from_obj(args[0]), handle,
        value, sizeof(value), &len, timeout));
    return mp_obj_new_bytes(value, len);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_gatt_read_obj, 2, 3, solaros_ble_gatt_read);

static mp_obj_t solaros_ble_gatt_write(size_t n_args, const mp_obj_t *args)
{
    const uint16_t handle = python_ble_handle(args[1]);
    mp_buffer_info_t data;
    mp_get_buffer_raise(args[2], &data, MP_BUFFER_READ);
    bool response = true;
    if (n_args > 3 && args[3] != mp_const_none) {
        if (args[3] != mp_const_true && args[3] != mp_const_false) {
            mp_raise_ValueError(MP_ERROR_TEXT("with_response must be boolean"));
        }
        response = args[3] == mp_const_true;
    }
    const uint32_t timeout = python_ble_timeout(n_args, args, 4);
    python_ble_check(solar_os_ble_peer_write(python_ble_get(), python_u32_from_obj(args[0]), handle,
        data.buf, data.len, response, timeout));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_gatt_write_obj, 3, 5, solaros_ble_gatt_write);

static mp_obj_t solaros_ble_gatt_subscribe(size_t n_args, const mp_obj_t *args)
{
    const uint16_t handle = python_ble_handle(args[1]);
    bool indication = false;
    if (n_args > 2 && args[2] != mp_const_none) {
        if (args[2] != mp_const_true && args[2] != mp_const_false)
            mp_raise_ValueError(MP_ERROR_TEXT("indicate must be boolean"));
        indication = args[2] == mp_const_true;
    }
    const uint32_t timeout = python_ble_timeout(n_args, args, 3);
    python_ble_check(solar_os_ble_peer_subscribe(python_ble_get(), python_u32_from_obj(args[0]),
        handle, indication ? 2 : 1, timeout));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_gatt_subscribe_obj, 2, 4, solaros_ble_gatt_subscribe);

static mp_obj_t solaros_ble_gatt_unsubscribe(size_t n_args, const mp_obj_t *args)
{
    const uint16_t handle = python_ble_handle(args[1]);
    const uint32_t timeout = python_ble_timeout(n_args, args, 2);
    python_ble_check(solar_os_ble_peer_subscribe(python_ble_get(), python_u32_from_obj(args[0]), handle, 0, timeout));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_gatt_unsubscribe_obj, 2, 3, solaros_ble_gatt_unsubscribe);

static mp_obj_t solaros_ble_gatt_configure_queue(mp_obj_t peer_obj, mp_obj_t capacity_obj)
{
    const uint32_t capacity = python_u32_from_obj(capacity_obj);
    python_ble_check(solar_os_ble_peer_configure_queue(python_ble_get(), python_u32_from_obj(peer_obj), capacity));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_ble_gatt_configure_queue_obj, solaros_ble_gatt_configure_queue);

static mp_obj_t solaros_ble_gatt_poll(mp_obj_t peer_obj)
{
    solar_os_ble_notification_t event;
    esp_err_t err = solar_os_ble_peer_poll(python_ble_get(), python_u32_from_obj(peer_obj), &event);
    if (err == ESP_ERR_NOT_FOUND) return mp_const_none;
    python_ble_check(err);
    mp_obj_t result = mp_obj_new_dict(3);
    python_dict_store_uint(result, "handle", event.handle);
    python_dict_store_bool(result, "indication", event.indication);
    mp_obj_dict_store(result, python_key("data"), mp_obj_new_bytes(event.value, event.value_len));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_ble_gatt_poll_obj, solaros_ble_gatt_poll);
