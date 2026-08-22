# Connectable BLE proxy — design & SDK map (phases 3b/3c)

Status: **3a done** (connection slots + connect/disconnect, deployed, no scanner
regression). **3b/3c** (GATT discovery + read/write/notify) designed below,
**not yet built/deployed** — it needs a real connectable BLE device to validate,
and shipping an unvalidated raw-SDK GATT client to the live proxy risks crashing
the BLE stack (and the scanner with it). Build + OTA + test in a loop with a device.

## Why raw SDK
The Arduino `BLEClient` discovers the full GATT table (services w/ handle ranges,
characteristics w/ decl+value handles + properties, descriptors) but stores it in
**private** members with no iterator and a private constructor — unreachable and
un-subclassable. So enumeration for HA must go through Realtek's GATT-client SDK
directly. `client_attr_read/write` are handle-based with arbitrary bytes, which
maps cleanly onto HA's handle model.

## SDK surface (profile_client.h)
```c
typedef uint8_t T_CLIENT_ID;
bool client_register_spec_client_cb(T_CLIENT_ID *out_id, const T_FUN_CLIENT_CBS *cbs);
typedef struct {
  P_FUN_DISCOVER_STATE_CB    discover_state_cb;    // (conn_id, T_DISCOVERY_STATE)
  P_FUN_DISCOVER_RESULT_CB   discover_result_cb;   // (conn_id, T_DISCOVERY_RESULT_TYPE, T_DISCOVERY_RESULT_DATA)
  P_FUN_READ_RESULT_CB       read_result_cb;       // (conn_id, cause, handle, value_size, p_value)
  P_FUN_WRITE_RESULT_CB      write_result_cb;      // (conn_id, T_GATT_WRITE_TYPE, handle, cause, credits)
  P_FUN_NOTIFY_IND_RESULT_CB notify_ind_result_cb; // (conn_id, notify, handle, value_size, p_value) -> T_APP_RESULT
  P_FUN_DISCONNECT_CB        disconnect_cb;        // (conn_id)
} T_FUN_CLIENT_CBS;
T_GAP_CAUSE client_all_primary_srv_discovery(conn_id, client_id);
T_GAP_CAUSE client_all_char_discovery(conn_id, client_id, start_handle, end_handle);
T_GAP_CAUSE client_all_char_descriptor_discovery(conn_id, client_id, start_handle, end_handle);
T_GAP_CAUSE client_attr_read(conn_id, client_id, handle);
T_GAP_CAUSE client_attr_write(conn_id, client_id, write_type, handle, length, p_data);
T_GAP_CAUSE client_attr_ind_confirm(conn_id);  // confirm indications
```

Discovery result union (from BLEClient.cpp, the working template):
- `DISC_RESULT_ALL_SRV_UUID16/128`: `p_srv_uuidNN_disc_data->{att_handle, end_group_handle, uuidNN}`
- `DISC_RESULT_CHAR_UUID16/128`: `p_char_uuidNN_disc_data->{decl_handle, value_handle, properties, uuidNN}`
- `DISC_RESULT_CHAR_DESC_UUID16/128`: `p_char_desc_uuidNN_disc_data->{handle, uuidNN}`

Discovery state machine (mirror `BLEClient::clientDiscoverStateCallbackDefault`):
`client_all_primary_srv_discovery` → on `DISC_STATE_SRV_DONE`, iterate services
calling `client_all_char_discovery(start,end)`; on `DISC_STATE_CHAR_DONE`, per
char call `client_all_char_descriptor_discovery`; when all done → discovery ready.

## 3b — GATT get services
- Register ONE spec client at boot; get `client_id`.
- Per connection (keyed by conn_id): a table of services `{uuid, start, end}`,
  chars `{uuid, decl_handle, value_handle, properties}`, descriptors `{uuid, handle}`.
