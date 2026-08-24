#!/usr/bin/env python3
"""Wait for the AmebaD ROM download banner, then immediately run rtltool.

Removes the timing race between the CHIP_EN/PA7 dance and launching the tool:
do the dance whenever you like inside the window and this fires on the banner.

  usage: grab.py <size> <outfile> [wait_seconds]
"""
import subprocess, sys, time
import serial

PORT = '/dev/cu.usbmodem11302'
size = sys.argv[1] if len(sys.argv) > 1 else '0x10000'
out  = sys.argv[2] if len(sys.argv) > 2 else 'test64k.bin'
wait = int(sys.argv[3]) if len(sys.argv) > 3 else 90

s = serial.Serial(PORT, 115200, timeout=0.2)
s.reset_input_buffer()
print(f"waiting up to {wait}s for '#Flash Download Start' — run the sequence now", flush=True)
deadline = time.time() + wait
buf = b""
seen = False
while time.time() < deadline:
    d = s.read(512)
    if d:
        buf += d
        if b"Flash Download Start" in buf:
            seen = True
            print(f"  banner seen at t={round(wait-(deadline-time.time()),1)}s", flush=True)
            break
s.close()

if not seen:
    print("banner never appeared — download mode was not entered")
    sys.exit(1)

time.sleep(0.2)
cmd = ['python3', 'rtltool.py', '-p', PORT, 'rf', '0', size, out]
print("running:", ' '.join(cmd), flush=True)
sys.exit(subprocess.call(cmd))
