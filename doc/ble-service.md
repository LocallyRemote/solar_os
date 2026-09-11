# BLE sessions and backend boundary

`src/services/solar_os_ble.h` is the general BLE API. It exposes SolarOS session
handles, scan results, characteristic properties, discovery/value types, and
shared initialization and sleep/resume entry points. Bluetooth host-stack types
are confined to the backend.

## Ownership and lifetime

`solar_os_ble_session_create(owner, &session)` allocates an opaque, nonzero
session handle. The service supports four app sessions and a separate reserved
`ble.shell` session used by the existing `solar_os_ble_gatt_*` API. Owner names
are bounded diagnostic labels, not security credentials.

All sessions share **one generic peer connection**. A session can connect only
when that slot is free. Reads, writes, discovery queries, and cancellation
require the owning session handle. The compatibility API cannot read or
disconnect an app's connection. The OS keyboard retains its separate HID
connection and existing pairing, bonding, and reconnect policy.

A caller keeps the handle for its lifetime and calls
`solar_os_ble_session_close(session)` on every exit path, including errors.
Close invalidates the handle immediately. An in-flight call pins the underlying
slot and its result until its caller returns, so closing and recreating a
session cannot transfer the old result to a new owner. Numeric handles and
connection/request tokens never wrap within a boot; exhaustion fails closed.
These are runtime handles, not persistent identifiers.

`solar_os_ble_session_get_info()` reports the owner's connection snapshot, busy
state, and retirement state. Each session permits one blocking operation.
Other tasks may cancel or close that session while its caller waits.
The service serializes lifecycle transitions and backend submission; metadata
locks are released before backend calls. Blocking waits do not hold the
submission lock.

## Cancellation and timeouts

`solar_os_ble_session_cancel()` keeps the session handle but aborts its
connection and wakes a waiting caller with `SOLAR_OS_BLE_ERR_CANCELLED`.
Cancellation cannot undo a write already transmitted. Closing a session has
the same cancellation behavior and also invalidates the handle.

`solar_os_ble_session_set_cancel_check()` installs an optional cooperative
cancellation check while the session is idle. A waiting connect/read/write
caller runs it outside service locks at intervals of at most 50 ms (subject to
task scheduling). The check must not block or raise interpreter exceptions.
The context must remain valid until the operation returns. A true result uses
the same cancellation/retirement path as explicit cancellation. Sessions without
a check retain the normal blocking wait. Reused slots clear the old check.

A connect/read/write timeout retires the connection and returns
`ESP_ERR_TIMEOUT`. A new operation cannot reuse the connection while an old
request might still complete. Reconnect returns `ESP_ERR_INVALID_STATE` while
retirement is pending. If teardown submission fails, the slot stays reserved;
cancel or a later connect attempt retries submission. A missing terminal
callback never makes the slot reusable merely because time elapsed.

Sleep cancels generic operations and invalidates their connection lifetime.
Session handles survive sleep, but apps must reconnect and rediscover
characteristics after resume. The legacy blocking discovery scan is rejected
while a generic connection is active or retiring, so it cannot delay
cancellation of that client's requests. Keyboard scan/reconnect policy remains
in the HID profile.

## Backend event contract

`solar_os_ble.c` owns session identity, connection ownership, operation
completion, timeouts, discovery cache, and per-session read-result buffers.
The private `solar_os_ble_backend.h` interface carries connection lifetime
(`epoch`) and request tokens. The service checks epoch, request, operation
type, connection ID, and characteristic handle before accepting a completion.
It ignores events from cancelled or previous lifetimes.

The NimBLE backend uses two links: the OS keyboard and one generic client.
`solar_os_ble_nimble.c` owns generic discovery, UUID conversion, and request
submission. Application tasks copy requests into adapter-owned storage and
enqueue work on the NimBLE host queue. They do not enter the host while holding
an adapter lock. GAP/discovery callbacks carry immutable connection epochs;
read/write callbacks carry immutable request tokens.

A failed connect has no live transport and retires immediately. Cancellation
before host submission does not cancel another profile's connection attempt.
Cancellation after submission cancels that attempt or terminates its link.
NimBLE aborts outstanding ATT procedures before delivering GAP disconnect;
disconnect is the retirement barrier. The adapter also rejects old tokens if
a transport handle is reused. The host must stop before callback storage is
deinitialized.

Internal service-event delivery is synchronous: the service copies borrowed
read bytes before returning. It never invokes app or interpreter callbacks.

`solar_os_ble_keyboard.c` retains boot policy, scan selection, one remembered
keyboard, layout, input translation, pairing UI, and reconnect scheduling. It
owns shared NimBLE initialization, bond storage and sleep/resume. Public
addresses remain in display order; conversion to NimBLE byte order occurs only
at the transport boundary.

