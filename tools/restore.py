#!/usr/bin/env python3
"""Write regions of the 8 MB Tuya backup back to the WBRG1 via the resident
flashloader, with read-back verification.

  usage: restore.py boot     # KM0 boot (0x0000-0x1FFF) + KM4 boot (0x4000-0x5FFF) only
         restore.py app      # image2 region 0x6000-0xD7FFF only
         restore.py all      # everything we ever erased: 0x0-0xD7FFF
"""
import os
import io, sys, time
import rtltool

PORT   = '/dev/cu.usbmodem11302'
# Point WBRG1_BACKUP at the backup of the board actually on the wire: each
# module's dump is that unit's ONLY way back to Tuya and they are NOT
# interchangeable between boards.
BACKUP = os.environ.get('WBRG1_BACKUP',
    os.path.expanduser('~/Projects/personal/wbrg1-tuya-backup/wbrg1-tuya-full-8mb.bin'))
REGIONS = {
    'boot': [(0x000000, 0x2000), (0x004000, 0x2000)],
    'app':  [(0x006000, 0xD2000)],
    'all':  [(0x000000, 0xD8000)],
}

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

what = sys.argv[1] if len(sys.argv) > 1 else ''
if what not in REGIONS: print(__doc__); sys.exit(1)
img = open(BACKUP, 'rb').read()
assert len(img) == 0x800000, "backup has wrong size"

rtl = Patient(PORT); rtltool.rtl = rtl
p = rtl._port
p.write(rtltool.CAN * 2); time.sleep(0.3); p.reset_input_buffer()
if not rtl.Floader(115200): print("ABORT: loader handshake failed"); sys.exit(1)
print("loader ready", flush=True)
if not rtl.SetFlashStatus(0, 0): print("warning: could not clear flash status register")

for off, ln in REGIONS[what]:
    data = img[off:off+ln]
    print(f"\n0x{off:06X}..0x{off+ln:06X}: erase ...", flush=True)
    if not rtl.EraseSectorsFlash(off, ln): print("ABORT: erase failed"); sys.exit(2)
    print(f"0x{off:06X}: write {ln} B ...", flush=True)
    t0 = time.time()
    if not rtl.WriteBlockFlash(io.BytesIO(data), 0x08000000 | off, ln): print("ABORT: write failed"); sys.exit(2)
    print(f"0x{off:06X}: written in {time.time()-t0:.1f}s", flush=True)

print("\nverifying ...", flush=True)
ok = True
for off, ln in REGIONS[what]:
    buf = io.BytesIO()
    if not rtl.ReadBlockFlash(buf, off, ln): print("ABORT: read-back failed"); sys.exit(3)
    same = buf.getvalue()[:ln] == img[off:off+ln]
    ok &= same
    print(f"  0x{off:06X}..0x{off+ln:06X} {'MATCH' if same else 'MISMATCH'}")
print("\nRESULT:", "restored and verified" if ok else "VERIFY FAILED")
sys.exit(0 if ok else 3)
