#!/usr/bin/env python3
"""Print whatever the WBRG1 says on LOG_TX, as text, with timestamps.

  usage: listen.py [seconds] [baud]
"""
import sys, time
import serial

import os
PORT = os.environ.get('WBRG1_UART', '/dev/cu.usbmodem11302')
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

s = serial.Serial(PORT, baud, timeout=0.2)
s.reset_input_buffer()
print(f"listening {secs:.0f}s @{baud} ...", flush=True)
t0, end, total, line = time.time(), time.time() + secs, 0, b""
while time.time() < end:
    d = s.read(4096)
    if not d:
        continue
    total += len(d)
    naks = d.count(b"\x15")
    if naks and naks == len(d):          # pure loader NAK polling, not boot output
        nak_total = globals().get("nak_total", 0) + naks
        globals()["nak_total"] = nak_total
        if nak_total % 100 < naks:
            print(f"[{time.time()-t0:6.2f}] (still in flashloader: {nak_total} NAK polls, no boot yet)", flush=True)
        continue
    line += d
    if len(line) > 512 and b"\n" not in line:   # flush long partial lines
        print(f"[{time.time()-t0:6.2f}] {line.decode('utf-8','replace')}", flush=True); line = b""
    while b"\n" in line:
        out, line = line.split(b"\n", 1)
        print(f"[{time.time()-t0:6.2f}] {out.decode('utf-8', 'replace').rstrip()}", flush=True)
if line:
    print(f"[{time.time()-t0:6.2f}] {line.decode('utf-8', 'replace').rstrip()}", flush=True)
s.close()
print(f"--- {total} bytes in {secs:.0f}s ---")
