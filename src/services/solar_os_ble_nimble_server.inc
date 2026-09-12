/* NimBLE peripheral transport, included in the adapter to share its host queue
 * and lock. All stack calls run on that queue. Public requests/events are copied
 * and contain no NimBLE pointers; service dispatch pins owner/host lifetime. */
typedef struct server_char {
    struct server_char *next;
    ble_uuid_any_t uuid;
    uint32_t id;
    uint16_t handle;
    size_t index;
    uint8_t properties;
    size_t len;
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
} server_char_t;
typedef struct server_service {
    struct server_service *next;
    ble_uuid_any_t uuid;
    uint32_t id;
    server_char_t *chars;
    struct ble_gatt_chr_def *definitions;
} server_service_t;
typedef struct server_peer {
    struct server_peer *next;
    uint32_t id;
    uint16_t conn, indication_handle;
    bool retiring;
    uint8_t subscriptions[]; /* Indexed by immutable local characteristic ID. */
} server_peer_t;
typedef struct {
    uint32_t owner, epoch;
    bool registered, closing, advertising, wants_advertising;
    char name[27];
    server_service_t *services;
    server_peer_t *peers;
    server_peer_t *pending_peer; /* Reserved before advertising; connect callback cannot allocate. */
    size_t service_count, char_count, peer_count, untracked;
    struct ble_gatt_svc_def *definitions;
    solar_os_ble_server_event_t *events;
    size_t capacity, head, count;
    uint32_t dropped;
} server_state_t;
typedef struct {
    uint32_t owner;
    unsigned refs;
    bool done, abandoned;
    esp_err_t result;
    StaticSemaphore_t storage;
    SemaphoreHandle_t wake;
    solar_os_ble_server_request_t request;
} server_command_t;
static server_state_t *peripheral;
static server_command_t *server_command;
static uint32_t server_peer_ticket;
static uint32_t server_epoch_ticket;
static uint32_t server_local_ticket;
static int server_gap(struct ble_gap_event *event, void *arg);

