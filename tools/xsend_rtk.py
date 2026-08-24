#!/usr/bin/env python3
"""Load the AmebaD flashloader into SRAM using Realtek's XMODEM variant, then
hand off to rtltool for a flash read.

Realtek's frame (from rtltool.send_xmodem) is NOT standard XMODEM:
    STX, seq, 0xFF-seq, <u32 LE dest address>, 1024 data (pad 0xFF), sum8(addr+data)
preceded by CMD_XMD (0x07) and closed with EOT, at which point the ROM jumps
to 0x00082000. SRAM only - no flash is written by this script.

  usage: xsend_rtk.py [wait_s] [read_size] [outfile]
"""
import os, struct, subprocess, sys, time
import serial

PORT, LOADER, DEST = os.environ.get('WBRG1_UART', '/dev/cu.usbmodem11302'), 'imgtool_flashloader_amebad.bin', 0x00082000
STX, EOT, ACK, NAK, CAN = b'\x02', b'\x04', b'\x06', b'\x15', b'\x18'
CMD_XMD, CMD_RWA = b'\x07', b'\x31'

args=[a for a in sys.argv[1:] if not a.startswith("--")]
wait  = int(args[0]) if len(args) > 0 else 90
rsize = args[1] if len(args) > 1 else "0x10000"
out   = args[2] if len(args) > 2 else "test64k.bin"

data = open(LOADER, 'rb').read()
npkt = (len(data) + 1023) // 1024
print(f"flashloader {len(data)} bytes -> {npkt} x 1K packets to 0x{DEST:08X}")

s = serial.Serial(PORT, 115200, timeout=2)

def burst(secs=0.5, limit=32):
    """read whatever arrives within secs (bounded)"""
    end, b = time.time() + secs, b""
    s.timeout = 0.1
    while time.time() < end and len(b) < limit:
        c = s.read(limit - len(b))
        if c: b += c
    s.timeout = 2
    return b

def wait_ack(secs=3.0):
    """ACK/NAK/CAN after a packet; tolerate a stale in-flight NAK."""
    end, saw_nak = time.time() + secs, False
    s.timeout = 0.1
    while time.time() < end:
        c = s.read(1)
        if c == ACK: s.timeout = 2; return 'ACK'
        if c == CAN: s.timeout = 2; return 'CAN'
        if c == NAK:
            if saw_nak: s.timeout = 2; return 'NAK'
            saw_nak = True; end = min(end, time.time() + 0.3)
    s.timeout = 2
    return 'NAK' if saw_nak else 'TIMEOUT'

def rd_regs(addr):
    s.reset_input_buffer()
    s.write(CMD_RWA + struct.pack('<I', addr)); s.flush()
    return burst(0.6, 8)

ALREADY = '--already' in sys.argv   # ROM already in download mode: skip phase 0 + banner wait
if ALREADY:
    s.reset_input_buffer()
    t = time.time() + 5; s.timeout = 0.1; seen = False
    while time.time() < t:
        if s.read(1) == NAK: seen = True; break
    s.timeout = 2
    print("ROM is NAK-polling - already in download mode" if seen else "no NAK - ROM not in download mode", flush=True)
    if not seen: s.close(); sys.exit(1)

if not ALREADY:
    # -- phase 0: sanity - does the module still boot Tuya normally? ---------------
    # (confirms flash is intact before we trust any backup we take from it)
    s.reset_input_buffer()
    print("PHASE 0: listening 20s - tap CHIP_EN to GND and release (NO resistor)", flush=True)
    dl, boot = time.time() + 20, b""
    while time.time() < dl:
        d = s.read(1024)
        if d: boot += d
        if b"TUYA" in boot and len(boot) > 2000: break
    if b"TUYA" in boot:
        print(f"   OK - Tuya firmware boots ({len(boot)} bytes of log). Flash intact.", flush=True)
    elif b"Flash Download Start" in boot:
        print("   saw the download banner instead - resistor was on. Skipping boot check.", flush=True)
    else:
        print(f"   WARNING: no Tuya boot log ({len(boot)} bytes: {boot[:60]!r}). Continuing anyway.", flush=True)

    # -- phase 1: wait for download mode ----------------------------------------
    s.reset_input_buffer()
    print(f"PHASE 1: waiting up to {wait}s for banner - now do the full sequence (with resistor)", flush=True)
    dl, buf = time.time() + wait, b""
    while time.time() < dl:
        d = s.read(512)
        if d:
            buf += d
            if b"Flash Download Start" in buf: break
    else:
        print("banner never appeared"); s.close(); sys.exit(1)
    t = time.time() + 10
    while time.time() < t:
        if s.read(1) == NAK: break
    else:
        print("no NAK"); s.close(); sys.exit(1)
    print("banner + NAK seen", flush=True)