- On `BluetoothGATTGetServicesRequest(70)`: run the discovery state machine (block
  the API task on a semaphore the discover_state_cb signals when done), then emit
  `BluetoothGATTGetServicesResponse(71)` — nested services→chars→descriptors with
  the **real ATT handles** — followed by `BluetoothGATTGetServicesDoneResponse(72)`.
  Watch message size; chunk into multiple (71) frames if needed.

## 3c — read / write / notify
- `BluetoothGATTReadRequest(73)`: `client_attr_read(conn,id,handle)`; block on a
  read semaphore; `read_result_cb` fills a buffer; reply `BluetoothGATTReadResponse(74)`
  (or `BluetoothGATTErrorResponse(82)` on non-zero cause).
- `BluetoothGATTWriteRequest(75)`: `client_attr_write(conn,id, response?REQ:CMD,
  handle, len, data)`; block on write semaphore; reply `BluetoothGATTWriteResponse(83)`.
- `BluetoothGATTNotifyRequest(78)`: write the char's CCCD (find descriptor 0x2902,
  `client_attr_write` value `0x0001` notify / `0x0002` indicate, or `0x0000` off);
  reply `BluetoothGATTNotifyResponse(84)`. Incoming `notify_ind_result_cb` (BLE
  stack ctx) → push {handle, data} to an SPSC queue → API task drains and sends
  `BluetoothGATTNotifyDataResponse(79)` (same pattern as advert streaming). For
  indications, call `client_attr_ind_confirm`.

## Concurrency — the context-safety rule (learned the hard way)
**BLE *control* calls hard-fault the chip when made from the API socket task.**
Repeatably: `BLE.configScan()->stopScan()` + `connect()` called from the API task
crashed the device (took the scanner down with it). A 32 KB API-task stack alone
let `connect()` *return* (false) without crashing, but the link never reached
CONNECTED because the scanner was still running — and stopping the scanner from the
API task is exactly what crashes. Catch-22 → the fix below.

**Rule: all BLE control (scan start/stop, connect, disconnect, and — by extension —
the GATT discovery/read/write kickoff calls) must run in the `loop()` task context,
where `BLE.begin*` was initialised (matching the Arduino examples). The API socket
task may only (a) touch the socket and (b) marshal BLE work to `loop()`.**

Marshalling pattern now implemented for connect/disconnect (see
`esphome_api.cpp: serviceBleOp / execConnectLoop / execDisconnectLoop`):
- API task fills `_pendAddr/_pendType`, sets `_pendOp` (1=connect, 2=disconnect),
  then `xSemaphoreTake(_opSem, timeout)` — blocks.
- `loop()` calls `espApi.serviceBleOp()` every pass; when `_pendOp!=0` it runs the
  op **in the loop task**, writes `_pendOk/_pendConnId`, clears `_pendOp`, gives
  `_opSem`. API task wakes, builds the response, sends on the socket (its own ctx).
- Callbacks (discovery/read/write/notify results) still fire in the BLE-stack ctx
  and only give semaphores / push to queues — never send on the socket, never call
  BLE control from there either (e.g. scan-resume-on-disconnect must be deferred to
  `loop()`, not done inside `disconnect_cb`).

**3b/3c must extend the SAME marshalling:** discovery, read, write and notify-enable
each need a `_pendOp` code so their SDK kickoff (`client_all_primary_srv_discovery`,
`client_attr_read/write`) runs in `loop()`, not the API task. The result callbacks
can stay as-is (they already just signal). Notifications marshal inbound via the
existing advert-style SPSC queue → API task drains → sends msg 79.

- MTU: report the negotiated MTU in `BluetoothDeviceConnectionResponse` (exchange
  MTU on connect; `client_attr_read` returns up to MTU-1). 3a currently reports 247
  as a placeholder — refine to the real value.

## Test targets
SwitchBot Meter/Bot or Xiaomi LYWSD03MMC (their HA integrations open GATT
connections), or an ESP32/nRF flashed as a GATT peripheral with a readable +
notify characteristic (ideal controlled target).
