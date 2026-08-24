# WBRG1 (RTL8721CSM / AmebaD) — UART flashing notes

What it actually took to talk to the ROM bootloader on the Tuya WBRG1 inside the
JZZWG-TY2.0 gateway, with a Pi Pico debugprobe as the USB-UART bridge. Everything
below was established on real hardware on 2026-08-21; nothing here is guessed.

## Wiring (board 1, JZZWG-TY2.0 — pads are labelled on the PCB)

| Pico | phys | → | board |
|---|---|---|---|
| GP5 (UART RX) | 7 | ← | `LOG_TX` (module pin 7, PA7) |
| GP4 (UART TX) | 6 | → | `LOG_RX` (module pin 8, PA8) |
| GND | 3 | ↔ | GND |

Plus, only during entry: a jumper `CHIP_EN → GND` and a **1 kΩ** `LOG_TX → GND`.
Never short LOG_TX straight to ground — it is a driven output.

Sanity check before anything else: tap CHIP_EN to GND and release with the Pico
listening at 115200. The Tuya firmware dumps ~11 KB of boot log. If that appears,
contact, direction and baud are all right. (Loopback GP4↔GP5 proves the Pico side.)

## Entering download mode

1. `CHIP_EN → GND` (hold)
2. `LOG_TX → GND` through the 1 kΩ (hold)
3. release `CHIP_EN` — the ROM samples PA7 low as it comes out of reset
4. remove the resistor — **mandatory**, the module cannot answer while it is on

The ROM prints `#Flash Download Start` and then polls NAK (`0x15`) continuously.
It stays there indefinitely; it is not lost if the tool is slow to start.
`flashtool/xsend_rtk.py --already` skips the wait if the ROM is already polling.

## The protocol (why off-the-shelf tools failed)

Baud **stays at 115200** for RTL8720D. `-b1500000` is for RTL8710B; here it just
fails with "Error Set Baud" and is a red herring.

The ROM is an XMODEM-1K receiver with **Realtek's frame**, not standard XMODEM:

```
STX(0x02) seq ~seq <u32 LE destination address> <1024 data, pad 0xFF> sum8(addr+data)
```

- 128-byte SOH packets are ignored outright (no reply at all).
- The address goes on **every** packet including the first. OpenBeken's flasher
  sends packet 1 plain and gets NAKed on this ROM; `rtltool`'s framing is right.
- Send `CMD_XMD` (`0x07`) first, expect ACK, then wait for the NAK initiation byte.
- `EOT` at the end → ACK, and the ROM **jumps** to the image just loaded.

Load `imgtool_flashloader_amebad.bin` (4688 B, 5 packets) to `0x00082000`. It
announces `UARTIMG_Download 2`, and `0x31`-read of `0x82000` returns
`31 21 20 08 00 15` — its signature. From then on it serves the command set
(`0x19`/`0x20` read flash, `0x17` erase, `0x07`+XMODEM write to `0x08xxxxxx`).

## Tool gotchas

- `rtltool.py gf` can **never** work in download mode: it skips the loader upload.
  Only `rf`/`wf` go through `Floader()`. Probing with `gf` is what sent the first
  attempts in circles.
