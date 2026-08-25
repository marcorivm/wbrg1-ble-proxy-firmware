# Handoff — WBRG1 BLE proxy fleet

## UPDATE 2026-08-25 (midday) — v7.1 fleet; scan-wedge VERDICT delivered
Both boards now run **v7.1** (this repo's sketch matches it — the sync-from-
archive requirement below is RESOLVED as of commits f0388ed/fc1cdbf).

**Scan-wedge root cause is settled: the BT HOST TASK stalls** (stops draining
its message queue) under sustained connect churn. Adverts die because that
task processes them; every soft fix (scan on/off, param reset, conn/disc
cycles) just posts to a queue nothing reads; and any SYNCHRONOUS GAP call
from another task then blocks forever. Proof: v7.1's soft-recovery ladder
persists its progress in retained SRAM (SFX_BASE 0x1007BFC0, printed as
`scanfix-prev:` next boot) — the first natural wedge produced all-sentinel
values: the ladder died inside its FIRST GAP call, reaped by the 8 s hardware
WDT. **Watchdog reboot is the only recovery** — the v6 scan-starvation
watchdog was already optimal; v7.1 keeps it and records a verdict at every
occurrence (~8 s slower than a direct reboot, via WDT).

Rates: ~1 wedge/hour on the board carrying GATT traffic (gw018, Airthings
endurance rig), zero on the idle one. Cost per event: ~40 s advert gap,
self-healed, HA rides through.

**Rule for all future firmware work: never call the GAP API synchronously
when the advert counter is starved — the caller hangs.**