# -- A: ROM command monitor alive? -----------------------------------------
r = rd_regs(DEST)
print(f"A  RWA 0x{DEST:08X} -> {r.hex(' ') or '(nothing)'}   (expect 31 xx xx xx xx 15)")

# -- B: enter xmodem mode, then wait for the receiver's NAK initiation byte --
s.reset_input_buffer(); s.write(CMD_XMD); s.flush()
r = burst(0.6, 4)
print(f"B  CMD_XMD -> {r.hex(' ') or '(nothing)'}   (expect 06)")
t = time.time() + 3; s.timeout = 0.1; init = None
while time.time() < t:
    c = s.read(1)
    if c == NAK: init = 'NAK'; break
s.timeout = 2
print(f"B  initiation byte: {init or 'none seen (sending anyway)'}")

# -- C: packets -------------------------------------------------------------
# OpenBeken (RTLFlasher.cs): packet 1 is a plain XMODEM-1K frame; packets 2+
# carry a 4-byte LE destination address after seq/~seq, checksum over addr+data.
# rtltool puts the address on every packet. Try OBK's way first, fall back.
def frame(seq, off, with_addr):
    body = data[off:off+1024].ljust(1024, b'\xFF')
    hdr  = bytes([0x02, seq & 0xFF, 0xFF - (seq & 0xFF)])
    payload = (struct.pack('<I', DEST + off) if with_addr else b'') + body
    return hdr + payload + bytes([sum(payload) & 0xFF])

def send_pkt(seq, off, with_addr, tries):
    for attempt in range(1, tries + 1):
        s.reset_input_buffer(); s.write(frame(seq, off, with_addr)); s.flush()
        v = wait_ack()
        print(f"C  pkt {seq}/{npkt} @0x{DEST+off:08X} {'addr ' if with_addr else 'plain'} try {attempt}: {v}", flush=True)
        if v in ('ACK', 'CAN'): return v
    return 'NAK'

seq, off, ok = 1, 0, True
while off < len(data):
    if seq == 1:
        v = send_pkt(seq, off, False, 2)            # OBK style
        if v != 'ACK': v = send_pkt(seq, off, True, 2)   # rtltool style
    else:
        v = send_pkt(seq, off, True, 3)
        if v != 'ACK': v = send_pkt(seq, off, False, 1)  # diagnostic only
    if v != 'ACK':
        ok = False; break
    seq += 1; off += 1024
if not ok:
    s.close(); sys.exit(2)

# -- D: EOT -> ROM jumps to flashloader ---------------------------------------
s.reset_input_buffer(); s.write(EOT); s.flush()
r = burst(0.6, 4)
print(f"D  EOT -> {r.hex(' ') or '(nothing)'}   (expect 06)")
time.sleep(0.5)
r = burst(1.0, 64)
if r: print(f"   flashloader said: {r!r}")

# -- E: is the flashloader answering? ----------------------------------------
r = rd_regs(DEST)
print(f"E  RWA 0x{DEST:08X} -> {r.hex(' ') or '(nothing)'}   (expect 31 21 20 08 00 15)")
s.close()

time.sleep(0.3)
print("\nhanding to rtltool ...", flush=True)
sys.exit(subprocess.call(['python3', 'rtltool.py', '-p', PORT, 'rf', '0', rsize, out]))
