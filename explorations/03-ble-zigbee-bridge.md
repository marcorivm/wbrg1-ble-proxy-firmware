# 03 · BLE ↔ Zigbee bridging

> One of the tri-radio explorations. Read [00-feasibility.md](00-feasibility.md) for the shared constraints.
> **In one line:** one box translates between two ecosystems at the radio level — genuinely useful, gated on confirming the inter-chip UART.

This box is unusual: two independently-flashable radios in one always-on, mains-powered enclosure in the middle of the house. The WBRG1 (RTL8721CSM) runs your firmware as a WiFi+BLE Swiss-army node. The ZS3L (EFR32MG21) is an 802.15.4 SoC currently doing stock Tuya Zigbee routing. This doc is about what happens if the ZS3L also comes under your control — and how far the two chips can actually talk.

## Concept

Use the WBRG1's connectable GATT client to poll a cheap BLE sensor, then have the ZS3L *originate* Zigbee frames that present that reading as a native Zigbee endpoint (e.g. a Temperature Measurement cluster) into your mesh. The box becomes a two-way translator: BLE-world data appears as Zigbee entities, and Zigbee events can drive BLE actions. Because both radios are local, none of this touches a cloud — the translation happens in the room, not in Tuya's or Xiaomi's datacenter.

## Why it's useful

Today your ecosystems only meet inside Home Assistant. That's fine when HA is the brain, but it means a Zigbee-only controller (a Tuya scene panel, a Zigbee remote, an offline ZHA group binding) can never *see* a BLE sensor, and vice-versa. One box unifying both at the radio level makes cross-ecosystem automations survive an HA reboot and lowers latency for the simple stuff.

## Scenarios

1. **Xiaomi LYWSD03MMC as a Zigbee sensor.** WBRG1 reads temp/humidity over GATT; ZS3L re-exposes it as a Zigbee Temperature/Humidity endpoint so a Zigbee thermostat or ZHA binding reacts directly.
2. **SwitchBot/beacon button → Zigbee scene.** A BLE button press seen by the WBRG1 fires a Zigbee scene-recall frame from the ZS3L, triggering Zigbee bulbs with no HA in the path.
3. **Zigbee occupancy → BLE write.** A Zigbee PIR event tells the WBRG1 to write a GATT characteristic on a BLE lock or light.

## The hard feasibility gap

Two blockers, both real:

- **Stock Tuya router firmware cannot do this.** A Zigbee *router* forwards and joins; it does not present arbitrary application endpoints or originate sensor reports on demand. You'd need custom 802.15.4 firmware (a Zigbee stack you control — Z-Stack/EmberZNet-class, or a bare 802.15.4 build) that can host endpoints and craft frames. That's a full reflash of the ZS3L, and joining your existing mesh as a custom device means matching its network key and coordinator policy. Prototype on a *spare* EFR32, then flash the in-box ZS3L once proven — reflashing it is reversible (re-pair + restore), not off-limits ([00 §2](00-feasibility.md)).
- **The inter-chip link must be confirmed.** WBRG1 and ZS3L are *separate chips*. Bridging requires a data channel between them — the UART that [00 §1](00-feasibility.md) says *probably* exists (ZS3L `RX1/TX1` on header P2, referenced in the efr32 handoff) but is **unverified at the trace level and unused today.** If Tuya wired the host↔NCP UART (combo gateways usually do), you're in business; if not, bridging needs a bodge wire. **This is the gate: confirm the trace before anything else.**

## Verdict

Genuinely useful, largely gated. Blocked on: (1) confirming the WBRG1↔ZS3L UART trace (cheap — an afternoon with a multimeter and logic analyzer), and (2) custom ZS3L firmware that can originate frames. **Effort tier: hard** — and step zero is a hardware investigation, not code. But the "probably already wired" UART makes this less far-fetched than it first looks; the trace-out is the make-or-break, do it first.

---
_See also: [00-feasibility.md](00-feasibility.md) · [01 local presence→action](01-local-presence-action.md) · [04 Thread / Matter horizon](04-thread-matter.md)_