- `rtltool`'s `WaitResp` gives up after 0.2 s of silence per byte. The loader goes
  quiet while it reads a 4 KB sector, so `rf` dies with "Error read block head id".
  `dump.py` subclasses `RTLXMD` with a 3 s wait and sends `CAN CAN` + flush on
  connect (the Pico's CDC ignores the DTR/RTS reset dance, so stale bytes persist).
- `Floader()` references a module global `rtl`; set `rtltool.rtl = obj` before
  calling it from outside.
- Throughput is ~11 KB/s at 115200: 64 KB in 6 s, 8 MB in ~13 min.

## What the flash holds

Offset 0 starts `99 99 96 96 3F CC 66 FC` — the AmebaD image signature — followed
by KM0 boot code. MAC address lives in eFuse, not flash; calibration appears to be
outside the normally-readable flash as well. The backup is the way back to Tuya,
not the module's radio identity.

Backup: `~/Projects/personal/wbrg1-tuya-backup/wbrg1-tuya-full-8mb.bin`.

## Files

| file | role |
|---|---|
| `flashtool/rtltool.py` | vendor tool (OpenBeken `Realtek/AmebaD_FlashTool/RTL872xDx`), unmodified |
| `flashtool/xsend_rtk.py` | enter download mode, load the flashloader with the correct frame, hand off |
| `flashtool/dump.py` | read flash via the resident loader with a patient handshake |
| `flashtool/try-rom.sh`, `grab.py`, `xsend.py`, `xsend1k.py` | earlier attempts, kept for the record; superseded |

## Writing (done 2026-08-21, verified)

`flashtool/flash.py <ameba_d_tools dir>` — refuses to run without the 8 MB backup,
then for each image: erase sectors → `0x07` + addressed XMODEM to `0x08000000|offset`
→ full read-back compare. Three images, the same three Realtek's uploader writes:

| image | offset | size |
|---|---|---|
| `km0_boot_all.bin` | `0x000000` | 4500 B |
| `km4_boot_all.bin` | `0x004000` | 4456 B |
| `km0_km4_image2.bin` | `0x006000` | 860160 B |

System data (`0x2000`–`0x3FFF`) untouched. Tuya's OTA2 image around `0x286000` is
left in place and inert (blank system data ⇒ bootloader runs OTA1).

Timing at 115200: bootloaders 0.6 s each, image2 ~80 s write + ~80 s verify.

Restoring Tuya = the same script pointed at the backup: erase `0x0`–`0xD8000` and write
`wbrg1-tuya-full-8mb.bin[0:0xD8000]` to `0x08000000` (or the whole 8 MB if in doubt).

## First boot (2026-08-21) — and the PA7 trap

It works: WiFi join in ~3 s, MQTT connected, ~650 BLE adverts per 30 s with zero
queue drops, passive scan, Zigbee router on the same board unaffected.

**The Pico's UART RX (GP5) holds PA7 low at reset.** With GP5 on LOG_TX, every
CHIP_EN tap lands in ROM download mode (`#Flash Download Start`, no
`#calibration_ok`) — the ROM checks the pin before it touches flash, so this looks
exactly like a rejected image and is nothing of the sort. Two hours went into that.

Rule: **nothing on LOG_TX while the module comes out of reset.** To capture a boot
log, tap CHIP_EN with GP5 unplugged and reconnect it within a couple of seconds;
the sketch starts printing at +200 ms and the WiFi/MQTT lines follow over the next
few seconds. A ≥100 kΩ series resistor on GP5 would probably make this go away
(RP2040 pads default to a pull-down); untested.

Steady-state log on LOG_TX: a `[wbrg1] adverts seen: N  queue drops: N  link: up`
line every 30 s. `Serial` on this core is `LOGUARTClass`, i.e. PA7/PA8.

## End to end working (2026-08-22)

Full chain confirmed live: WBRG1 passive scan → MQTT `wbrg1/wbrg1-gw1/adv` →
HA integration (`hass-wbrg1-ble-proxy`, HACS) → `BaseHaRemoteScanner` → Bermuda.
`bermuda/dump_devices` shows `bermuda_wbrg1_gw1` hearing 18+ devices.

### Firmware v2 fix (the MQTT flap)
Root cause: this core's `PubSubClient::loop()` has `delay(500)` in it, and its
keepalive is driven by *inbound* silence — with no subscriptions the only inbound
is PINGRESP, and the impl self-closes the socket at each keepAlive boundary.
Symptom was a clean client-side reconnect every keepAlive seconds.
Fixes in `sketches/wbrg1_ble_proxy/`:
- drain the BLE ring every loop pass; service `sink.loop()` on a 1 s timer so the
  `delay(500)` never starves the queue (queue drops went from ~1800/cycle to 0),
- `setKeepAlive(3600)` — we publish every 1 s so the broker sees us as alive; the
  fragile ping path is then never exercised, while a real socket drop is still
  caught by `connected()`/reconnect,
- `setSocketTimeout(5)` and a reconnect counter in the 30 s status line.

## OTA: wired, apply step still failing (2026-08-22, open)

The running firmware (UART-flashed) includes an OTA server: on first WiFi connect it
starts mDNS (`_arduino._tcp`, id `wbrg1-gw1`) and `ota.beginOTA(8082)`, and prints
`[ota] ready on port 8082`. Port 8082 confirmed open from the LAN.

Push tooling: `flashtool/ota_push.sh <ip> [image] [port]` runs Realtek's
`DownloadServer.darwin <port> <km0_km4_image2.bin> <ip>` (the stock arduino-cli
network-upload recipe points at a nonexistent `misc/OTA_All.bin`, so we call
DownloadServer directly with the real build output).

**Status: not working yet.** DownloadServer connects (computes checksum, opens the
socket to :8082, exits 0) and the device's OTA server closes afterwards — but the
image is NOT applied: a marker change (`online` -> `online ota-ok` on the retained
status topic) did not appear after the push, nor after a manual reboot. So the
image either failed the device-side checksum/verify or the transfer framing didn't
match. `beginOTA` appears one-shot (the server thread exits after one connection;
8082 is closed until the next reboot), so each attempt needs a reboot to retry.

Device is healthy throughout — still scanning and publishing ~1 batch/s.

Next debug loop (needs UART visibility):
1. reboot (CHIP_EN tap, GP5 off LOG_TX) to restart the OTA server,
2. listen on LOG_TX and push: `ota_push.sh 192.168.0.175`,
3. read `ota_http`'s printfs (file_info / checksum verify / signature) to see the
   failure point.
Fallback: switch the firmware to the HTTP-pull path (`http_update_ota(server,port,
resource)` triggered via an MQTT command) and serve the image from a plain HTTP
server, instead of the DownloadServer push protocol.

## OTA debug findings (2026-08-22) — needs a redesign, not a fix

Captured LOG_TX (KM4) during a DownloadServer push against the always-on
`beginOTA(8082)` server. What we learned:

- `reconnects: 0` — the keepalive/flap fix is solid.
- `queue drops: 350` appeared *during* the push: **`beginOTA()` blocks the main
  loop** while handling a connection, so `drainQueue()` stalls and the BLE ring
  overflows. An always-listening `beginOTA` in `loop()` is the wrong shape.
- **No `beginOTA` step-printfs on our UART** ("Waiting for client", "Receive
  file_info", etc.), even though the transfer was attempted. AmebaD KM0 and KM4
  have separate log UARTs; the OTA lib's printf is almost certainly on KM0's,
  which is not the pin we read (Serial = KM4 LOGUART, PA7/PA8). We are blind to
  ota_http progress on this UART.
- `DownloadServer` (itself a *server*) + `beginOTA` (also a *server*) is a
  suspicious pairing; the transfer never completed and the image was not applied.

### Recommended redesign (next session)
Drop always-on `beginOTA`. Instead trigger OTA on demand and off the scan loop:
- add an MQTT command topic (e.g. `wbrg1/<id>/cmd`); on `"ota"`, call
  `ota.start_OTA_threads(port, server_ip)` (threaded — does not block loop) or
  `http_update_ota(server,port,resource)` and serve `km0_km4_image2.bin` from a
  plain `python3 -m http.server`.
- to see failures, bridge KM0's log UART too, or add our own Serial prints around
  the OTA call on KM4.
The OTA server/tooling groundwork (port, DownloadServer, ota_push.sh) stays useful
for reference. Until then, firmware updates use the proven UART path (this file).

## OTA attempt 2 (on-demand HTTP pull) — root-caused to an image-format wall

Redesigned OTA to on-demand: MQTT command `ota <host> <port> <resource>` ->
`http_update_ota()` (threaded HTTP pull) -> reboot. Removed the blocking always-on
`beginOTA`. Firmware flashed and healthy (marker `online ota-ok`, scanning, no flap,
8082 no longer open). The MQTT->HTTP chain works: the device connects to our HTTP
server and starts downloading.

**But the image is rejected.** With a byte-counting server we measured the device
pulling exactly **138416 / 884736 bytes (15%)** then closing — the same early abort
on both the HTTP and the earlier DownloadServer/socket path. Cause: the AmebaD OTA
parser expects an **OTA-format file** (`update_file_hdr{FwVer,HdrNum}` + `ImgId[4]`
tagged sub-image headers, per rtl8721d_ota.h), but every image the Arduino build
produces (`km0_km4_image2.bin` etc.) starts with the `81958711` image2 signature,
not that OTA header. The build ships **no OTA-format image** and there's no
packaging tool in the core. The OTA library's own error messages go to the **KM0
log UART** (a different pin than our KM4 Serial), so the failure is invisible on the
pin we read.

Net: OTA via this core needs either (a) reverse-engineering the OTA file format to
wrap `km0_km4_image2.bin` correctly, or (b) KM0-log-UART visibility to see the exact
reject reason, or (c) DownloadServer's socket protocol with a threaded server. All
are substantial work against a precompiled, undocumented OTA lib. Parked here.

**Firmware updates remain via UART** (this document's proven path). The device runs
the on-demand-OTA build; the MQTT `ota` command is harmless (fails safely, writes
only the inactive slot, device stays on the current image).

## OTA WORKS (2026-08-22) — resolved

The "parked" sections above were the debugging journey; OTA over WiFi now works.
Two things were missing, both fixed:

1. **Image container.** `http_update_ota()` expects the SDK **OTA_All** format, not
   the raw `km0_km4_image2.bin`. Format (from `rtl8721d_ota.c`), a 32-byte header:
   - `update_file_hdr` (8B): `u32 FwVer; u32 HdrNum(=1);`
   - `update_file_img_hdr` (24B): `u8 ImgId[4]="OTA\0"; u32 ImgHdrLen(=24);
     u32 Checksum; u32 ImgLen; u32 Offset(=32); u32 FlashAddr(=0, device overrides);`
   - then the whole `km0_km4_image2.bin` (its first 8 bytes are the signature).
   `Checksum` = u32 sum of all image bytes. `flashtool/make_ota.py` builds it.
   Serving the raw image was the "aborts at 15%" failure.
2. **A trigger bug of ours** — the MQTT handler set `otaPending` but the action was
   missing from `loop()`; re-added.

`ota_get_cur_index()` reads the MMU: the device boots OTA1 (0x6000), so OTA targets
OTA2 (0x106000) — the inactive slot. Safe; the earlier socket-path hang was the
malformed raw image crashing the writer, not a wrong-slot erase.

### The OTA workflow (no UART)
```sh
WB=~/Projects/personal/wbrg1-ble
TD=$WB/arduino-data/packages/realtek/tools/ameba_d_tools/1.1.3
# 1. build
arduino-cli --config-file $WB/arduino-cli.yaml compile \
  --fqbn realtek:AmebaD:Ameba_AMB21_AMB22 $WB/sketches/wbrg1_ble_proxy
# 2. wrap into the OTA_All container
python3 $WB/flashtool/make_ota.py $TD/km0_km4_image2.bin /tmp/ota_all.bin
# 3. serve it (any HTTP server; ota_httpd.py logs bytes)
python3 $WB/flashtool/ota_httpd.py /tmp/ota_all.bin 8000 &
# 4. tell the module to pull it (MQTT), <mac-ip> = this host on the module's subnet
mosquitto_pub -h <broker> -u <user> -P <pass> -t wbrg1/wbrg1-gw1/cmd \
  -m "ota <mac-ip> 8000 /ota_all.bin"
```
The module pulls (~7 s), verifies checksum, flips the slot, reboots into the new
image (~20-30 s total). Confirm via the retained `wbrg1/wbrg1-gw1/status`.
Firmware in the repo is the on-demand OTA build; UART is only ever needed if OTA
itself is broken by a bad push (the inactive-slot design makes that recoverable).

## Board 2: RSH-GW018-DM (2026-08-24) — second module converted

Same ZS3L + WBRG1 combo as the JZZWG-TY2.0, converted the same way. Two things
differ and both matter.

### No soldering — the WBRG1 UART is on a header

This board brings the WBRG1 out on **P1**, so there are no module pads to solder:

| header | module | pinout |
|---|---|---|
| **P1** | **WBRG1** | **1 GND · 2 LogTX · 3 LogRX · 4 EN** |
| P2 | ZS3L | 1 Vcc · 2 GND · 3 RX1 · 4 TX1 |
| P3 | ZS3L | 1 GND · 2 RST2 · 3 SWCLK · 4 SWDIO |

Wiring: Pico GP5 → P1-2 (LogTX), GP4 → P1-3 (LogRX), GND → P1-1. `EN` on P1-4
is the reset: tap it to GND to restart the module.

Because GP5 sits on LOG_TX, the PA7 strap is held low and **every EN tap lands
straight in ROM download mode** — convenient here (it is how you get in), but
see below for the trap on the way out.

### Getting it to BOOT is the hard part, not getting it to flash

The flash itself is quick and verifies clean. What cost the most time was the
PA7 strap on the way *out*: three boot attempts went back into download mode
because GP5 was still on LOG_TX at reset. Continuous `0x15` NAK polling on the
UART is the signature — that is the ROM, not your firmware.

The reliable procedure is to stop using the UART for the boot check entirely:

1. **Unplug GP5 from P1-2 and leave it off.**
2. Power-cycle the gateway (a power-on reset also takes the external flash out
   of XIP mode; an EN tap does not).
3. Find the module on the network instead of on the wire — it opens the ESPHome
   API on `:6053` and the control socket on `:6054`. `:6054` greets with
   `wbrg1 ctrl ready\n`, which identifies our firmware unambiguously; a
   HelloRequest/DeviceInfoRequest on `:6053` returns the `SCANNER_ID` name.

Note `:6053` is single-client: if Home Assistant holds the slot, a probe will
time out even though the device is perfectly healthy. Use `:6054` to check
liveness, and mind that the LAN here is a **/18** (192.168.0.0–192.168.63.255),
so a /24 sweep misses most of it.

The front LEDs are the fastest check of all: **red blinking = WiFi down, red
off = healthy, red solid = safe-mode**. Blue is a heartbeat that pulses only
once a minute, so "no blue" means very little — don't read it as a failure.

### Building for a second board

`SCANNER_ID` is selected at build time; nothing needs editing:

```sh
arduino-cli --config-file arduino-cli.yaml compile \
  --fqbn realtek:AmebaD:Ameba_AMB21_AMB22 sketches/wbrg1_ble_proxy \
  --build-property compiler.c.extra_flags=-DWBRG1_BOARD=2
```

**This platform's C++ recipe interpolates `{compiler.c.extra_flags}` and
`{build.extra_flags}` — NOT `compiler.cpp.extra_flags`.** Getting this wrong is
quiet and dangerous:

- `compiler.cpp.extra_flags` is silently dropped; you build a gw1 image and only
  notice because the ID string in the binary never changed.
- `build.extra_flags` *works*, but it **overwrites the board's own flags** from
  boards.txt (`-mthumb -DCORE_RTL8722DM -DCORE_RTL8722CSM -DBOARD_AMB21_AMB22
  -DArduino_STD_PRINTF`), producing a ~20 KB-smaller broken image.

Sanity check every per-board build by diffing it against the known-good image:
keeping the IDs the same length means it should differ by only a handful of
bytes (the ID digit plus the build timestamp), which also proves the XIP layout
did not move.

### Flash the RIGHT backup

`flash.py` refuses to run without an 8 MB backup, but it checks whatever
`WBRG1_BACKUP` points at — it cannot tell which board is on the wire. Each
module's dump is that unit's **only** way back to Tuya and the two are **not**
interchangeable, so set it explicitly:

```sh
WBRG1_BACKUP=~/Projects/personal/wbrg1-tuya-backup-gw018/wbrg1-gw018-tuya-full-8mb.bin \
PYTHONPATH=$PWD python3 flash.py <images_dir>
```

### If the USB link drops mid-write

Seen once on this board: the Pico de-enumerated during the image2 write
(`SerialException: [Errno 6] Device not configured`), leaving the app slot
erased and partly written. This is **not** a brick — ROM download mode lives in
mask ROM and is always reachable. Re-running `flash.py` re-erased and rewrote
from scratch and the read-back verify passed. Reseat the USB cable before a
long write; a drop during erase/write is the one way to make real trouble.
