# BLE service boundary

`src/services/solar_os_ble.h` is the general BLE API. It contains scan results,
address types, characteristic property flags, GATT discovery/value types, and
the shared initialization, scan, and sleep/resume entry points. It depends on
the common ESP error type but does not expose Bluetooth host-stack types.

`solar_os_ble.c` owns the existing synchronous GATT client state, service cache,
operation completion, timeouts, and read-result storage. It calls operations in
the private `solar_os_ble_backend.h` interface. That interface carries SolarOS
events with a normalized result and an optional backend diagnostic status.
The diagnostic status is used for messages, not service decisions.

`solar_os_ble_bluedroid.c` owns the generic client's Bluedroid registration,
GATT interface, UUID conversion, characteristic-cache queries, and request
submission. It translates callbacks into the private event interface. Event
delivery is synchronous and internal: the service consumes the event and copies
borrowed value bytes before returning. No application or interpreter callback
runs through this path. Service metadata locks are released before backend
operations are called.

`solar_os_ble_keyboard.c` implements the current backend's shared stack/GAP
lifecycle and keyboard HID profile. Its callback dispatcher routes HID events
to ESP-IDF HIDH and generic client events to the Bluedroid adapter. Keyboard
pairing, bonding, input translation, reconnect policy, and sleep teardown remain
in that profile implementation. The legacy keyboard lifecycle/scan functions
delegate to the general service API; its header also includes the general GATT
declarations for source compatibility. Disabling BLE for the current boot still
prevents stack initialization and service runtime allocation.

## Current contract

- The generic client has one shared connection and one outstanding operation.
  Callers must serialize its use, including initialization and sleep/resume.
  Lifecycle transitions must not overlap GATT requests. The compatibility API
  does not provide application ownership, cancellation, or generation-checked
  connection handles; `conn_id` is diagnostic transport information.
- Existing timeouts, 128-byte value buffering, discovery limits, scan behavior,
  and write-with/without-response behavior are retained. A write without a
  response reports submission success, not remote acknowledgement.
- MTU exchange is requested at connection establishment. The reported MTU
  reflects the latest successful exchange event; connecting does not explicitly
  wait for that event. The 128-byte API limit is independent of negotiated MTU.
- Python and Lua expose keyboard input only. Generic script sessions, public
  event queues, notification subscriptions, GATT servers, and advertising are
  not implemented by this API.
- `service.ble` selects the service, Bluedroid adapter, and keyboard profile
  together under the existing BLE board capability and package gates.

The private backend is selected at build time. An alternative host implements
its operations and event translation together with the keyboard/GAP lifecycle;
it does not require exposing its callback structures to service consumers.

## Validation

`make -C tests/host ble_service_test` builds the service with an independent
fake backend and counting semaphore stubs. Run `tests/host/ble_service_test` to
exercise initialization, discovery, MTU state, bounded/copy-owned read data,
operation errors, timeout, disconnection, and lifecycle forwarding. This test
does not validate radio behavior or concurrent callers.

Target acceptance requires keyboard pairing/input, reconnect with an absent
and present keyboard, sleep/wake, and the shell GATT connect/discover/read/write
path on physical hardware. Firmware builds and host tests do not establish
that acceptance or measure live Bluetooth heap use.
