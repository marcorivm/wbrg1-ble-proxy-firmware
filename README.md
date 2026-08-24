# WBRG1 BLE Proxy Firmware

Turn the **Tuya WBRG1** Wi-Fi/Bluetooth module (Realtek **RTL8721CSM**, AmebaD) —
the kind found inside Tuya multi-mode Zigbee gateways — into a **passive BLE
advertisement scanner** that reports to Home Assistant over MQTT, so
[Bermuda](https://github.com/agittins/bermuda) can use it for room-level presence.

It runs **alongside** the gateway's Zigbee radio: on these gateways the Zigbee
side is a *separate* chip (a Silicon Labs EFR32 "ZS3L" module), untouched by any of
this. You end up with one box giving you both a Zigbee router **and** a BLE
presence scanner.

The Home Assistant side (the integration that registers this as a Bluetooth
scanner) lives in a companion repo:
**[hass-wbrg1-ble-proxy](https://github.com/marcorivm/hass-wbrg1-ble-proxy)**.

> Firmware updates are **over-the-air** once bootstrapped — see [OTA](#5-updating-over-the-air-ota).
> UART is only needed for the first flash and for recovery.

---

## Contents
- [What you get](#what-you-get)
- [Hardware](#hardware)
- [How it works](#how-it-works)
- [0. Prerequisites](#0-prerequisites)
- [1. Configure & build](#1-configure--build)
- [2. Wiring](#2-wiring)
- [3. Enter ROM download mode](#3-enter-rom-download-mode)
- [4. First flash (bootstrap) — back up first!](#4-first-flash-bootstrap--back-up-first)
- [5. Updating over the air (OTA)](#5-updating-over-the-air-ota)
- [6. Home Assistant / Bermuda](#6-home-assistant--bermuda)
- [Hard-won lessons](#hard-won-lessons)
- [Troubleshooting](#troubleshooting)
- [Repo layout](#repo-layout)
- [Credits](#credits)

---

## What you get

- Passive BLE scan (never transmits), advertisements coalesced per device and
  published once per second as JSON on `wbrg1/<id>/adv`.
- A retained `online`/`offline` LWT on `wbrg1/<id>/status`.
- A stable MQTT link (no keepalive flapping — see lessons) and **zero** advert
  drops under load.
- **OTA updates over Wi-Fi** via an MQTT command — no opening the case after the
  first flash.

## Hardware

| | |
|---|---|
| Module | Tuya **WBRG1** = Realtek **RTL8721CSM** (AmebaD: KM4 + KM0 Cortex-M33, 8 MB flash, 4 MB PSRAM, Wi-Fi + BLE 5.0) |
| Seen in | Tuya multi-mode gateways (e.g. JZZWG-TY2.0, RSH-GW018/GW006, "ZS3L + WBRG1" boards) |
| Build target | Arduino **AMB21/AMB22** (`realtek:AmebaD:Ameba_AMB21_AMB22`) — the RTL8722CSM sibling; binary-compatible for this purpose |
| Programmer | Any 3.3 V USB-UART. A Raspberry Pi **Pico running `debugprobe`** works great (it's a CMSIS-DAP + UART bridge) |

**WBRG1 pins used** (Tuya datasheet):

| Pin | Name | Use |
|---|---|---|
| 7 | `LOG_TX` (PA7) | module TX / **also the ROM download strap** |
| 8 | `LOG_RX` (PA8) | module RX |
| — | `CHIP_EN` | active-low-ish enable/reset (pulled up, 3.3 V) |
| — | `GND` / `3V3` | power (the gateway powers itself; you share GND only) |

## How it works

```
 BLE adverts ──▶ RTL8721CSM (this firmware, passive scan)
                    │  coalesce per (mac, adv-type), 1 Hz
                    ▼
              MQTT  wbrg1/<id>/adv  (JSON batches)
                    │
                    ▼
        Home Assistant + hass-wbrg1-ble-proxy
        (parses raw AD → BaseHaRemoteScanner)
                    │
                    ▼
                 Bermuda
```

The MQTT wire format (raw advertising payload as hex, parsed on the HA side):

```json
{ "scanner": "wbrg1-gw1",
  "adv": [ {"mac":"AABBCCDDEEFF","at":0,"et":0,"rssi":-73,"data":"0201060909..."} ] }
```

---

## 0. Prerequisites

```sh
# arduino-cli + the Realtek AmebaD core
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://github.com/Ameba-AIoT/ameba-arduino-d/raw/master/Arduino_package/package_realtek_amebad_index.json
arduino-cli core update-index
arduino-cli core install realtek:AmebaD          # ~large; includes the macOS/Linux/Win toolchain
```

Python 3 with `pyserial` (`pip install pyserial`) for the flashing/OTA tools.

## 1. Configure & build

```sh
cp wbrg1_ble_proxy/config.h.example wbrg1_ble_proxy/config.h
# edit config.h: WiFi SSID/pass, MQTT host/user/pass, and a unique SCANNER_ID
arduino-cli compile --fqbn realtek:AmebaD:Ameba_AMB21_AMB22 wbrg1_ble_proxy
```

The build drops `km0_boot_all.bin`, `km4_boot_all.bin`, `km0_km4_image2.bin` into
the core's `.../packages/realtek/tools/ameba_d_tools/<ver>/` directory — that path
is the `<arduino-ameba_d_tools-dir>` the flash/OTA tools want.

`config.h` knobs worth knowing: `SCANNER_ID` (must match the HA integration entry),
`SCAN_ACTIVE` (0 = passive, never transmits — recommended next to the Zigbee
radio), `FLUSH_INTERVAL_MS` (MQTT batch cadence).

## 2. Wiring

Power the gateway from **its own USB-C**. Share **GND only** with your
programmer — never feed it 3V3 from the programmer.

| Programmer (Pico example) | → | WBRG1 |
|---|---|---|
| GP5 (UART **RX**) | ← | `LOG_TX` (pin 7) |
| GP4 (UART **TX**) | → | `LOG_RX` (pin 8) |
| GND | ↔ | GND |

Note the crossover (programmer RX ↔ module TX). For the download dance you also
need a **jumper** (`CHIP_EN`→GND) and a **~1 kΩ resistor** (`LOG_TX`→GND).

> ⚠️ **The reset trap.** `LOG_TX` (PA7) is *also the ROM download strap*. If your
> programmer's RX line holds it low at reset, the chip boots into the ROM
> downloader instead of your firmware — and looks dead. **Keep RX off `LOG_TX`
> while the module comes out of reset**, then reconnect it to read logs.

## 3. Enter ROM download mode

The AmebaD ROM enters UART download mode when PA7 is low as it leaves reset. Order
matters:

1. `CHIP_EN` → GND  (hold)
2. `LOG_TX` → GND through the **1 kΩ**  (hold)
3. release `CHIP_EN`   ← ROM samples PA7 low here
4. remove the resistor  ← **mandatory**; the module can't reply while it's on

Success looks like the ROM printing `#Flash Download Start` and then NAK-polling
(`0x15`) on `LOG_TX`. The `--wait` flag on `uart_flash.py`/the OTA tools watches
for that banner so timing is forgiving.

## 4. First flash (bootstrap) — back up first!

Get the vendor UART tools (not redistributed here), then **back up the stock Tuya
firmware** — it holds the module's Wi-Fi MAC and RF calibration and is the only
copy:

```sh
cd tools && sh fetch_vendor_tools.sh && cd ..
# module in download mode, then:
python3 tools/uart_dump.py <serial-port> wbrg1-tuya-full-8mb.bin   # ~13 min @115200
```

Verify it's 8 MB and real (starts with `81958711`/`99999696`, not all-0xFF) and
keep it safe. To go back to Tuya later, write it back with a full-flash tool.

Then flash this firmware:

```sh
TD=~/.../packages/realtek/tools/ameba_d_tools/<ver>   # where the build put the images
python3 tools/uart_flash.py <serial-port> "$TD" --wait
# do the download dance during the 120 s window
```

It writes the three images to their slots and verifies each by read-back. When it
says `all images verified`, reset the module (`CHIP_EN` tap, **RX off `LOG_TX`**)
to boot the firmware. It should join Wi-Fi, connect to MQTT, and publish adverts.

## 5. Updating over the air (OTA)

After the bootstrap, you never need UART again. The module runs an OTA client
triggered by an MQTT command; it pulls the new image over plain HTTP, verifies it,
and reboots into the inactive slot.

**The one non-obvious bit:** `http_update_ota()` needs the image wrapped in the SDK
**`OTA_All`** container (a 32-byte header with an `OTA` magic + checksum), **not**
the raw `km0_km4_image2.bin`. `make_ota.py` builds it.

```sh
# 1. build (as in step 1), then wrap:
python3 tools/make_ota.py "$TD/km0_km4_image2.bin" ota_all.bin

# 2. serve it from this host (ota_httpd.py logs how many bytes the device pulls):
python3 tools/ota_httpd.py ota_all.bin 8000 &

# 3. tell the module to pull it — <this-host-ip> on the module's subnet:
mosquitto_pub -h <broker> -u <user> -P <pass> \
  -t wbrg1/<id>/cmd -m "ota <this-host-ip> 8000 /ota_all.bin"
```

The module downloads (~7 s), verifies the checksum, flips the boot slot, and
reboots (~20–30 s total). Confirm from the retained `wbrg1/<id>/status`.

Because OTA writes only the **inactive** slot and verifies before switching, a bad
push is safe — the module keeps running the current image; worst case, re-bootstrap
over UART.

## 6. Home Assistant / Bermuda

Install **[hass-wbrg1-ble-proxy](https://github.com/marcorivm/hass-wbrg1-ble-proxy)**
(HACS custom repo), add the integration with your `SCANNER_ID` and topic prefix,
and the module appears under *Settings → Devices & services → Bluetooth* and in
Bermuda's scanner list. It's advertisement-only (`connectable=False`), which is all
Bermuda needs.

## ESPHome native API (Bluetooth proxy)

Besides the MQTT path, the firmware runs a minimal **ESPHome native-API** server
(plaintext, port 6053) in its own task, so Home Assistant's **ESPHome integration**
adopts the module directly and uses it as a native **Bluetooth proxy** — no custom
integration needed. Add it in HA via *Settings → Devices → Add integration →
ESPHome*, host = the module IP, port 6053, no encryption key.

It advertises `bluetooth_proxy_feature_flags = PASSIVE_SCAN | RAW_ADVERTISEMENTS`
and streams `BluetoothLERawAdvertisementsResponse` frames, which Bermuda consumes
like any ESP32 proxy. Implemented from ESPHome's `api.proto` by hand-rolling just
the protobuf framing and the handful of messages needed (Hello/Connect/DeviceInfo/
Ping/ListEntities + the BLE advert messages) — see `wbrg1_ble_proxy/esphome_api.*`.

Notes from building it:
- Plaintext framing is `0x00, varint(len), varint(type), payload`. HA still works
  without encryption when the device offers plaintext.
- Realtek's `bd_addr` is **LSB-first**; ESPHome's advert `address` is a big-endian
  uint64, so reconstruct it from `bd_addr[5..0]` or every device shows up
  byte-reversed and won't correlate with your other scanners.
- Don't hold the core's `WiFiClient` across loop iterations — its destructor
  closes the socket and it has no refcounting. Use raw sockets (`ard_socket.h`)
  in a dedicated task instead.
- The server is single-client (HA). Add an idle timeout so a dead client is
  dropped and HA can reconnect.

Both transports (MQTT scanner + ESPHome proxy) can run at once; the ESPHome path
is the recommended one and can replace the MQTT scanner + companion integration.

## Diagnostics

The firmware publishes a telemetry line on `wbrg1/<id>/telemetry` every 30 s and
auto-creates Home Assistant sensors via MQTT discovery (entity category
*diagnostic*, availability tied to the `online`/`offline` LWT):

- **WiFi RSSI**, **Free heap**, **Uptime**, **Adverts** (total seen),
  **Reconnects** (broker (re)connections after the first — a steady climb means
  the link is flapping), **Queue drops** (BLE ring overflow — a small one-time
  count at boot is normal).

```json
wbrg1/<id>/telemetry  {"rssi":-67,"heap":105952,"uptime":31,"adverts":597,"recon":0,"drops":78}
```

## Passive vs active scanning

`SCAN_ACTIVE` in `config.h` (default `0` = passive). Passive never transmits —
the safe default next to a co-located Zigbee radio. Active additionally sends
scan requests, which pulls **scan responses** (so more devices report a **name**)
and yields more RSSI samples per second (smoother presence). In testing on a
combined ZS3L+WBRG1 gateway, active roughly doubled the sample rate and surfaced
device names, and the co-located Zigbee router stayed online — but that was a short
test. If you enable active, watch the router's link quality (visible in HA) and the
WBRG1's reconnect counter, and revert with one OTA if either degrades.

---

## Hard-won lessons

Everything below cost real time to discover; it's here so it doesn't cost you any.

- **`LOG_TX` (PA7) is the download strap.** A programmer RX line holding it low at
  reset boots the ROM downloader — the #1 "it's bricked / it's dead" red herring.
  Keep RX off `LOG_TX` during reset.
- **Baud stays 115200** for the RTL8720D ROM. `-b 1500000` is for the RTL8710B and
  just fails here.
- **`rtltool.py gf`/register commands can't work in download mode** — only
  `rf`/`wf` (which upload the SRAM flashloader first) do. Probing with `gf` sends
  you in circles.
- **rtltool's handshake is too impatient** (0.2 s/byte); the loader goes quiet
  while erasing/reading a sector. The tools here subclass it with a 3 s wait.
- **On UART write, write the app before the bootloaders** is *not* needed here —
  `uart_flash.py` erases per-slot and writes all three cleanly. (This differs from
  the EFR32 on the same board, where bootloader-last matters.)
- **MQTT keepalive flapping.** This core's `PubSubClient::loop()` has a `delay(500)`
  and drives keepalive off *inbound* silence; with no subscriptions that made the
  client self-disconnect every keepalive interval and starved the BLE queue.
  Fixes in the firmware: drain the ring every loop pass, service MQTT on a timer,
  and set a large `keepAlive` (we publish every second, so the broker never times
  us out and the fragile ping path is never exercised).
- **OTA image format is the whole game.** `http_update_ota` (and the DownloadServer
  socket path) want the `OTA_All` container, not the raw image2. Serving the raw
  image makes the device abort ~15% in while parsing. `make_ota.py` replicates the
  format from `rtl8721d_ota.c`:
  - `update_file_hdr` (8B): `u32 FwVer; u32 HdrNum=1;`
  - `update_file_img_hdr` (24B): `u8 ImgId[4]="OTA\0"; u32 ImgHdrLen=24; u32
    Checksum; u32 ImgLen; u32 Offset=32; u32 FlashAddr=0;`
  - then the whole `km0_km4_image2.bin`. `Checksum` = u32 sum of all its bytes.
- **OTA targets the inactive slot.** `ota_get_cur_index()` reads the flash MMU;
  booting from OTA1 (`0x6000`) means OTA writes OTA2 (`0x106000`). A malformed raw
  image crashing the writer looks like a hang — that was our earlier "socket OTA
  hangs the device", not a wrong-slot erase.
- **OTA logs go to the KM0 log UART**, a different pin than KM4's `Serial`. Don't
  expect the SDK's OTA `printf`s on the pin you read; the firmware prints its own
  `[ota] ...` lines on KM4 instead.
- **AMB22 (RTL8722CSM) is the build target** for the RTL8721CSM. A clean compile
  proves the toolchain, not compatibility — always back up before writing.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Module "dead" after reset, no firmware | RX held `LOG_TX`/PA7 low at reset → booted ROM downloader. Remove RX, reset. |
| ROM never prints `#Flash Download Start` | Entry order wrong; resistor missing; TX/RX swapped; no shared GND. |
| `Error read data head id` / garbage from rtltool | Impatient handshake, or not in download mode. Use the tools here; re-enter download mode. |
| OTA download aborts partway (~15 %) | Served the raw image, not an `OTA_All` — wrap it with `make_ota.py`. |
| OTA "succeeds" but marker doesn't change | Serving stale image, or the `ota` command didn't reach the module (check subscription / topic / broker auth). |
| MQTT reconnects every ~30–120 s | Keepalive path; make sure you're on this firmware (large keepAlive + timered service). |

## Repo layout

```
wbrg1_ble_proxy/            # the Arduino sketch
  wbrg1_ble_proxy.ino       #   scan callback, coalescing, loop, OTA trigger
  advert.h                  #   Advert record, AdvertSink interface, SPSC ring
  sink_mqtt.{h,cpp}         #   MQTT transport (swappable behind AdvertSink)
  config.h.example          #   copy to config.h and fill in
docs/
  FLASHING.md               # the ROM protocol, wiring, the PA7 trap, per-board notes
tools/
  requirements.txt          # pyserial + paho-mqtt (install before wiring anything up)
  make_ota.py               # wrap km0_km4_image2.bin -> OTA_All container  (the key tool)
  ota_httpd.py              # tiny HTTP server that logs bytes served (for OTA)
  uart_flash.py             # first-time bootstrap flash over UART
  uart_dump.py              # back up the stock 8 MB flash over UART
  fetch_vendor_tools.sh     # fetch rtltool.py + flashloader (OpenBeken FlashTools, GPL)

  # bootstrap / recovery path -- what you need to convert a NEW module or
  # rescue one that will not boot. All of these talk to the ROM bootloader.
  xsend_rtk.py              # enter download mode, upload the SRAM flashloader
  dump.py                   # read flash through the resident loader (backup)
  verify_dump.py            # sanity-check an 8 MB backup before writing anything
  flash.py                  # erase + write the three images, then verify by read-back
  restore.py                # put the stock Tuya firmware back
  listen.py                 # watch LOG_TX (boot log, or ROM NAK polling)
  esphome_gatt_test.py      # exercise connect + GATT discovery over the API
  pcycle.sh                 # power-cycle the gateway via an HA smart plug
  ota_push.sh               # legacy DownloadServer push (superseded by make_ota.py)
  grab.py xsend.py xsend1k.py try-rom.sh jig.py    # earlier attempts, kept for the record
```

Host tooling needs `pyserial` and `paho-mqtt`; without them the UART tools fail
at import, which is easy to misread as a hardware fault mid-flash:

```sh
python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt
```

Most tools honour `WBRG1_UART` (serial port) and `WBRG1_BACKUP` (which board's
stock dump to check). Set `WBRG1_BACKUP` explicitly when more than one module
exists — each backup is that unit's only way back to Tuya, and they are not
interchangeable.

## Credits

- Realtek AmebaD Arduino core: [Ameba-AIoT/ameba-arduino-d](https://github.com/Ameba-AIoT/ameba-arduino-d)
- `rtltool.py` + AmebaD flashloader: [openshwprojects/FlashTools](https://github.com/openshwprojects/FlashTools) (GPL) — fetched, not redistributed here
- OTA format reference: `rtl8721d_ota.c` in the AmebaD SDK
- Presence consumer: [Bermuda](https://github.com/agittins/bermuda)

Not affiliated with Tuya or Realtek. Use at your own risk; you are reflashing a
module — keep your backup.
