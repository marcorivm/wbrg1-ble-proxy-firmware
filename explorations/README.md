# Tri-radio gateway explorations

The Tuya JZZWG-TY2.0 gateway is now, uniquely, a single always-on box the owner
controls across **three radios**: WiFi + BLE (WBRG1 / RTL8721CSM, custom firmware)
and 802.15.4 / Zigbee (ZS3L, stock Tuya router today). These documents explore how
to exploit that. Each is an independent idea; read `00-feasibility.md` first — it's
the shared ground-truth every other doc is checked against.

| # | Document | One-line |
|---|----------|----------|
| 00 | [Feasibility ground-truth](00-feasibility.md) | The hard constraints every idea lives under |
| 01 | [On-box local presence→action](01-local-presence-action.md) | Fuse BLE RSSI + act locally, WiFi/HA-independent |
| 02 | [Transport resilience & failover](02-transport-resilience.md) | Real failover when WiFi drops (hint: it isn't MQTT) |
| 03 | [BLE ↔ Zigbee bridging](03-ble-zigbee-bridge.md) | One box unifies two ecosystems |
| 04 | [Thread / Matter horizon](04-thread-matter.md) | The 802.15.4 radio's other life |

_Drafted 2026-08-24. These are exploration docs, not commitments._
