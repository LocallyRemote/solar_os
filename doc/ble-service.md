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
registration and existing pairing, bonding, and reconnect policy.

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

`solar_os_ble_bluedroid.c` owns generic GATT registrations, UUID conversion,
cache queries, and request submission. Each connection attempt uses a distinct
GATT application ID. Bluedroid callbacks lack a SolarOS request cookie, so the
adapter permits only one pending operation and retains its identity until
completion or retirement. Cancellation unregisters that generic application,
including when registration/open is pending. It does not unregister HID or
force-disconnect a shared physical keyboard link.

`ESP_GATTC_UNREG_EVT` is the retirement barrier. IDF 5.5 delivers it with a NULL
parameter; the adapter handles it before checking other callback data. The
adapter does not permit another registration until this barrier. New
registrations use new application IDs even after sleep; reused transport IDs
therefore do not suffice to identify an old lifetime. Failed registration has
no live transport and emits retirement directly.

Adapter calls enqueue Bluedroid work under an adapter mutex. That mutex is
released before delivering service events. Internal event delivery is
synchronous: the service copies borrowed read bytes before returning. It never
runs app or interpreter callbacks. A future host implementation must provide
the same retirement guarantee, not stamp late events with the latest request.

`solar_os_ble_keyboard.c` implements the backend's shared stack/GAP lifecycle
and keyboard HID profile. Legacy keyboard lifecycle and scan functions forward
to the general API. The service's static locks and bounded session storage do
not require heap allocations of their own. BLE boot policy still controls
whether the Bluetooth stack initializes.

## Existing GATT limits

- Read/write buffers remain limited to 128 bytes; discovery limits are unchanged.
- MTU exchange is requested during connection establishment. Connect waits for
  service discovery, not a separate MTU completion. Status reports the latest
  successful MTU event.
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
make -C tests/host ble_service_test ble_bluedroid_test ble_lua_bindings_test
tests/host/ble_service_test
tests/host/ble_bluedroid_test
tests/host/ble_lua_bindings_test
```

The session tests use actual pthread waits and a controlled backend to exercise
cross-task close/cancel, owner isolation, timeout quarantine, stale handles and
events, reused transport IDs, sleep, and compatibility calls. Adapter tests run
the actual Bluedroid adapter with controlled IDF callbacks, covering pending
registration/open cancellation, request correlation, enqueue failures, and the
NULL-parameter unregister barrier. Firmware builds compile against real IDF
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
