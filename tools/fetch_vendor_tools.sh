#!/bin/sh
# Fetch the vendor UART tools needed for the FIRST (bootstrap) flash:
#   - rtltool.py               (Realtek RTL872xDx ROM bootloader utility)
#   - imgtool_flashloader_amebad.bin  (SRAM flashloader the ROM runs)
# Source: openshwprojects/FlashTools (GPL). Not redistributed here.
set -e
D="$(dirname "$0")"
base="https://raw.githubusercontent.com/openshwprojects/FlashTools/master/Realtek/AmebaD_FlashTool/RTL872xDx"
for f in rtltool.py imgtool_flashloader_amebad.bin; do
    echo "fetching $f"
    curl -fsSL "$base/$f" -o "$D/$f"
done
echo "done. rtltool.py + imgtool_flashloader_amebad.bin are in $D"