static size_t server_used_locked(void)
{
    return peripheral ? peripheral->peer_count + peripheral->untracked +
        (peripheral->advertising ? 1 : 0) : 0;
}
static bool server_idle_locked(void) { return !peripheral && !server_command; }
static size_t server_central_count(void)
{
    size_t count = 0;
    for (ble_client_t *c = clients; c; c = c->next) count++;
    return count;
}
static server_char_t *server_characteristic(uint32_t id, uint16_t handle)
{
    if (peripheral) for (server_service_t *s = peripheral->services; s; s = s->next)
        for (server_char_t *c = s->chars; c; c = c->next)
            if ((id && c->id == id) || (handle && c->handle == handle)) return c;
    return NULL;
}
static server_peer_t *server_peer(uint32_t id, uint16_t conn)
{
    if (peripheral) for (server_peer_t *p = peripheral->peers; p; p = p->next)
        if ((id && p->id == id) || (!id && p->conn == conn)) return p;
    return NULL;
}
static void server_drop(void)
{
    if (peripheral->dropped != UINT32_MAX) peripheral->dropped++;
}
static bool server_event(solar_os_ble_server_event_t *event)
{
    if (!peripheral || peripheral->closing) return false;
    if (peripheral->count == peripheral->capacity) { server_drop(); return false; }
    size_t index = (peripheral->head + peripheral->count++) % peripheral->capacity;
    peripheral->events[index] = *event;
    return true;
}
static void server_free(void)
{
    if (!peripheral) return;
    while (peripheral->services) {
        server_service_t *s = peripheral->services;
        peripheral->services = s->next;
        while (s->chars) { server_char_t *c = s->chars; s->chars = c->next; free(c); }
        free(s->definitions); free(s);
    }
    while (peripheral->peers) {
        server_peer_t *p = peripheral->peers; peripheral->peers = p->next; free(p);
    }
    free(peripheral->pending_peer);
    free(peripheral->definitions); free(peripheral->events); free(peripheral); peripheral = NULL;
}
static void server_command_release(server_command_t *c)
{
    if (!--c->refs) { vSemaphoreDelete(c->wake); free(c); }
}
static void server_reset_locked(void)
{
    /* Host stopped and callbacks drained; NimBLE no longer retains definitions. */
    server_free();
    if (server_command) {
        server_command_t *c = server_command; server_command = NULL;
        c->done = true; c->result = SOLAR_OS_BLE_ERR_CANCELLED;
        xSemaphoreGive(c->wake); server_command_release(c);
    }
}
static void server_retire(void)
{
    if (!peripheral || !peripheral->closing) return;
    peripheral->wants_advertising = false;
    if (peripheral->advertising) {
        int rc = ble_gap_adv_stop();
        if (rc && rc != BLE_HS_EALREADY) return;
        peripheral->advertising = false;
    }
    for (server_peer_t *p = peripheral->peers; p; p = p->next) {
        if (!p->retiring && !ble_gap_terminate(p->conn, BLE_ERR_REM_USER_CONN_TERM)) p->retiring = true;
    }
    if (peripheral->peer_count || peripheral->untracked) return;
    if (peripheral->registered) {
        for (server_service_t *s = peripheral->services; s; s = s->next) {
            int rc = ble_gatts_delete_svc(&s->uuid.u);
            if (rc && rc != BLE_HS_ENOENT) return; /* Retain definitions on teardown failure. */
        }
    }
    server_free();
}
static int server_advertise(void)
{
    if (!peripheral || peripheral->closing || !peripheral->wants_advertising ||
        peripheral->advertising) return 0;
    if (server_central_count() + peripheral->peer_count + peripheral->untracked >=
        solar_os_ble_backend_capacity()) return BLE_HS_EBUSY;
    if (server_peer_ticket == UINT32_MAX) return BLE_HS_ENOMEM;
    if (!peripheral->pending_peer) {
        peripheral->pending_peer = calloc(1, sizeof(server_peer_t) + peripheral->char_count + 1);
        if (!peripheral->pending_peer) return BLE_HS_ENOMEM;
    }
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    ble_uuid_any_t *uuid = &peripheral->services->uuid;
    if (uuid->u.type == BLE_UUID_TYPE_16) {
        fields.uuids16 = &uuid->u16; fields.num_uuids16 = 1;
    } else if (uuid->u.type == BLE_UUID_TYPE_32) {
        fields.uuids32 = &uuid->u32; fields.num_uuids32 = 1;
    } else {
        fields.uuids128 = &uuid->u128; fields.num_uuids128 = 1;
    }
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) return rc;
    memset(&fields, 0, sizeof(fields));
    fields.name = (uint8_t *)peripheral->name;
    fields.name_len = strlen(peripheral->name); fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&fields);
    if (rc) return rc;
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND; params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &params,
        server_gap, (void *)(uintptr_t)peripheral->epoch);
    peripheral->advertising = rc == 0;
    return rc;
}
static int server_access(uint16_t conn, uint16_t handle, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    lock();
    server_char_t *c = arg;
    server_peer_t *p = server_peer(0, conn);
    if (!peripheral || peripheral->closing || !p || p->retiring) { unlock(); return BLE_ATT_ERR_UNLIKELY; }
    solar_os_ble_server_event_t event = {.peer=p->id, .characteristic=c->id, .mtu=ble_att_mtu(conn)};
    int rc = 0;
    if (ctx->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        /* NimBLE applies Read Blob offsets after the access callback. */
        if (ctx->offset > c->len) rc = BLE_ATT_ERR_INVALID_OFFSET;
        else if (os_mbuf_append(ctx->om, c->value, c->len)) rc = BLE_ATT_ERR_INSUFFICIENT_RES;
        else { event.type = SOLAR_OS_BLE_SERVER_READ; (void)server_event(&event); }
    } else if (ctx->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        event.type = SOLAR_OS_BLE_SERVER_WRITE; event.value_len = OS_MBUF_PKTLEN(ctx->om);
        if (ctx->offset) rc = BLE_ATT_ERR_INVALID_OFFSET;
        else if (event.value_len > sizeof(event.value)) rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        else if (os_mbuf_copydata(ctx->om, 0, event.value_len, event.value)) rc = BLE_ATT_ERR_UNLIKELY;
        else if (!server_event(&event)) rc = BLE_ATT_ERR_INSUFFICIENT_RES;
        else { memcpy(c->value, event.value, event.value_len); c->len = event.value_len; }
    } else rc = BLE_ATT_ERR_UNLIKELY;
    (void)handle;
    unlock();
    return rc;
}
static int server_gap(struct ble_gap_event *event, void *arg)
{
    lock();
    if (!peripheral || peripheral->epoch != (uint32_t)(uintptr_t)arg) { unlock(); return 0; }
    solar_os_ble_server_event_t e = {0};
    server_peer_t *p = NULL;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        peripheral->advertising = false;
        if (!event->connect.status) {
            p = peripheral->pending_peer; peripheral->pending_peer = NULL;
            if (!p) {
                peripheral->untracked++;
                server_drop();
                (void)ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
            p->id = ++server_peer_ticket; p->conn = event->connect.conn_handle;
            p->next = peripheral->peers; peripheral->peers = p; peripheral->peer_count++;
            if (peripheral->closing || server_central_count() + peripheral->peer_count +
                peripheral->untracked > solar_os_ble_backend_capacity()) {
                p->retiring = !ble_gap_terminate(p->conn, BLE_ERR_REM_USER_CONN_TERM);
            } else {
                e.type = SOLAR_OS_BLE_SERVER_CONNECTED; e.peer = p->id; e.mtu = ble_att_mtu(p->conn);
                (void)server_event(&e);
            }
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        p = server_peer(0, event->disconnect.conn.conn_handle);
        if (p) {
            e.type = SOLAR_OS_BLE_SERVER_DISCONNECTED; e.peer = p->id; e.status = event->disconnect.reason;
            (void)server_event(&e);
            server_peer_t **entry = &peripheral->peers;
            while (*entry && *entry != p) entry = &(*entry)->next;
            if (*entry) *entry = p->next;
            peripheral->peer_count--; free(p);
        } else if (peripheral->untracked) peripheral->untracked--;
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        peripheral->advertising = false;
        /* Unexpected controller stop is observable; do not spin-retry errors. */
        peripheral->wants_advertising = false;
        break;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        p = server_peer(0, event->subscribe.conn_handle);
        server_char_t *c = server_characteristic(0, event->subscribe.attr_handle);
        if (p && c) {
            p->subscriptions[c->index] = (event->subscribe.cur_notify ? 1 : 0) |
                (event->subscribe.cur_indicate ? 2 : 0);
            e.type = SOLAR_OS_BLE_SERVER_SUBSCRIBE; e.peer = p->id; e.characteristic = c->id;
            e.notify = event->subscribe.cur_notify; e.indicate = event->subscribe.cur_indicate;
            (void)server_event(&e);
        }
        break;
    }
    case BLE_GAP_EVENT_NOTIFY_TX: {
        p = server_peer(0, event->notify_tx.conn_handle);
        server_char_t *c = server_characteristic(0, event->notify_tx.attr_handle);
        if (p && c && (!event->notify_tx.indication || event->notify_tx.status)) {
            if (event->notify_tx.indication) p->indication_handle = 0;
            e.type = SOLAR_OS_BLE_SERVER_SENT; e.peer = p->id; e.characteristic = c->id;
            e.indicate = event->notify_tx.indication;
            e.status = event->notify_tx.status == BLE_HS_EDONE ? 0 : event->notify_tx.status;
            (void)server_event(&e);
        }
        break;
    }
    default: break;
    }
    /* Defer registration, advertising restart and deletion out of GAP callbacks. */
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    unlock();
    return 0;
}
static bool server_uuid(const char *text, ble_uuid_any_t *uuid)
{
    size_t n = strlen(text);
    if (n == 6 && text[0] == '0' && text[1] == 'x') { text += 2; n -= 2; }
    if (n != 4 && n != 8 && n != 36) return false;
    for (size_t i = 0; i < n; i++) {
        if (n == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
            if (text[i] != '-') return false;
        } else if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f') ||
                     (text[i] >= 'A' && text[i] <= 'F'))) return false;
    }
    return ble_uuid_from_str(uuid, text) == 0;
}
static int server_register_services(void)
{
    if (peripheral->registered) return 0;
    if (!peripheral->service_count) return BLE_HS_EINVAL;
    free(peripheral->definitions);
    peripheral->definitions = calloc(peripheral->service_count + 1, sizeof(*peripheral->definitions));
    if (!peripheral->definitions) return BLE_HS_ENOMEM;
    size_t index = 0;
    for (server_service_t *s = peripheral->services; s; s = s->next, index++) {
        uint16_t existing;
        if (!ble_gatts_find_svc(&s->uuid.u, &existing)) return BLE_HS_EALREADY;
        size_t count = 0;
        for (server_char_t *c = s->chars; c; c = c->next) count++;
        free(s->definitions); s->definitions = calloc(count + 1, sizeof(*s->definitions));
        if (!s->definitions) return BLE_HS_ENOMEM;
        peripheral->definitions[index] = (struct ble_gatt_svc_def){
            .type=BLE_GATT_SVC_TYPE_PRIMARY, .uuid=&s->uuid.u, .characteristics=s->definitions};
        size_t ci = 0;
        for (server_char_t *c = s->chars; c; c = c->next, ci++) {
            uint16_t flags = 0;
            if (c->properties & SOLAR_OS_BLE_CHAR_READ) flags |= BLE_GATT_CHR_F_READ;
            if (c->properties & SOLAR_OS_BLE_CHAR_WRITE) flags |= BLE_GATT_CHR_F_WRITE;
            if (c->properties & SOLAR_OS_BLE_CHAR_WRITE_NO_RESPONSE) flags |= BLE_GATT_CHR_F_WRITE_NO_RSP;
            if (c->properties & SOLAR_OS_BLE_CHAR_NOTIFY) flags |= BLE_GATT_CHR_F_NOTIFY;
            if (c->properties & SOLAR_OS_BLE_CHAR_INDICATE) flags |= BLE_GATT_CHR_F_INDICATE;
            s->definitions[ci] = (struct ble_gatt_chr_def){.uuid=&c->uuid.u, .flags=flags,
                .access_cb=server_access, .arg=c, .val_handle=&c->handle};
        }
    }
    int rc = ble_gatts_add_dynamic_svcs(peripheral->definitions);
    peripheral->registered = rc == 0;
    return rc;
}
static esp_err_t server_execute(uint32_t owner, solar_os_ble_server_request_t *r)
{
    if (r->op == SOLAR_OS_BLE_SERVER_CREATE) {
        if (peripheral || server_epoch_ticket == UINT32_MAX) return ESP_ERR_INVALID_STATE;
        size_t length = strlen(r->text), capacity = r->capacity ? r->capacity : 16;
        if (!length || length > 26 || capacity > SIZE_MAX / sizeof(solar_os_ble_server_event_t))
            return ESP_ERR_INVALID_ARG;
        server_state_t *s = calloc(1, sizeof(*s));
        if (!s) return ESP_ERR_NO_MEM;
        s->events = calloc(capacity, sizeof(*s->events));
        if (!s->events) { free(s); return ESP_ERR_NO_MEM; }
        s->owner = owner; s->epoch = ++server_epoch_ticket;
        s->capacity = capacity; memcpy(s->name, r->text, length + 1);
        peripheral = s; return ESP_OK;
    }
    if (!peripheral || peripheral->owner != owner) return ESP_ERR_INVALID_STATE;
    if (r->op == SOLAR_OS_BLE_SERVER_STATUS) {
        r->info = (solar_os_ble_server_info_t){.registered=peripheral->registered,
            .advertising=peripheral->advertising, .closing=peripheral->closing,
            .peers=peripheral->peer_count, .services=peripheral->service_count,
            .characteristics=peripheral->char_count, .event_capacity=peripheral->capacity,
            .event_count=peripheral->count, .events_dropped=peripheral->dropped};
        return ESP_OK;
    }
    if (peripheral->closing) return ESP_ERR_INVALID_STATE;
    if (r->op == SOLAR_OS_BLE_SERVER_PEER) {
        server_peer_t *found = NULL;
        for (server_peer_t *p = peripheral->peers; p; p = p->next)
            if (p->id > r->peer && (!found || p->id < found->id)) found = p;
        if (!found) return ESP_ERR_NOT_FOUND;
        r->event = (solar_os_ble_server_event_t){.peer=found->id,
            .mtu=ble_att_mtu(found->conn), .status=found->retiring ? 1 : 0};
        return ESP_OK;
    }
    if (r->op == SOLAR_OS_BLE_SERVER_CLOSE) { peripheral->closing = true; return ESP_OK; }
    if (r->op == SOLAR_OS_BLE_SERVER_POLL) {
        if (!peripheral->count) return ESP_ERR_NOT_FOUND;
        r->event = peripheral->events[peripheral->head];
        peripheral->head = (peripheral->head + 1) % peripheral->capacity; peripheral->count--;
        return ESP_OK;
    }
    if (r->op == SOLAR_OS_BLE_SERVER_SERVICE || r->op == SOLAR_OS_BLE_SERVER_CHARACTERISTIC) {
        if (peripheral->registered) return ESP_ERR_INVALID_STATE;
        if (server_local_ticket == UINT32_MAX) return ESP_ERR_NO_MEM;
        ble_uuid_any_t uuid;
        if (!server_uuid(r->text, &uuid)) return ESP_ERR_INVALID_ARG;
        if (r->op == SOLAR_OS_BLE_SERVER_SERVICE) {
            if (peripheral->service_count >= UINT16_MAX) return ESP_ERR_NO_MEM;
            server_service_t **entry = &peripheral->services;
            while (*entry) {
                if (!ble_uuid_cmp(&uuid.u, &(*entry)->uuid.u)) return ESP_ERR_INVALID_ARG;
                entry = &(*entry)->next;
            }
            server_service_t *s = calloc(1, sizeof(*s));
            if (!s) return ESP_ERR_NO_MEM;
            s->uuid = uuid; s->id = ++server_local_ticket; peripheral->service_count++; *entry = s; r->id = s->id;
        } else {
            if (!r->properties || (r->properties & ~0x3e)) return ESP_ERR_INVALID_ARG;
            if (peripheral->char_count >= UINT16_MAX) return ESP_ERR_NO_MEM;
            server_service_t *s = peripheral->services;
            while (s && s->id != r->parent) s = s->next;
            if (!s) return ESP_ERR_INVALID_ARG;
            server_char_t **entry = &s->chars;
            while (*entry) entry = &(*entry)->next;
            server_char_t *c = calloc(1, sizeof(*c));
            if (!c) return ESP_ERR_NO_MEM;
            c->uuid = uuid; c->id = ++server_local_ticket; c->index = peripheral->char_count++;
            c->properties = r->properties;
            c->len = r->value_len; memcpy(c->value, r->value, c->len); *entry = c; r->id = c->id;
        }
        return ESP_OK;
    }
    if (r->op == SOLAR_OS_BLE_SERVER_START) {
        int rc = server_register_services();
        if (rc) return solar_os_ble_nimble_error(rc);
        peripheral->wants_advertising = true;
        rc = server_advertise();
        if (rc) peripheral->wants_advertising = false;
        return solar_os_ble_nimble_error(rc);
    }
    if (r->op == SOLAR_OS_BLE_SERVER_STOP) {
        peripheral->wants_advertising = false;
        int rc = peripheral->advertising ? ble_gap_adv_stop() : 0;
        if (!rc || rc == BLE_HS_EALREADY) { peripheral->advertising = false; return ESP_OK; }
        return solar_os_ble_nimble_error(rc);
    }
    server_peer_t *p = server_peer(r->peer, BLE_HS_CONN_HANDLE_NONE);
    if (r->op == SOLAR_OS_BLE_SERVER_DISCONNECT) {
        if (!p || p->retiring) return ESP_ERR_INVALID_STATE;
        int rc = ble_gap_terminate(p->conn, BLE_ERR_REM_USER_CONN_TERM);
        if (!rc) p->retiring = true;
        return solar_os_ble_nimble_error(rc);
    }
    server_char_t *c = server_characteristic(r->id, 0);
    if (!c) return ESP_ERR_INVALID_ARG;
    if (r->op == SOLAR_OS_BLE_SERVER_SET) {
        memcpy(c->value, r->value, r->value_len); c->len = r->value_len; return ESP_OK;
    }
    if (r->op != SOLAR_OS_BLE_SERVER_SEND || !p || p->retiring) return ESP_ERR_INVALID_STATE;
    if (!(p->subscriptions[c->index] & (r->indicate ? 2 : 1)) || (r->indicate && p->indication_handle))
        return ESP_ERR_INVALID_STATE;
    uint16_t mtu = ble_att_mtu(p->conn);
    if (mtu < 3 || r->value_len > (size_t)(mtu - 3)) return ESP_ERR_INVALID_SIZE;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(r->value, r->value_len);
    if (!om) return ESP_ERR_NO_MEM;
    if (r->indicate) p->indication_handle = c->handle;
    int rc = r->indicate ? ble_gatts_indicate_custom(p->conn, c->handle, om) :
        ble_gatts_notify_custom(p->conn, c->handle, om); /* Both consume om on all outcomes. */
    if (rc && r->indicate) p->indication_handle = 0;
    return solar_os_ble_nimble_error(rc);
}
static void server_commands_locked(void)
{
    if (server_command) {
        server_command_t *c = server_command; server_command = NULL;
        c->result = c->abandoned ? SOLAR_OS_BLE_ERR_CANCELLED : server_execute(c->owner, &c->request);
        c->done = true; xSemaphoreGive(c->wake); server_command_release(c);
    }
    server_retire();
    if (peripheral && peripheral->registered) (void)server_advertise();
}
esp_err_t solar_os_ble_backend_server_request(solar_os_ble_session_t owner, solar_os_ble_server_request_t *r)
{
    if (!mutex || !command_ready) return ESP_ERR_INVALID_STATE;
    if (r->op == SOLAR_OS_BLE_SERVER_POLL || r->op == SOLAR_OS_BLE_SERVER_STATUS || r->op == SOLAR_OS_BLE_SERVER_SET) {
        /* These operations touch only copied adapter state, never host APIs. */
        lock(); esp_err_t result = server_execute(owner, r); unlock(); return result;
    }
    server_command_t *c = calloc(1, sizeof(*c));
    if (!c) return ESP_ERR_NO_MEM;
    c->owner = owner; c->refs = 2; c->request = *r;
    c->wake = xSemaphoreCreateBinaryStatic(&c->storage);
    lock();
    if (server_command) { c->refs = 1; server_command_release(c); unlock(); return ESP_ERR_INVALID_STATE; }
    server_command = c;
    unlock();
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    (void)xSemaphoreTake(c->wake, pdMS_TO_TICKS(5000));
    lock();
    esp_err_t result = c->done ? c->result : ESP_ERR_TIMEOUT;
    if (c->done) *r = c->request;
    else {
        c->abandoned = true;
        if (peripheral && peripheral->owner == owner) peripheral->closing = true;
    }
    server_command_release(c);
    unlock();
    return result;
}
void solar_os_ble_backend_server_cancel(solar_os_ble_session_t owner)
{
    if (!mutex || !command_ready) return;
    lock();
    if (peripheral && (!owner || peripheral->owner == owner)) peripheral->closing = true;
    if (server_command && (!owner || server_command->owner == owner)) server_command->abandoned = true;
    unlock();
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
}
