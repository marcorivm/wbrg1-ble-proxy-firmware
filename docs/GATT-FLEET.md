# GATT-capable fleet — state as of 2026-08-25 (v6)

Both gateways run a converged firmware: BLE scanner + native ESPHome API
(sensors, raw advertisements) + **working GATT connect/read/write/notify** +
control/OTA socket on :6054. No MQTT.

| board | name | IP |
|---|---|---|
| JZZWG-TY2.0 | `wbrg1-jzzwg` | 192.168.0.175 |
| RSH-GW018-DM | `wbrg1-gw018` | 192.168.0.130 |

Build source of truth: `wbrg1-ble/experiments/gw2-gatt-SOLVED-20260824/`
(local, non-git: sketch snapshots per version, OTA images, full evidence
chain). This doc is the condensed engineering record.

## The connect-freeze root cause (ends the "XIP layout lottery")
Both boards use a Boya BY25Q64 flash (JEDEC 68 40 17) whose read margin at
the SDK's default SPIC clock is near zero on some dies; BLE-connect bus load
pushes it over → unpreemptable KM4 stall. Layout- and unit-sensitivity were
proxies for cache-miss patterns and per-die flash grade.
**Fix: `flashClkSafe()`** — FLASH_baud_rate=2 + FLASH_SetSpiMode, re-asserted
on a 100 ms timer and immediately before every connect kickoff (KM0-side
clock management sporadically restores the fast divider). Verified: 0/7+
connects at full clock froze; 15+/15+ connects at safe clock succeed, on both
boards, plus full GATT r/w/notify through HA.

## ESPHome-proxy contract lessons (cost a night to learn)
- Feature flags must be **103**: PASSIVE_SCAN|ACTIVE_CONNECTIONS|
  RAW_ADVERTISEMENTS|STATE_AND_MODE **|REMOTE_CACHING** — aioesphomeapi
  hard-refuses to route GATT connections without REMOTE_CACHING, and sends
  CONNECT_V3 request types once it is set.
- BluetoothScannerStateResponse(126) (RUNNING/ACTIVE) is what stops HA's
  panel showing "No scanning".
- The GATT services response (msg 71) **must include descriptors** (field 4;
  short_uuid field 3) — without the CCCD listed, HA refuses start_notify.
- HA caches GATT tables per MAC: changing a device's attribute table under a
  known address breaks with-cache connects (stale handles, 2-4 s drop loops).

## Scanner-freeze failure modes + guards (both in firmware since v5/v6)
1. **Pending-connect wedge**: a timed-out le_connect stays PENDING in the
   controller forever and blocks all scanning; startScan cannot revive it.
   Guard: `cancelPendingConnects()` (le_disconnect on CONNECTING links) +
   startScan in every connect-failure path.
2. **Scan-engine hard wedge**: after many connect/pause/resume cycles the
   scan engine dies (startScan accepted, zero adverts; only reboot revives;
   connects still work, so it hides). Guard: scan-starvation watchdog —
   no adverts for 120 s with no active connection → logged self-reboot.
   Root cause open.

**Health checks measure counter DELTAS** (adverts seen), never connection
state: api=1/sub=1 while deaf was tonight's most deceptive state.

## Operational notes
- OTA quirk: the first `ota` trigger after boot is ignored; send it twice and
  verify via the boot marker / counter reset, not the HTTP log.
- Diag over :6054: stat, log (ring), conn/disc, flashid, flashbench,
  flashphase/flashbaud, scan on/off, reboot, uartburn.
- Deploy ritual: OTA → 3x conn/disc drill → `conn 112233445566 0` (failed-
  connect drill; scanner must keep streaming) → GATT rw test if the API slot
  is free.
- Test rig: ESP32 Airthings Wave Plus emulator
  (`wbrg1-ble/sketches/airthings_emulator/`, MAC 58:2A:BD:7D:31:99) — makes
  HA's airthings_ble poll via the proxies every 5 min; the organic endurance
  test that found most of the above.
