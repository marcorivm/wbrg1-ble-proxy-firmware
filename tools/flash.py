#!/usr/bin/env python3
"""Write the three Arduino AmebaD images to the WBRG1 via the resident
flashloader, then read every written range back and compare byte-for-byte.

Layout (Realtek stock, confirmed identical on the Tuya dump):
    0x000000  km0_boot_all.bin
    0x002000  system data  -- NOT TOUCHED
    0x004000  km4_boot_all.bin
    0x006000  km0_km4_image2.bin

Refuses to run unless the full 8 MB backup exists and is the right size.
Uses rtltool's protocol class with a patient handshake (see dump.py).

  usage: flash.py <images_dir>          # e.g. the ameba_d_tools/1.1.3 dir
"""
import os, sys, time, io
import rtltool

PORT   = os.environ.get('WBRG1_UART', '/dev/cu.usbmodem11302')
# The backup is this board's ONLY way back to Tuya, and the two boards' images
# are NOT interchangeable -- so point WBRG1_BACKUP at the backup of the board
# actually on the wire before flashing it.
BACKUP = os.environ.get('WBRG1_BACKUP',
    os.path.expanduser('~/Projects/personal/wbrg1-tuya-backup/wbrg1-tuya-full-8mb.bin'))
SECTOR = 4096
IMAGES = [  # (flash offset, filename)
    (0x000000, 'km0_boot_all.bin'),
    (0x004000, 'km4_boot_all.bin'),
    (0x006000, 'km0_km4_image2.bin'),
]

class Patient(rtltool.RTLXMD):
    def WaitResp(self, code=rtltool.ACK):
        saved = self._port.timeout
        self._port.timeout = 3.0
        try:
            for _ in range(4000):
                c = self._port.read(1)
                if not c: return None
                if c == code: return True
            return False
        finally:
            self._port.timeout = saved

def die(msg, code=1):
    print("ABORT:", msg); sys.exit(code)

imgdir = sys.argv[1] if len(sys.argv) > 1 else '.'
if not (os.path.exists(BACKUP) and os.path.getsize(BACKUP) == 0x800000):
    die("full 8 MB backup missing or wrong size - not writing anything")

plan = []
for off, name in IMAGES:
    path = os.path.join(imgdir, name)
    if not os.path.exists(path): die(f"missing {path}")
    data = open(path, 'rb').read()
    if not data: die(f"{name} is empty")
    end = off + len(data)
    plan.append((off, name, data))
    print(f"  {name:<22} 0x{off:06X}..0x{end:06X}  {len(data)} B")
# the boot images must fit in their 8 KB slots and never reach system data
for off, name, data in plan:
    if off == 0x000000 and len(data) > 0x2000: die("km0 boot would overrun into system data")
    if off == 0x004000 and len(data) > 0x2000: die("km4 boot would overrun into image2 slot")

# Optional: wait for the ROM download banner first, so the CHIP_EN/PA7 entry
# dance can be done at any point in the window.
if '--wait' in sys.argv:
    import serial as _serial
    _w = _serial.Serial(PORT, 115200, timeout=0.3)
    print("waiting up to 120s for '#Flash Download Start' - do the entry sequence now", flush=True)
    _dl, _buf, _seen = time.time() + 120, b"", False
    while time.time() < _dl:
        _d = _w.read(512)
        if _d:
            _buf += _d
            if b"Flash Download Start" in _buf:
                _seen = True; break
    _w.close()
    if not _seen:
        die("download banner never appeared - entry sequence did not take")
    print("banner seen; starting flash", flush=True)
    time.sleep(0.3)

rtl = Patient(PORT)
rtltool.rtl = rtl
p = rtl._port
p.write(rtltool.CAN * 2); time.sleep(0.3); p.reset_input_buffer()
if not rtl.Floader(115200): die("loader handshake failed")
print("loader ready", flush=True)

# flash status register: clear write protection, as rtltool wf does. Not fatal
# if the loader declines - a protected sector makes the erase fail loudly below.
if not rtl.SetFlashStatus(0, 0):
    print("warning: could not clear flash status register; continuing", flush=True)

for off, name, data in plan:
    sectors = (len(data) + SECTOR - 1) // SECTOR
    print(f"\n{name}: erase {sectors} sectors @0x{off:06X} ...", flush=True)
    if not rtl.EraseSectorsFlash(off, len(data)): die(f"erase failed for {name}", 2)
    print(f"{name}: write {len(data)} B ...", flush=True)
    t0 = time.time()
    if not rtl.WriteBlockFlash(io.BytesIO(data), 0x08000000 | off, len(data)):
        die(f"write failed for {name}", 2)
    print(f"{name}: written in {time.time()-t0:.1f}s", flush=True)

print("\nverifying by read-back ...", flush=True)
allok = True
for off, name, data in plan:
    buf = io.BytesIO()
    if not rtl.ReadBlockFlash(buf, off, len(data)): die(f"read-back failed for {name}", 3)
    got = buf.getvalue()[:len(data)]
    same = got == data
    allok &= same
    print(f"  {name:<22} {'MATCH' if same else 'MISMATCH'}")
    if not same:
        for i, (a, b) in enumerate(zip(got, data)):
            if a != b:
                print(f"    first diff at +0x{i:X}: flash {a:02X} vs image {b:02X}"); break
print("\nRESULT:", "all images verified" if allok else "VERIFY FAILED")
sys.exit(0 if allok else 3)
