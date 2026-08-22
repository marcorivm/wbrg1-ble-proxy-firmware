# Handoff — connectable BLE proxy (Phase 3), 2026-08-22

## UPDATE 2026-08-22 afternoon — task-context was NOT the root cause
Validated on hardware with the marshalled build (connect runs in `loop()`):
- From `loop()`: `stopScan()` OK, `connect()` to a non-existent address OK
  (times out cleanly). Both driven by new MQTT diag commands (see below).
- Connect to the real ESP32 via the ESPHome API path → **still hard-faults**.
  So the crash happens when a link actually *establishes*, not because of the
  calling task. Root cause still unknown; needs the UART fault dump.
- Found & fixed on the way (all in the working tree, compiled, NOT flashed):
  1. `client_register_spec_client_cb` was called without `client_init()` →
     it failed silently (`_gattClientId` invalid). Now `EspHomeApi::initGatt()`
     does `client_init` + general cb + spec cb from `setup()`; boot log says `gatt=1`.
  2. `onDisconnect` (BLE-stack ctx) sent on the socket and called `startScan()`
     — violating our own rule. Now it only sets `_discMask`; the API task sends
     (`serviceDisconnects`) and `loop()` resumes scanning (`_resumeScan`).
  3. Hardware watchdog (8 s, `WDT` lib) added; kicked in `loop()` and inside the
     connect wait. A crash now reboots in ~25 s total. **It must be stopped
     around `http_update_ota`** (the pull blocks loop() ~15 s) — fixed in the
     working tree, but the image currently ON the device (build 12:01:45) has
     the watchdog without that fix → **it cannot be OTA'd; next flash is UART.**
- New tooling in `flashtool/`: `esp32_reset.py` (reset peripheral),
  `ota_trigger.py` (MQTT ota cmd), `mqtt_log.py [secs] --cmd "..."` (tails
  `wbrg1/<id>/log|status|telemetry`, optional command). Firmware publishes a
  boot marker (`boot: build <date> gatt=N heap=N`) and honours diag commands on
  the cmd topic, all executed in `loop()`: `scan off`, `scan on`,
  `conn <12hex> [type]`, `disc`.
- Next: wire the Pico (FLASHING.md), flash `km0_km4_image2.bin` with
  `tools/uart_flash.py`, keep the UART attached, reproduce the connect with
  `mqtt_log.py 20 --cmd "conn 582abd7d3152 0"` (no API involved) and read the
  fault dump; map the PC with `arm-elf-addr2line -e <cache>/application.axf`.
  If the diag connect works but the API path crashes, the bug is in
  `handleDeviceRequest`/the blocked API task, not BLE.

Read this first, then `CONNECTABLE_DESIGN.md` (§Concurrency has the crash rule).
This is a live-hardware task: build → OTA → power-cycle-to-recover → test, in a loop.

## Where we are
- **Scanner: done & deployed.** WBRG1 (RTL8721CSM inside a Tuya multi-mode gateway)
  streams BLE adverts to HA two ways in parallel: the MQTT scanner *and* a native
  ESPHome API server on :6053. Byte order validated (20/24 devices correlate with
  the ESP32 proxies). Active scan is on (names + ~2× samples, no Zigbee harm).
- **OTA-over-WiFi: works.** `flashtool/make_ota.py` wraps the Arduino image into the
  OTA_All container; the firmware pulls it on the MQTT `ota` command. This is the
  ONLY sane iteration path now (no wires). First-time/recovery flash is UART only.
- **Phase 3a (connect slots): deployed, no scanner regression.**
- **Phase 3 connectable (connect → GATT): IN PROGRESS. This is your task.**

## The one hard-won lesson (do not relearn it)
Calling **BLE control from the API socket task crashes the whole chip** (scanner
included). `stopScan()`+`connect()` from that task = repeatable hard-fault. The fix:
**run all BLE control in the `loop()` task**, with the API task only marshalling
requests and owning the socket. See `CONNECTABLE_DESIGN.md` §Concurrency.

## What I changed this session (compiles clean; NOT yet flashed/tested)
Marshalling infra for connect/disconnect, in the wbrg1-ble working tree
(`~/Projects/personal/wbrg1-ble/sketches/wbrg1_ble_proxy/`):
- `esphome_api.h`: added `serviceBleOp()` (public) + pending-op state
  (`_pendOp/_pendAddr/_pendType/_pendConnId/_pendOk`, `_opSem`) +
  `execConnectLoop()/execDisconnectLoop()`.
- `esphome_api.cpp`: `handleDeviceRequest` now marshals connect/disconnect to
  `loop()` instead of calling BLE inline; `serviceBleOp()` dispatches in loop ctx;
  `_opSem` created in `taskRun()`.
