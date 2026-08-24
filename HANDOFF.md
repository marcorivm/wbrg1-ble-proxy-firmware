# Handoff — connectable BLE proxy (Phase 3), 2026-08-22

## STATUS LEDs restored 2026-08-24 — pins identified, wired to link health
Both front LEDs are single active-high GPIOs on the WBRG1 (Tuya's old state/net
LEDs); our firmware never drove them, so they sat dark. Identified by an on-device
pad sweep: **red = PA25 (0x19), blue = PB22 (0x36)**. Now driven by ledInit()/
ledService() in the sketch: blue = link health (solid = WiFi+MQTT up, blink =
connecting, off = no WiFi), red = attention (off healthy, blink = WiFi down, solid
= safe-mode). Build 2026-08-24 13:51:58 passed the verify ritual (3x conn/disc, no
freeze); archived in experiments/known-good-20260824c-leds/. The scan/diag LED
commands were removed once the pins were known.

## PHASE 3 COMPLETE 2026-08-24 — connect + GATT read/write/notify all work
3c is implemented and verified twice end-to-end via the ESPHome API
(`tools/esphome_gatt_rw_test.py`): read (returns the char value), write with
response (acked), notify subscribe (CCCD write; notifications stream as msg 79
via an SPSC ring drained by the API task), unsubscribe, disconnect. Messages:
73/76→74, 75/77→83, 78→84, data→79, errors→82. Kickoffs run from the API task
blocking on `_rwSem` (same proven pattern as discovery); indications get
`client_attr_ind_confirm` in the callback. Ship build 2026-08-24 12:47:34,
archived in `experiments/known-good-20260824b/` (the earlier connect-only build
remains in `known-good-20260824/`). Advert queue capacity is 64 now (SRAM).

**HA integration re-enabled 2026-08-24 and validated:** ESPHome connected
(update entity live), and Bermuda's active proxy count went 3 -> 4 with the
WBRG1 streaming adverts into HA's Bluetooth stack. GATT connections will be
exercised organically when an HA integration needs one (the proxy advertises
active-connections + raw-advertisements, flags=35). Optional future cleanup:
strip the diag/spy instrumentation — but ONLY with the layout-sensitivity
verify ritual below, and there is no strong reason to.

## Previous milestone (2026-08-24 morning) — connections work; root cause narrowed
**State: BLE connect + MTU 247 + full GATT discovery work end-to-end through the
ESPHome API** (esphome_gatt_test.py passes; 4× connect/disconnect cycles, 60 s
held link, scanner pauses during a connection and resumes after — all verified).
Build 2026-08-24 12:30:03, archived with its exact toolchain artifacts in
`~/Projects/personal/wbrg1-ble/experiments/known-good-20260824/`
(ota bin, image2, axf, map, patched lib_arduino.a, sketch snapshot).

**What the bug was (as far as instrumentable):** on the first ACL send after a
connection (`att_send_mtu_req` → `l2cu_send_data` → `hci_send_pkt`, and
`btif_gatt_server_store_ind` persisting GATT state), the KM4 ends up stuck in a
context that even a priority-0 WDG IRQ cannot preempt — no fault, printf cut
mid-line, WiFi dead, only the hardware watchdog recovers. Systematically ruled
out with retained-SRAM spies across ~10 freeze cycles: osif_lock (depth 0),
kernel vPortEnterCritical (nest 0), FLASH_Write_Lock (state 0 — though its
`cpsid i` + IPC-to-KM0 + masked ACK spin at 0x48000204 remains the most
suspicious code in the image), the HCI UART ISR (inflag 0), and every hooked
NVIC vector. **The trigger is XIP code layout**: byte-identical configurations
freeze or work depending on where the flash-cached hot paths land; adding or
removing ~100 B of code flips it deterministically. The ship build works and
keeps ALL spies armed — if it ever freezes again, the next boot's MQTT log
prints girq/crt/flk/lck lines naming the context.

**Rules for future rebuilds (IMPORTANT):**
- Any source or library change can reshuffle XIP layout and re-trigger the
  freeze. After EVERY rebuild: OTA, then run at least 3× `conn`/`disc` diag
  cycles + `esphome_gatt_test.py` before trusting it. The safe-mode boot
  counter and watchdog make a bad build recoverable over OTA (2 failed BT
  inits → WiFi-only boot); a connect-freeze build just WDT-reboots.
- The known-good image can always be restored: OTA
  `experiments/known-good-20260824/wbrg1_ota.bin`, or UART-flash the archived
  `km0_km4_image2.bin`.
- lib_arduino.a is MODIFIED in the working tree (osif_lock/unlock and
  ftl_flash_write/erase weakened; vPortEnter/ExitCritical and
  FLASH_Write_Lock/Unlock renamed to real_*). Pristine copy: lib_arduino.a.orig.
  The sketch defines the strong replacements (spies + FTL flash no-op — the
  proxy keeps no persistent bonds by design).
