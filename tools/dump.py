#!/usr/bin/env python3
"""Read flash via the resident AmebaD flashloader, using rtltool's protocol
class but with a patient handshake (its 0.2 s per-byte timeout gives up while
the loader is busy reading a sector). Read-only.

  usage: dump.py <size> <outfile> [addr]
"""
import os, sys, time, struct
import rtltool

PORT = os.environ.get('WBRG1_UART', '/dev/cu.usbmodem11302')
size = int(sys.argv[1], 0); out = sys.argv[2]
addr = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0

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

rtl = Patient(PORT)
rtltool.rtl = rtl                      # Floader() references this global
p = rtl._port
p.write(rtltool.CAN * 2); time.sleep(0.3); p.reset_input_buffer()   # abort any half-done transfer

if not rtl.Floader(115200):
    print("loader handshake failed"); sys.exit(1)
print("loader ready; reading 0x%X bytes from 0x%X" % (size, addr), flush=True)
t0 = time.time()
with open(out, 'wb') as f:
    ok = rtl.ReadBlockFlash(f, addr, size)
print("result:", "OK" if ok else "FAILED", "in %.1fs" % (time.time() - t0))
sys.exit(0 if ok else 2)