New in v7/v7.1 (in this repo's sketch): scanwd soft-recovery ladder +
retained verdict; diags `scanstate` (GAP dev state) and `scanfix` (run ladder
on demand). Deploy verification is now BOOT-MARKER based (`ctrl log` after
OTA; a counter heuristic false-positived once).

Open items (revised):
1. (optional) v8: simplify scanwd back to direct reboot + retained wedge
   counter once 2-3 more wedges confirm identical sentinels.
2. OTA first-trigger-ignored quirk (unchanged, consistent).
3. Why the BT host task stalls under churn — deep-research, closed-binary
   stack; fenced by watchdogs, does not affect operation.
4. Offsite copies of the four Tuya backups.
5. Retire hass-wbrg1-ble-proxy (MQTT-era HACS integration, obsolete).


## FLEET STATE 2026-08-25 — GATT SOLVED AND DEPLOYED ON BOTH BOARDS (v6)

**The XIP connect-freeze is root-caused and fixed. GATT connect + discovery +
read/write/notify work on BOTH boards, verified end-to-end through HA.** The
"layout lottery" is closed; everything below the ruler line is history.

| board | name (renamed) | IP | firmware |
|---|---|---|---|
| JZZWG-TY2.0 | `wbrg1-jzzwg` (was wbrg1-gw1) | 192.168.0.175 | v6 fleet build |
| RSH-GW018-DM | `wbrg1-gw018` (was wbrg1-gw2) | 192.168.0.130 | v6 fleet build (+disc instrumentation) |

Source of truth for the deployed build:
`~/Projects/personal/wbrg1-ble/experiments/gw2-gatt-SOLVED-20260824/`
(v2-fleet/sketch-v6 = sketch snapshot; v6_*_ota.bin = deployed images; README
= full evidence chain, one section per iteration). Condensed engineering
record in this repo: `docs/GATT-FLEET.md`.
**The sketch in this repo (`wbrg1_ble_proxy/`) and the shared working tree
(`wbrg1-ble/sketches/wbrg1_ble_proxy/`) are BEHIND the deployed v6 — sync
from the archive snapshot before touching firmware.**

### Root cause of the connect freeze (ends every layout theory below)
Both modules carry a **Boya BY25Q64** flash (JEDEC 68 40 17). On marginal
dies its read-timing eye at the SDK's default SPIC clock is ~zero; the bus
load of a BLE connection pushes it over → garbage XIP fetch → unpreemptable
KM4 stall. "Layout sensitivity" was cache-miss patterns moving across the
marginal fetches; "unit sensitivity" was per-die flash grade. Proven by
elimination on board 2 (deterministic 100% freezer): three board-1-verified
layouts binary-patched with zero layout drift → froze; trace-UART stubbed →
froze; **Tuya-KM0 transplant booted and froze** (KM0 exonerated, IPC ABI
compatible); PSRAM empty, FTL writes already stubbed, wakelocks already held;
runtime phase probe showed the read window is exactly {calibrated}; halving
the SPIC clock → **first successful connects ever on that board**, then 6/6,
4/4, 3/3, and 2/2 first-ever connects on board 1.

**The fix: `flashClkSafe()`** (flash_diag.cpp) — FLASH_baud_rate/boot=2 +
FLASH_SetSpiMode, re-asserted every 100 ms in loop() AND immediately before
every connect kickoff (KM0-side clock management sporadically restores the
fast divider; set-once is not enough). Diags: flashid / flashbench /
flashphase / flashbaud over :6054.

### What the fleet build (v2→v6) adds on top of the 2026-08-24 resident
- v2: unified sketch, per-board identity via config.h `WBRG1_BOARD`
  (build with `compiler.c.extra_flags=-DWBRG1_BOARD=N` — NOT
  compiler.cpp.extra_flags, silently dropped; NOT build.extra_flags, which
  wipes the board defines and links a broken image), renames to
  wbrg1-jzzwg / wbrg1-gw018, flashClkSafe, trace_uart_* stubs (need
  trace_uart_{init,deinit,tx} WEAKENED in lib_arduino.a via objcopy), ctrl
  `log` command reading a 10-line in-RAM ring.
- v3: **DeviceInfo feature flags 99 → 103.** aioesphomeapi HARD-REFUSES to
  route ANY GATT connection through a proxy without REMOTE_CACHING(4)
  ("does not support REMOTE_CACHING feature"); with it set HA sends the
  CONNECT_V3 request types (handleDeviceRequest already accepted 0/4/5).
- v4: **GATT services response (71) now serializes descriptors** (field 4;
  short_uuid = field 3). Without the CCCD listed, HA's bleak client refuses
  start_notify ("does not have a characteristic client config descriptor").
- v5: **cancelPendingConnects()** — a timed-out le_connect stays PENDING in
  the controller forever and silently blocks ALL scanning (startScan cannot
  revive it). Both boards went advert-dark for Bermuda this way while
  api=1/sub=1 looked healthy. Fix: le_disconnect() every link in
  GAP_CONN_STATE_CONNECTING on any connect failure, then startScan. Deploy
  drill: `conn 112233445566 0` must leave the scanner streaming.
- v6: **scan-starvation watchdog** — a SECOND, deeper freeze: after many
  connect/pause/resume cycles the scan engine hard-wedges (startScan
  accepted, zero adverts; scan off/on and clean conn/disc do NOT revive it;
  only reboot does; connects keep working so it hides). Guard: no adverts for
  120 s with no active connection → "scanwd:" log + self-reboot (~30 s).
  Root-causing the wedge (scan re-init without reboot?) is the top open item.

**Health checks measure COUNTER DELTAS (stat `seen`), never connection
state** — api=1/sub=1 while deaf was the night's most deceptive state.

### HA side (done tonight)
- Devices/entities/config entries renamed (`sensor.wbrg1_jzzwg_*`,
  `sensor.wbrg1_gw018_*`; MAC-based unique_ids so history survived); ghost
  MQTT-era device removed + 20 retained broker topics cleared; both
  bluetooth-adapter entries retitled. HA shows both proxies Auto (active).
- HA caches GATT tables per MAC: changing a peer's attribute table under a
  known address breaks with-cache connects (stale handles → 2-4 s
  connect/drop loops). Give test peripherals a FRESH MAC instead.

### Endurance rig (running)
ESP32 (`wbrg1-ble/sketches/airthings_emulator/`, MAC 58:2A:BD:7D:31:99)
emulates an Airthings Wave Plus; HA's airthings_ble integration polls it via
GATT through the proxies every 5 min ("Airthings Wave Plus (2930123456)", 11
entities, battery 95%). This organic traffic found v3-v6's bugs in hours.
The ESP32 goes back to test-peripheral/proxy duty by reflashing.

### Open items
1. Root-cause the scan-engine hard wedge (v6 reboots around it; boot logs'
   `scanwd:` frequency = how often it bites).
2. OTA quirk now CONSISTENT: the FIRST `ota` trigger after boot is ignored;
   the second identical trigger applies. Verify via boot marker / uptime
   reset, never the HTTP log.
3. serveClient exit-path instrumentation (rare benign API session drops).
4. Sync this repo's sketch + shared tree from the v6 archive snapshot
   (includes the objcopy lib-weakening step for the trace stubs).
5. Board-1-style LED wiring survives in the build; re-verify LEDs after any
   sync (unchanged since c-leds).

---
# (Historical) Handoff — connectable BLE proxy (Phase 3), 2026-08-22
**Everything below predates the 2026-08-25 root-cause fix. Layout-lottery
theories, "GATT parked", and gw1/gw2 naming are SUPERSEDED by the section
above; kept for the investigation record.**

## RESIDENT STATE 2026-08-24 (evening) — MQTT retired; GATT-connect parked again
The resident firmware is now **MQTT-free** and is the daily-driver proxy:
- **No MQTT / PubSubClient.** The flapping (rc=-4 CONNACK-timeout retry storms on
  this core's PubSubClient) is gone. HA gets everything over the ESPHome API.
- **Telemetry = native ESPHome API sensors** (`sensor.office_wbrg1_gw1_*`:
  wifi_rssi, free_heap, uptime, adverts_seen, queue_drops). See
  esphome_api.cpp: setTelemetry / sendListEntities / sendSensorStates (msgs 16/25).
- **Control/OTA over a reliable socket on :6054** (`ard_socket.h`, same proven layer
  as the API — not PubSubClient). Tool: `tools/ctrl.py "<cmd>"` (e.g. `ctrl.py "ota
  <host> <port> <res>"`, `ctrl.py "reboot"`, `ctrl.py "conn <mac>"`). One command
  per connection. Replaces the old MQTT `ota_trigger.py` / command topic.
  Query `ctrl.py "stat"` -> `api_client=.. bt_sub=.. adv_sent=.. seen=.. wifi=..`
  to confirm HA is connected and advert streaming is live.
- **Advert path verified working:** after a reboot HA takes ~60 s to reconnect to
  :6053 and re-subscribe; during that window HA's adapter page shows the proxy as
  "No scanning / 0 connections" (transient, not a fault). Once settled, bt_sub=1,
  adverts stream (msg 93), and Bermuda counts it as an active proxy. The proxy does
  NOT implement scan-mode management, so HA never labels it "auto" like full
  ESPHome proxies — cosmetic only; raw advertisements (what presence needs) work.
- **Scanner state/mode implemented 2026-08-24:** DeviceInfo flags now 99 (adds
  STATE_AND_MODE=64); on advert-subscribe (msg 66) and on SetMode (msg 127) we send
  BluetoothScannerStateResponse(126) = RUNNING + ACTIVE, so HA's adapter page shows
  the proxy as actively scanning instead of "No scanning". We always active-scan,
  so mode is fixed. (Was flags 35 before; the "No scanning" label was this gap.)
  Confirmed on hardware: HA's adapter card now shows it actively scanning.
- **OTA gotcha (paid for):** a `ctrl.py "ota …"` can download the full image yet NOT
  reboot into it (silent verify/apply failure) — the device keeps running the old
  build. Always confirm the reboot took: `ctrl.py "stat"` after (or the HA uptime
  sensor) — if uptime did NOT reset to ~0, the OTA didn't apply; just re-trigger it.
  A build whose behavior "didn't change" after OTA is almost always this.
- Scanner + proxy + presence + LEDs (blue = 1/min heartbeat, red = warnings): solid.
  Verified stable (uptime climbs, adverts stream to HA, Bermuda proxy).
- Resident image archived: `experiments/resident-20260824-mqtt-free/`.

**GATT-connect is PARKED AGAIN (regressed by the MQTT removal).** The big code
change reshuffled the whole-image layout and re-triggered the XIP-layout stall.
Key finding this session: it is **NOT** just BT-library alignment — padding the app
so that ALL seven BT hot functions (hci_send_pkt, hci_adjust_link_quota,
l2c_send_pdu_msg, hci_if_task, gatt_handle_le_conn_cmpl_evt, hci_handle_le_evt,
bte_sched_handler) land at the EXACT known-good addresses did **not** fix it. So
the stall depends on the *whole* image (KM0 WiFi/PMC firmware, data/heap, other
libs, app), which a fundamentally different app (no PubSubClient, +ctrl/sensors)
cannot reproduce. A robust fix needs root-causing the silicon XIP stall — a real
research project. **Nothing in normal presence operation triggers a connect**
(Bermuda is passive), so the freeze never fires in daily use.

**To use GATT connect today:** flash the archived GATT-working build
`experiments/known-good-20260824c-leds/` (it has GATT + LEDs, but MQTT flapping).
The freeze-debug spies (girq/crt/flk/lck/rec, printed on the UART at each boot
after a freeze) and the weakened lib symbols remain in the resident build, dormant
— ready for a future GATT re-stabilization attempt.


## STATUS LEDs restored 2026-08-24 — pins identified, wired to link health
Both front LEDs are single active-high GPIOs on the WBRG1 (Tuya's old state/net
LEDs); our firmware never drove them, so they sat dark. Identified by an on-device
pad sweep: **red = PA25 (0x19), blue = PB22 (0x36)**. Now driven by ledInit()/
ledService() in the sketch: 
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