- `wbrg1_ble_proxy.ino`: `loop()` calls `espApi.serviceBleOp()` each pass.
- Built OK (901884 B, 43%). Fresh OTA image at `flashtool/wbrg1_ota.bin` (11:47).

## YOUR FIRST STEP — flash & validate the connect fix
1. Reset the ESP32 test peripheral to advertising (it's on the PC via USB serial;
   `/dev/cu.usbserial-0001`, BT MAC **58:2a:bd:7d:31:52**): pyserial
   `setDTR(False); setRTS(True); sleep .15; setRTS(False)` → run boot 0x13. It
   advertises service `12340000-…-34fb` with RD(…0001)/WR(…0002)/NT(…0003) chars.
2. Serve the OTA image + trigger it (see "Build & OTA" below). Watch it come back.
3. From the Mac, run `flashtool/esphome_gatt_test.py` (default target is the ESP32
   MAC) — it connects via the ESPHome API and dumps GATT.
   **Success for this step = device connects (connected=true) and does NOT crash.**
   That validates the loop-context marshalling hypothesis. The scanner should keep
   streaming throughout.
4. If it crashes anyway: `flashtool/pcycle.sh` power-cycles the gateway (~12 s back).

## THEN — finish 3b/3c (extend the SAME marshalling)
GATT discovery + read/write/notify are DESIGNED but not built. Each SDK kickoff
(`client_all_primary_srv_discovery`, `client_attr_read/write`, CCCD write) must run
in `loop()` too — add `_pendOp` codes for them exactly like connect. Result
callbacks already just signal semaphores (fine as-is). Notifications drain via an
SPSC queue → API task → msg 79. Full message map + SDK surface in
`CONNECTABLE_DESIGN.md` §§3b, 3c. Handler stubs (`handleGetServices`, discovery
storage `_svc/_chr/_dsc`, callbacks `onDiscState/onDiscResult`) already exist in
`esphome_api.cpp` — they need the kickoff calls moved into loop ctx.

## Strongly recommended before deep iteration
Add a **hardware watchdog** so a crash auto-reboots (~seconds) instead of needing a
manual/plug power-cycle. Combined with `pcycle.sh` this makes iteration painless.
(Realtek AmebaD: `watchdog_init` / `watchdog_start` / `watchdog_refresh`; refresh
from `loop()`.)

## Test hardware & controls
- **WBRG1 gateway**: live, WiFi. Recover with `flashtool/pcycle.sh` (toggles HA
  `switch.luces_porton_socket_1`, "Plug R1"). ~12 s to adverts.
- **ESP32-WROOM test peripheral**: on PC USB (`/dev/cu.usbserial-0001`). Keep it on
  the PC — you can reset it via serial AND read its logs (`[peer] connected`,
  `[write] N bytes`) to confirm the peripheral side during read/write/notify.
  Sketch: `~/Projects/personal/wbrg1-ble/sketches/ble_test_peripheral/`.
- **HA ESPHome integration** for this device is left DISABLED by the user so tools
  can hold the single API slot. Server is single-client — HA and a test script
  can't both connect. Re-enable only when validating end-to-end in HA.

## Build & OTA (the loop)
```
cd ~/Projects/personal/wbrg1-ble
arduino-cli compile --config-file arduino-cli.yaml \
  -b realtek:AmebaD:Ameba_AMB21_AMB22 sketches/wbrg1_ble_proxy
python3 flashtool/make_ota.py \
  arduino-data/packages/realtek/tools/ameba_d_tools/1.1.3/km0_km4_image2.bin \
  flashtool/wbrg1_ota.bin
python3 flashtool/ota_httpd.py &            # serves wbrg1_ota.bin, logs bytes
# trigger: publish MQTT "ota" cmd with host/port/resource to the device
# (see wbrg1_ble_proxy.ino OTA handler + sink_mqtt for the topic)
```
OTA targets the inactive slot (OTA1 0x6000 / OTA2 0x106000); `ota_get_cur_index`
picks it. A bad OTA is recovered over UART with `tools/uart_flash.py` (ROM
download mode: CHIP_EN + PA7 dance).

## Repos
- Firmware guide: this repo (github.com/marcorivm/wbrg1-ble-proxy-firmware).
  Working tree with secrets/live code: `~/Projects/personal/wbrg1-ble/`.
- HA integration: github.com/marcorivm/hass-wbrg1-ble-proxy.

## Gotchas already paid for
- WiFiClient's copy-dtor closes the socket → we use raw `ard_socket.h` in a task.
- This core's PubSubClient has `delay(500)` + inbound keepalive → `setKeepAlive(3600)`,
  drain every pass, timered MQTT service (don't gate `drainQueue()` behind it).
- PA7/LOG_TX is the ROM download strap (reset trap) — mind it when wiring serial.
- `getConnId(addr)` address match is unreliable → we also scan `BLE.connected(i)`
  slots to find a newly-connected conn_id.
