# 00 · Feasibility ground-truth & shared constraints

> **Read this first.** It's the reality check every other exploration is measured against.
> If an idea contradicts something marked **confirmed** here, the idea is wrong; if it depends
> on something marked **unknown**, it is a research task, not a feature.

## 1. The two-chips link (the load-bearing question)

The WBRG1 (RTL8721CSM, WiFi+BLE) and the ZS3L (**EFR32MG21**, 802.15.4) are **two independent SoCs** sharing one PCB and one power rail. In Tuya's stock design the WBRG1 is the host MCU and the ZS3L is a Zigbee NCP driven over a **UART** — the efr32 handoff references "the ZS3L/WBRG1 wiring" and the ZS3L exposes `RX1/TX1` pads (header P2). So a physical UART link between them **probably exists at the trace level** — a much better starting point than "two chips that only share power."

**Confirmed:** both chips now run *owner* firmware that does **not** use any inter-chip link. The WBRG1 firmware is a standalone BLE proxy/MQTT client; the ZS3L runs stock EmberZNet **8.2.2.0 router** firmware, which needs no host interface. They cooperate only by both being powered.

**Unknown (the one gate for every cross-radio idea):** whether the WBRG1's UART pins are actually trace-routed to the ZS3L's `RX1/TX1`, at which GPIOs, and with what level/pull conditions. To resolve: (a) buzz out the PCB traces between the WBRG1 UART pads and ZS3L P2, (b) cross-check the RTL8721CSM and EFR32MG21 pinouts, (c) confirm with a logic analyzer once firmware toggles a candidate pin. Until then, **assume no usable bridge** but rate it "likely wired, needs an afternoon to confirm" — not "impossible."

## 2. Zigbee router ≠ coordinator

The ZS3L runs **router** firmware. A router **relays/routes** frames and hosts its own endpoints — nothing more. It **cannot** originate arbitrary application commands, bind as a controller, or manage the network. The coordinator (on HA / Zigbee2MQTT) owns all of that.

So any "**the box commands Zigbee devices locally**" idea is **not possible with current firmware.** It needs either (a) reflashing the ZS3L to a **secondary-coordinator / custom-endpoint / NCP** image, or (b) merely *routing* a command the real coordinator issued (no local intelligence).

**Reflashing risk is real.** The ZS3L is a **working router in the live mesh**, and it's slated to **move to the Kitchen for fridge temp sensors** — it is *in use*, not spare. Reflashing means losing router service during the flash, NVM3 mesh-state churn, SWD access (Pico/CMSIS-DAP, per the efr32 handoff), and a restore path that leans on the out-of-repo Tuya backup. Treat the ZS3L as **frozen** unless a feature is worth taking it off the mesh — and prototype on a *spare* EFR32 first (you already converted a second board).

## 3. 2.4 GHz coexistence

Three radios, one band, two of them in one enclosure: RTL8721CSM (WiFi **+** BLE, one RF front-end, time-sliced internally) and ZS3L (Zigbee).

- **BLE + WiFi on the WBRG1 already time-share one radio.** Active BLE scanning steals airtime from WiFi/MQTT; the firmware deliberately runs a coalescing, passive-leaning scan for this reason. A Zigbee-transport or heavy connect workload competes for the *same* radio.
- **Zigbee (ZS3L) and WiFi (WBRG1) overlap in-band.** Pick a Zigbee channel (15/20/25) that dodges the gateway's WiFi channel, or throughput/range on both drop.
- **Net cap:** this box is a good *sensor/proxy* on all three radios; it is **not** headroom for high-duty-cycle simultaneous BLE-connect + WiFi-OTA + Zigbee-relay bursts.

## 4. RTL8721CSM firmware is delicate

Per HANDOFF.md, the WBRG1 firmware is **verified-fragile**:

- A **XIP-flash-layout-sensitive KM4 stall** froze BLE connections below the HCI layer; ±100 B of code flips freeze/work. Beaten with retained-SRAM spies, **not** fully root-caused — the ship build keeps all spies armed.
- Recovery machinery exists because it's needed: 8 s hardware watchdog, safe-mode boot counter (2 failed BT inits → WiFi-only), OTA-over-WiFi, UART flashing as last resort.
- **Non-negotiable ritual:** any rebuild = OTA, then 3× `conn`/`disc` cycles + `esphome_gatt_test.py` before trusting it. Every new WBRG1 feature inherits this cost.

## 5. Effort / risk table

| Ambition | Blocked on | Effort | Risk to working setup |
|---|---|---|---|
| **Transport resilience — near-term layer** (reconnect, LWT, buffering, watchdog) | Nothing — pure WBRG1 firmware | Low | Low |
| **Zigbee-transport fallback** (route/relay a coordinator-issued command) | Limited value; still needs the HA coordinator | Low–Med | Low |
| **Local presence→action via BLE actuators** | Nothing — WBRG1 GATT client works today | Low–Med | Low |
| **Local presence→action via Zigbee actuators** | ZS3L router→coordinator reflash **+** confirmed UART (§1) | High | **High** — in-use mesh router |
| **BLE ↔ Zigbee bridge** | Confirm §1 UART + custom firmware on both + coexistence (§3) | Very High | **High** — touches both chips |
| **Thread / Matter** on the ZS3L | New 802.15.4 stack, full reflash, no border-router story here | Very High (research) | **High** — loses Zigbee router |

**Bottom line:** the near-term resilience layer and BLE-actuator local control are a weekend away. Everything genuinely cross-radio hinges first on **confirming the §1 UART** — which is likely wired and cheap to check — and every ZS3L reflash trades away a working, soon-to-be-relocated mesh router, so prototype on a spare EFR32.
