# 02 · Transport resilience & failover

> One of the tri-radio explorations. Read [00-feasibility.md](00-feasibility.md) for the shared constraints.
> **In one line:** MQTT is *not* a fallback for a WiFi outage — the only genuinely separate pipe this box has is the 802.15.4 radio.

## 1. Three transports, one shared dependency

Your gateway speaks over three logical transports, but they do **not** fail independently:

| Transport | Carrier | Dies when… |
|---|---|---|
| ESPHome native API (:6053) | TCP over **WiFi** | WiFi/AP down, HA down |
| MQTT (Mosquitto) | TCP over **WiFi** | WiFi/AP down, broker down |
| Zigbee (ZS3L, 802.15.4) | **RF mesh**, no WiFi | mesh/coordinator down |

The gentle correction to the "fall back to MQTT" instinct: **MQTT is not a fallback for a WiFi outage.** Both the ESPHome API and MQTT are TCP sessions riding the same WBRG1 WiFi association. When the AP reboots or the radio drops, *both* sockets go down at the same instant. MQTT is a fallback for an *API-layer* problem (HA's API component wedged, native API disabled), not for a *link-layer* problem. The only genuinely separate pipe in this box is the ZS3L's 802.15.4 radio — it reaches HA through the Zigbee coordinator, over RF that never touches your AP.

The real failover ladder:

```
1. ESPHome native API over WiFi     (primary, rich, low-latency)
2. MQTT over WiFi                    (app-layer redundancy, SAME link)
   —— WiFi link boundary ——
3. Zigbee 802.15.4 via ZS3L → coordinator → HA   (survives WiFi loss)
```

Steps 1–2 are one failure domain. Step 3 is the one that changes the game.

## 2. What the parallel API+MQTT setup already buys you

Real value, at one layer only:

- **App-layer redundancy.** If HA's native-API integration is unavailable but MQTT is up (or vice-versa), presence data still lands. Config reloads, integration reloads, a wedged API component — MQTT covers these.
- **Decoupling / retained state.** MQTT retained topics + a subscriber other than HA (Node-RED, another logger) means data isn't lost if HA restarts mid-stream.

What it does **not** buy: survival of a WiFi/AP outage, a DHCP failure, or the WBRG1 losing association. In every one of those, API and MQTT go dark **together**. No amount of "also publish to MQTT" changes that, because the bytes still leave over the same radio and the same AP.

## 3. The Zigbee-transport fallback, in depth

The idea: on detected WiFi loss, the WBRG1 hands a **tiny** message to the ZS3L side, which relays it over the mesh to something HA's Zigbee coordinator already reads.

**What is worth sending — and what is not.** Zigbee is a low-rate, small-frame medium: usable application payloads are a few tens of bytes per frame, and you want well under one message every few seconds to be a good mesh citizen. That budget is fine for **state**, hostile to **streams**:

- **Send:** presence boolean(s) (`someone_home`, per-zone occupancy), a door/open event, an alarm/tamper flag, a heartbeat. A single byte of bitflags covers most of it.
- **Do NOT send:** raw BLE RSSI streams, per-advert dumps, anything Bermuda-shaped. Trilateration needs high-rate multi-scanner RSSI; that's a WiFi/API job that simply pauses during an outage.

**Latency reality:** seconds, not milliseconds — route discovery, retries, and rate-limiting add up. Acceptable for "someone is home" or "door opened during the outage," which is the whole point.

## 4. The hard problem, stated honestly

**The WBRG1 and ZS3L are separate chips.** A UART link between them *probably exists* (see [00 §1](00-feasibility.md) — ZS3L's `RX1/TX1` on header P2), but it's **unconfirmed at the trace level and unused by current firmware**. And a stock ZS3L in ROUTER firmware forwards *mesh* frames but exposes **no interface** for the WBRG1 to inject an arbitrary application payload — routing frames ≠ originating your frames.

Making this real would need, all still to be proven:
1. A **confirmed physical UART** between the modules (trace-out + logic analyzer).
2. **Custom ZS3L firmware** (leaving stock Tuya — an EmberZNet/Z-Stack build) that reads that UART and emits a Zigbee frame on a cluster/attribute your coordinator can read.
3. A **cluster/attribute contract** plus a matching HA-side quirk/converter (ZHA quirk or Z2M external converter) so the byte becomes an entity.

Also unresolved: whether re-flashing the ZS3L drops it from your existing mesh (it will — you'd re-pair), and the fact that it's an **in-use** router slated for the Kitchen. Treat this whole path as a *research project*, prototyped on a spare EFR32 — not a config change.

## 5. Simpler wins that ARE achievable now (do these first)

Pragmatic near-term layer, all in your existing Arduino firmware, no second chip involved:

- **WiFi-reconnect robustness.** Non-blocking reconnect with backoff; never block the BLE loop waiting on WiFi (respects the must-run-in-`loop()` rule). Re-establish both API and MQTT on link-up.
- **MQTT LWT + birth message.** Broker publishes `offline` on your Last-Will topic the moment the socket dies; firmware publishes `online` (retained) on connect. HA *knows within seconds* the gateway dropped, instead of trusting stale data.
- **Local buffering + flush.** Ring-buffer presence *transitions* (not raw adverts) during brief drops; flush on reconnect with timestamps. A 30–90 s AP reboot then costs zero events.
- **Watchdog-backed auto-recovery.** Hardware watchdog (already present) + a soft supervisor: N failed reconnects → controlled reboot. Mains-powered + always-on makes a reboot cheap insurance.

These give you *detection* and *graceful ride-through* of the common short outages the Zigbee path would cover — without touching the ZS3L.

## 6. Scenarios: with vs without

- **Router/AP reboot (60–120 s).** *Without:* API + MQTT both dark; HA marks entities unavailable or holds stale "home." *With near-term layer:* LWT fires `offline` instantly, events buffer, flush on reconnect — clean gap, no false presence. *With Zigbee path:* an occupancy change during the gap still reaches HA live.
- **LAN outage, ISP up (switch/DHCP dies).** *Without:* total blackout — HA is LAN-only, the internet being fine is irrelevant. *With near-term:* offline is known and bounded. *With Zigbee:* presence/alarm still flows — it never used the LAN.
- **AP firmware update (minutes down).** *Without:* long unavailable window, likely stale state. *With near-term:* explicit offline + buffered flush, but blind during the window. *With Zigbee:* the one scenario where the second transport clearly earns its keep.
- **HA native-API component wedged, WiFi fine.** *With existing API+MQTT:* MQTT carries it — the case your current parallel setup already solves; Zigbee not needed.

**Bottom line:** ship section 5 now — it's most of the real-world benefit for a weekend of firmware work. Treat 3–4 (Zigbee as a true out-of-band transport) as a deliberate reverse-engineering project, worth it only for the AP-update / LAN-dead cases where nothing over WiFi can help.

---
_See also: [00-feasibility.md](00-feasibility.md) · [01 local presence→action](01-local-presence-action.md) · [03 BLE↔Zigbee bridge](03-ble-zigbee-bridge.md)_