`solar_os_ble_hid.c` is SolarOS's bounded HID-over-GATT client; it does not use
ESP-IDF's HID host transport. Its asynchronous setup authenticates/encrypts,
negotiates MTU, discovers HID/battery services, reads report maps/references,
and subscribes to keyboard input and battery CCCDs. A bounded report-map
classifier identifies keyboard input IDs; the existing SolarOS key-report
decoder still interprets their payloads.

HID limits are five relevant HID/battery services, 64 characteristics per
service, 32 subscribed reports, a 2048-byte report map and 64-byte notification
payload. Report-map global nesting is limited to eight, collection nesting to
16; unsupported long items/local delimiters are rejected. Limits fail closed
instead of truncating discovery into an apparently ready keyboard.

Connection establishment is limited to three seconds, each discovery step to
ten seconds, passkey entry to 60 seconds, and the entire HID open to 90 seconds.
Sleep closes the admission gate before cancelling setup, so a racing worker
cannot start another connection. Workers are never forcibly deleted during
HID setup. Retirement waits for disconnect and delivery of the final policy
event. Input queue overflow disconnects the keyboard and releases pressed keys;
two queue slots are reserved for OPEN/CLOSE.

NimBLE and Bluedroid do not share bond storage. Existing keyboard preferences
and the remembered address remain, but keyboards previously bonded using
Bluedroid need to be forgotten/re-paired on both sides. Generic GATT databases
are rediscovered per connection, not persisted to NVS. Forget removes the
NimBLE bond synchronously and clears remembered state only after success or
confirmation that the bond is absent.

The firmware selects NimBLE central/observer roles only. Peripheral/server
support is a separate API increment. Existing local generated `sdkconfig.*`
files must also select `CONFIG_BT_NIMBLE_ENABLED=y` and disable
`CONFIG_BT_BLUEDROID_ENABLED`; repository defaults select NimBLE for fresh builds.

## Existing GATT limits

- Read/write buffers remain limited to 128 bytes. Discovery is bounded at 24
  services and 64 characteristics per service; exceeding a limit fails setup.
- MTU exchange completes before service/characteristic discovery. Status reports
  the negotiated MTU; writes must fit in MTU minus three bytes. A peer refusing
  exchange can continue using the default MTU.
- Writes with response wait for GATT completion. Writes without response wait
  for local stack completion, which does not acknowledge remote application
  receipt.
- Characteristic handles are valid for the connection that discovered them.
  Apps must not retain them across reconnects.
- Python/Lua expose synchronous client operations under `solaros.ble.gatt`,
  with a lazily allocated runtime-owned session and automatic cleanup before VM
  teardown. Cooperative checks use the existing stop/deadline signals. Legacy
  `solaros.ble.read()` still reads decoded keyboard input. Public event queues,
  notifications, GATT servers, and advertising are not exposed.
- `service.ble` selects the service, adapter, and keyboard profile under the
  existing package and board capability gates.

## Validation

Build and run the host tests:

```sh
make -C tests/host ble_service_test ble_nimble_test ble_hid_test ble_lua_bindings_test
tests/host/ble_service_test
tests/host/ble_nimble_test
tests/host/ble_hid_test
tests/host/ble_lua_bindings_test
```

The session tests use actual pthread waits and a controlled backend to exercise
cross-task close/cancel, owner isolation, timeout quarantine, stale handles and
events, reused transport IDs, sleep, and compatibility calls. Adapter and HID
tests execute the production state machines against controlled NimBLE callbacks,
including failed connection, cancellation before/after submission, stale epochs
and requests, discovery bounds, chained mbufs, binary writes, MTU limits,
CCCD subscription, report-map bounds and notification queue pressure.
Firmware builds compile against real IDF
headers. Cooperative checks are tested during connect, read, and write, including
callback reset when a slot is reused. The Lua test runs the actual interpreter
and bindings against the session service with a controlled radio backend. It
checks binary values, argument validation, ownership, VM cleanup, and stop during
a blocking read. Python has descriptor/source regression checks and firmware
build coverage; its runtime behavior also needs device validation.

Hardware checks should cover keyboard input/reconnect and sleep/wake, shell GATT
connect/discover/read/write, repeated disconnect/reconnect, and recovery after
a GATT timeout. Run discovery/read/write from both Python and Lua, stop a script
during a pending operation, and confirm the shell can subsequently reconnect
without rebooting. Host tests and successful builds do not establish those radio
and lifecycle results or live heap use.
