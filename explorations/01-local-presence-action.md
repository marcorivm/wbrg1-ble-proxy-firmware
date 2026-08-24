# 01 · On-box local presence→action

> One of the tri-radio explorations. Read [00-feasibility.md](00-feasibility.md) for the shared constraints.
> **In one line:** the gateway fuses the BLE RSSI it already sees and acts locally — sub-second, and alive even when WiFi/HA are down.

## 1. Concept

The gateway already sees the raw material for presence: it's a BLE scanner picking up advertisement RSSI from the owner's iBeacon phone (and any other tagged beacons), and it's physically central and always-on. The idea is to close the loop *inside the box* — fuse the RSSI it already collects, decide "someone is here / someone left," and fire an action, without a round-trip to Home Assistant.

**Data fused (all on the WBRG1):**
- Per-beacon RSSI, smoothed over a short window. A single packet is noise; an EWMA or median over ~2-5 s is a usable proximity signal.
- Multiple beacons tracked independently (phone, a keyfob tag, a car tag) so rules can distinguish *who*.
- Time-since-last-seen per beacon, the key signal for "left" / "empty."

**Decision logic** lives as a tiny rule engine in firmware: thresholds on smoothed RSSI (`> -65 dBm` ≈ this room), hysteresis (enter at -65, exit at -78) to stop flapping, and dwell/absence timers ("gone for 10 min"). Output is a boolean per rule with debounce.

**Action path** is the hard part (see §4). The clean version: rule flips → WBRG1 emits a Zigbee command to a bound device. Realistically the first working version drives a device over a path the WBRG1 actually controls, and only later reaches native Zigbee actuation.

## 2. Why this box, here

HA + Bermuda already does room-level presence *well* — better trilateration (multiple proxies), richer logic, history, a UI. This approach does **not** try to beat Bermuda at accuracy. Its entire value is what happens when the normal path is unavailable or too slow:

- **Resilience:** if WiFi, the AP, or the HA host is down (reboot, update, SD-card death), Bermuda is dead and every presence automation with it. The on-box loop keeps working because the sensor, the logic, and ideally the actuator are all on one always-on device.
- **Latency:** scan → local decision → local action is sub-second and deterministic. The HA path is scan → ESPHome/MQTT over WiFi → Bermuda → automation engine → command back out to the Zigbee coordinator → mesh. That's a lot of hops and a lot of "if the network is healthy."
- **No single point of failure for one critical behavior.** You're not replacing HA; you're giving *one or two* safety-critical behaviors a hardened local fallback.

Be honest: everything else — dashboards, multi-room fusion, notifications, anything needing the internet — stays in HA. This is a narrow, deliberately dumb, very reliable layer.

## 3. Scenarios

- **Hallway light on arrival:** phone RSSI crosses the "just inside the front door" threshold → hallway Zigbee bulb on, before you reach the switch — *even mid WiFi outage.* Off again after 90 s of absence.
- **Whole-home empty:** all known beacons unseen for 10 min → turn off a Zigbee plug (space heater, iron, the "everything" strip). This is the classic safety automation you least want to depend on the cloud/WiFi for.
- **Fridge/kitchen nudge:** owner's beacon strong in the kitchen after dark → under-cabinet light. A per-room rule keyed to one scanner's RSSI, no trilateration needed.
- **Night bathroom path:** between 01:00-05:00, beacon appears in hallway range → dim floor-level light to 10%, not the harsh overhead. Time gate lives on-box.
- **Left-behind tag:** keyfob tag present but phone gone for 20 min → chirp a Zigbee siren/plug (you left without your keys). Cross-beacon logic, fully local.

## 4. Feasibility & hard problems

**The big one — Zigbee actuation.** The ZS3L runs stock Tuya *router* firmware. A router relays frames; it **cannot originate application commands** to other devices — only a coordinator (or a device acting through binding) can. Routing ≠ commanding. So local Zigbee actuation needs one of:

1. **Reflash the ZS3L** to firmware that can send commands — e.g. a Zigbee stack where it acts as a secondary coordinator or issues bound commands. Highest payoff, highest cost: you need the toolchain for that 802.15.4 part, a flashing path (SWD, already exercised on these EFR32 boards), and it will re-join / possibly need re-pairing — reversible, not a blocker in a homelab. This is the only path that's truly WiFi- and HA-independent end to end.
2. **WBRG1 drives the device by another radio.** The RTL8721 has BLE — if the target load is a *BLE* device (a BLE bulb/plug), the WBRG1 already does connectable GATT and can command it directly, locally, today. No Zigbee at all. This sidesteps the whole router problem but only works for BLE actuators.
3. **Fallback through cached HA.** WBRG1 → local MQTT/HA API → ZHA/Z2M coordinator. Works with today's firmware, but it *contradicts the WiFi-independent goal* — it's just a faster-triggered normal automation. Worth having as the day-one version, not the end state.

**Honest read:** option 2 is the fastest real local win but constrains you to BLE loads; option 1 is the "proper" Zigbee answer but is the biggest project and the riskiest to the working mesh.

**Where state and rules live:** on the WBRG1, in firmware/config (a small table of beacon MACs, thresholds, timers, target device + action). Editable over the existing WiFi OTA/MQTT path when the network *is* up; executed locally when it isn't.

**Calibration:** RSSI thresholds are device- and placement-specific. Needs a one-time walk-around capture (log RSSI at door, in-room, adjacent room) to set enter/exit levels. iBeacon TX power and phone-orientation variance are real; keep thresholds loose and lean on hysteresis + dwell rather than tight cutoffs.

**False positives:** hysteresis, minimum-dwell before "enter," and generous absence timers before "leave." Never let a single missed scan window trigger "empty" on a safety load — require sustained absence.

**Airtime contention:** BLE scanning and Zigbee both live in 2.4 GHz, and here they're *different chips* in one box — so the concern isn't one radio time-sharing, it's local RF coupling and channel overlap (BLE adv channels 37/39 sit near Zigbee channels 11/25-26). Pick a Zigbee channel away from the BLE adv channels, and if the WBRG1 ever does connectable GATT while a rule fires, expect brief scan gaps — size timers to tolerate them.

## 5. Build path

1. **Instrument:** log smoothed per-beacon RSSI + last-seen from the WBRG1; do the calibration walk. No actions yet.
2. **Rule engine (dry-run):** implement thresholds/hysteresis/timers on-box; publish "would-fire" events over MQTT and compare against Bermuda for a week.
3. **First real action, safest path:** drive one *BLE* load via WBRG1 GATT (option 2) — proves the local loop end to end with zero Zigbee risk.
4. **HA-fallback action (option 3):** wire the same rules to trigger a Zigbee device via HA for the cases where the load is Zigbee — accept the WiFi dependence for now.
5. **Zigbee-native spike (option 1):** research/prototype ZS3L reflash on a *spare* module, validate it can command + still route, before touching the in-home one.
6. **Harden:** watchdog, safe defaults on boot, and an explicit "HA is up → defer to HA; HA is down → act locally" arbitration so the two layers never fight.

---
_See also: [00-feasibility.md](00-feasibility.md) · [02 transport resilience](02-transport-resilience.md) · [03 BLE↔Zigbee bridge](03-ble-zigbee-bridge.md)_
