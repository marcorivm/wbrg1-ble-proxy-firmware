#!/bin/sh
# Poll for the AmebaD ROM bootloader while the CHIP_EN / PA7 entry dance is done.
#   usage: sh try-rom.sh [seconds]
cd "$(dirname "$0")"
SECS=${1:-60}
end=$(( $(date +%s) + SECS ))
n=0
while [ $(date +%s) -lt $end ]; do
	n=$((n+1))
	out=$(timeout 15 python3 rtltool.py -p /dev/cu.usbmodem11302 gf 2>&1)
	if ! echo "$out" | grep -qE "Error read data head id|Error Read|could not open|Connecting\.\.\.$"; then
		echo
		echo "=== attempt $n: ROM BOOTLOADER RESPONDED ==="
		echo "$out"
		exit 0
	fi
	printf .
done
echo
echo "no ROM response in ${SECS}s over $n attempts"
exit 1
