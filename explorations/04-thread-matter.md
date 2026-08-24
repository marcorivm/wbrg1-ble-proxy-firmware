# 04 · Thread / Matter horizon

> One of the tri-radio explorations. Read [00-feasibility.md](00-feasibility.md) for the shared constraints.
> **In one line:** the ZS3L's 802.15.4 radio can do Thread instead of Zigbee — a fascinating, long-horizon experiment, not a weekend project.

## Concept

The ZS3L's (EFR32MG21) 802.15.4 PHY is the same radio layer Thread uses. Reflash it with an OpenThread-based build and the box could act as a Thread router node, or — with much more work — a Thread Border Router (OTBR), bridging the Thread mesh to your WiFi LAN via the WBRG1.

## What it unlocks

Native support for **Matter-over-Thread** devices (the fast-growing low-power category), future-proofing as Thread becomes the low-power standard, and interop with Apple/Google/Amazon ecosystems that all speak Matter. A local OTBR here would extend Thread coverage into a central room and let HA's Matter integration reach Thread devices without buying a dedicated border router.

## Realistic scenarios

Start as a plain Thread *router* to densify a mesh seeded by an existing Apple/Nest/HA border router — low commitment, immediately useful. Longer term, a home-grown OTBR feeding HA's Matter server; or exposing one of your own devices as a Matter endpoint for cross-platform control.

## The honest caveats

- **You must choose Zigbee or Thread.** One 802.15.4 radio can't run both stacks simultaneously; flashing Thread ends this chip's Zigbee-router duty. Your mesh loses a router — and this is the router you're about to move to the Kitchen, so it's not free.
- **A real border router is a big build.** OTBR isn't just Thread firmware — it needs an IP interface bridging Thread↔WiFi, with the RCP (radio) on the ZS3L and the host stack somewhere with real networking. The RTL8721CSM has WiFi but isn't a Linux host running `otbr-agent`; standing up a genuine OTBR across these two constrained chips (RCP over the same unproven inter-chip link, host duties on a microcontroller) is a major, possibly impractical, project. A plain router node is far more attainable than a border router.
- **Matter adds certification and complexity.** Commissioning, the Matter data model, and interop testing are heavy; certification only matters if you distribute, but the protocol surface is large either way.

## Verdict

Fascinating, long-horizon, not a weekend project. A Thread *router* experiment is plausible-to-moderate effort (and something you're well-placed for, having already flashed EFR32 boards over SWD); a real Border Router is **experimental / very hard**, blocked on the same inter-chip link plus a host capable of IP bridging. Best treated as "someday, on a spare EFR32," well after the Zigbee-side work settles.

---
_See also: [00-feasibility.md](00-feasibility.md) · [03 BLE↔Zigbee bridge](03-ble-zigbee-bridge.md)_
