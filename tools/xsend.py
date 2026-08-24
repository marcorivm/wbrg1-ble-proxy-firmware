#!/usr/bin/env python3
"""Enter-and-load helper for the AmebaD ROM UART download mode.

The ROM, after '#Flash Download Start', is a plain XMODEM receiver (it floods
NAK 0x15 asking for a sender). rtltool instead expects a ROM command monitor,
so its first ReadRegs fails and it bails. This sends the flashloader the way
the ROM actually wants it, then hands over to rtltool.

  usage: xsend.py [wait_seconds]
Reads nothing from flash and writes nothing to flash: the flashloader is
loaded into SRAM only.
"""
import sys, time, subprocess
import serial

PORT   = '/dev/cu.usbmodem11302'
LOADER = 'imgtool_flashloader_amebad.bin'
SOH, EOT, ACK, NAK, CAN = b'\x01', b'\x04', b'\x06', b'\x15', b'\x18'
wait = int(sys.argv[1]) if len(sys.argv) > 1 else 90

data = open(LOADER, 'rb').read()
print(f"flashloader: {len(data)} bytes -> {(len(data)+127)//128} packets of 128")

s = serial.Serial(PORT, 115200, timeout=1)
s.reset_input_buffer()
print(f"waiting up to {wait}s for '#Flash Download Start' — run the sequence now", flush=True)
deadline, buf, seen = time.time() + wait, b"", False
while time.time() < deadline:
    d = s.read(512)
    if d:
        buf += d
        if b"Flash Download Start" in buf:
            seen = True; break
if not seen:
    print("banner never appeared"); s.close(); sys.exit(1)
print("banner seen; waiting for NAK handshake ...", flush=True)

# wait for the receiver's NAK
t = time.time() + 10
got = False
while time.time() < t:
    c = s.read(1)
    if c == NAK:
        got = True; break
if not got:
    print("no NAK from receiver — not in XMODEM mode"); s.close(); sys.exit(1)
print("NAK received, sending ...", flush=True)

STX = b'\x02'

def build(seq, chunk, use_crc, big):
    hdr = STX if big else SOH
    body = chunk.ljust(1024 if big else 128, b'\x1a')
    if use_crc:
        crc = 0
        for b in body:
            crc ^= b << 8
            for _ in range(8):
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
        tail = bytes([crc >> 8, crc & 0xFF])
    else:
        tail = bytes([sum(body) & 0xFF])
    return hdr + bytes([seq & 0xFF, 255 - (seq & 0xFF)]) + body + tail

# Diagnostic: try each framing variant once on packet 1 and report the reply.
print("\n--- probing framing variants on packet 1 ---")
for big in (False, True):
    for use_crc in (False, True):
        s.reset_input_buffer()
        pkt = build(1, data[:1024 if big else 128], use_crc, big)
        s.write(pkt); s.flush()
        time.sleep(0.4)
        r = s.read(64)
        name = f"{'STX-1024' if big else 'SOH-128'}/{'CRC' if use_crc else 'checksum'}"
        verdict = "ACK" if (r and ACK in r) else ("NAK-only" if r and set(r) <= {0x15} else "other")
        print(f"  {name:<22} reply={r[:16]!r}  -> {verdict}")
        if r and ACK in r:
            print(f"  >>> {name} ACCEPTED")
ok = False
sent = seq = 0

if ok:
    s.write(EOT); s.flush()
    r = s.read(1)
    print(f"sent {sent} bytes in {seq-1} packets; EOT -> {r!r}")
s.close()
if not ok:
    sys.exit(2)

time.sleep(0.5)
print("\nhanding over to rtltool ...", flush=True)
sys.exit(subprocess.call(['python3','rtltool.py','-p',PORT,'rf','0','0x10000','test64k.bin']))
