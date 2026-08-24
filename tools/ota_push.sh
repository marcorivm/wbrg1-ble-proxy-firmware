#!/bin/sh
# Push a new firmware image to the WBRG1 over WiFi (no UART), using Realtek's
# DownloadServer against the on-device OTA server (ota.beginOTA / mDNS).
#
#   usage: sh ota_push.sh <device-ip> [image.bin] [port]
#
# The image is the ordinary Arduino build output km0_km4_image2.bin; the OTA
# server writes it to the inactive slot, verifies checksum, flips the boot
# signature, and the module reboots into it.
# AMEBA_TOOLS = the core's ameba_d_tools dir (holds DownloadServer + build output).
TD=${AMEBA_TOOLS:-$HOME/Projects/personal/wbrg1-ble/arduino-data/packages/realtek/tools/ameba_d_tools/1.1.3}
IP=${1:?usage: ota_push.sh <device-ip> [image.bin] [port]}
IMG=${2:-$TD/km0_km4_image2.bin}
PORT=${3:-8082}
if [ ! -f "$IMG" ]; then echo "image not found: $IMG"; exit 1; fi
echo "pushing $(basename "$IMG") ($(wc -c < "$IMG") bytes) to $IP:$PORT ..."
"$TD/DownloadServer.darwin" "$PORT" "$IMG" "$IP"
