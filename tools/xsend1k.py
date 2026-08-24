#!/usr/bin/env python3
"""Send the AmebaD flashloader over XMODEM-1K, then hand off to rtltool.

The ROM ignores 128-byte SOH packets entirely and only responds to STX-1024,
so 1K framing is used throughout. Checksum mode (the receiver opens with NAK,
not 'C'). SRAM only - nothing is written to flash.

  usage: xsend1k.py [wait_seconds] [read_size] [outfile]
"""
import sys, time, subprocess
import serial

PORT   = '/dev/cu.usbmodem11302'
LOADER = 'imgtool_flashloader_amebad.bin'
SOH, STX, EOT, ACK, NAK, CAN = b'\x01', b'\x02', b'\x04', b'\x06', b'\x15', b'\x18'

wait  = int(sys.argv[1]) if len(sys.argv) > 1 else 90
rsize = sys.argv[2] if len(sys.argv) > 2 else '0x10000'
out   = sys.argv[3] if len(sys.argv) > 3 else 'test64k.bin'

data = open(LOADER, 'rb').read()
npkt = (len(data) + 1023) // 1024
print(f"flashloader {len(data)} bytes -> {npkt} x 1K packets")

s = serial.Serial(PORT, 115200, timeout=2)
s.reset_input_buffer()
print(f"waiting up to {wait}s for banner - run the sequence now", flush=True)
dl, buf = time.time() + wait, b""
while time.time() < dl:
    d = s.read(512)
    if d:
        buf += d
        if b"Flash Download Start" in buf:
            break
else:
    print("banner never appeared"); s.close(); sys.exit(1)
print("banner seen; waiting for NAK ...", flush=True)

t = time.time() + 10
while time.time() < t:
    if s.read(1) == NAK:
        break
else:
    print("no NAK"); s.close(); sys.exit(1)
print("NAK seen, sending 1K packets ...", flush=True)

seq, off = 1, 0
while off < len(data):
    body = data[off:off+1024].ljust(1024, b'\x1a')
    pkt = STX + bytes([seq & 0xFF, 255 - (seq & 0xFF)]) + body + bytes([sum(body) & 0xFF])
    for attempt in range(1, 7):
        s.reset_input_buffer()
        s.write(pkt); s.flush()
        r = s.read(1)
        if r == ACK:
            print(f"  pkt {seq}/{npkt} ACK"); break
        if r == CAN:
            print(f"  pkt {seq} CAN - receiver cancelled"); s.close(); sys.exit(2)
        print(f"  pkt {seq} try {attempt}: reply={r!r}")
    else:
        print(f"  pkt {seq} failed after 6 tries"); s.close(); sys.exit(2)
    seq += 1; off += 1024

s.write(EOT); s.flush()
print(f"EOT -> {s.read(1)!r}")
s.close()
time.sleep(0.5)
print("\nhanding to rtltool ...", flush=True)
sys.exit(subprocess.call(['python3','rtltool.py','-p',PORT,'rf','0',rsize,out]))