- Recovery model for this board (learned the hard way): any CHIP_EN reset
  lands in ROM download mode (external flash stays in XIP mode; only power-on
  resets it). `pcycle.sh` (Plug R1) = boot; MQTT `uartburn` cmd = enter
  download mode; UART flashing via the Pico debugprobe on GP4/GP5
  (`tools/uart_flash.py` from flashtool/ with PYTHONPATH=$PWD;
  `tools/uart_bkup.py` reads/clears BKUP_REG0 at 0x480003C0;
  `tools/uart_erase.py` wipes an OTA slot signature).

**Remaining Phase 3 work (3b/3c):** GATT read/write/notify handlers — the
kickoffs must run in loop() via the existing `_pendOp` marshalling
(CONNECTABLE_DESIGN.md §§3b/3c). Discovery already works. After implementing,
re-verify per the rebuild rules above, then re-enable the HA ESPHome
integration for the device (it was left disabled so tools can hold the single
API slot).

## STATUS 2026-08-22 evening — connect freezes the chip below the HCI layer; Phase 3 parked
**Symptom (deterministic, reproduced ~15×):** any `le_connect()` to any peer
(ESP32 test peripheral, a random nearby 18:de:50:… device) establishes the link
(peer sees it) and within ~1 s the **KM4 CPU stops dead**: polling printf cuts
mid-line, WiFi/ping die, no fault dump, the 8 s hardware watchdog reboots it.
Scanning (continuous, active, duplicates on) is rock solid.

**Ruled out on hardware (each one an OTA cycle with UART + ping + peer logs):**
- calling task (marshalled to `loop()`), scan running vs stopped, peer identity
- `client_init()`/GATT registration (was broken: fixed), disconnect-path sends
- WiFi power-save off, WiFi disconnected, WiFi-side coex (`wifi_btcoex_set_bt_off`)
- BT-side coex hooks (rtk_coex.o replaced by no-ops via link override)
- BT controller patch: the core's (chip3, ver E8B87C92) AND Tuya's stock patch
  (entry 2, ver 4298EE6D, re-keyed to chip 3 — accepted by the controller once
  the FLATK write (0xFD91, rejected by the old patch) is tolerated by NOP'ing the
  status check at `hci_tp_config+0x30` in `lib_arduino.a(hci_adapter.o)`)
- BT config blob: byte-identical between Tuya's firmware and the core (both the
  11-byte `rtlbt_config` and the 25-byte fix-efuse config)
- flash/boot images (read back and verified), OTA slots

**Conclusion:** the stall is under the host stack — most likely the BT block's
low-power/clock handshake with the KM0 power core, i.e. the RTL8722 core's KM0
image/PMC running on an RTL8721CSM. Tuya's firmware (own KM0 image) connects fine.

**Options if resuming:**
1. ~~SWD~~ — **impossible on the WBRG1.** AmebaD SWD is fixed at SWDIO=PA27
   (module pin 33 `GNT`) / SWCLK=PB3, and PB3 is NOT bonded out of the module
   (checked against the datasheet pad diagram, 2026-08-22). Don't retry.
2. KM0 transplant: build image2 with Tuya's KM0 part (dump @0x6000, 90656 B)
   + our KM4 part. IPC ABI mismatch risk → boot hang → UART recovery.
3. Park Phase 3 (current state). Scanner + Bermuda unaffected; ESP32 proxies
   do the GATT work.

**Recovery lessons (paid for today):**
- The watchdog must be stopped around `http_update_ota` (blocks loop ~15 s) and
  must start BEFORE `BLE.init()` (a hung BT init otherwise sticks forever).
- Boot safety net: `BKUP_REG5` counter; 2 consecutive BT-init failures → boot
  WiFi/MQTT-only (SAFE-MODE in boot marker) so OTA stays possible.
- With the Pico debugprobe wired, its UART side holds PA7 low at reset → every
  reset lands in ROM download mode. To boot: unplug LOG_TX, tap CHIP_EN, replug.
- The smart-plug `pcycle.sh` only works if the gateway is actually on that plug.
- Bootloader can prefer the OTA2 slot; a bad image there is wiped with
  `tools/uart_erase.py <port> 0x106000 0x1000` from ROM download mode.
- `uart_flash.py` must run from `flashtool/` with `PYTHONPATH=$PWD` (needs
  `rtltool.py` + `imgtool_flashloader_amebad.bin` in cwd).

**Tools added:** `tools/uart_erase.py`, `mqtt_log.py` (tail log/status/telemetry,
`--cmd`), `ota_trigger.py`, `esp32_reset.py`. Experiment artifacts (patch/coex
overrides, tolerant-init lib, disassembly) live in the non-git working tree
`~/Projects/personal/wbrg1-ble/experiments/` (Tuya blobs stay out of git).
Diag MQTT cmds: `scan on|off`, `conn <12hex> [type]`, `connw …` (WiFi off first),
`disc`, `ps off`, `coex off`, `reboot`.

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
