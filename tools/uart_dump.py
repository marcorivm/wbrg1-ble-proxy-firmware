#!/usr/bin/env python3
"""Back up the WBRG1's full 8 MB flash over UART, via the RTL ROM flashloader.

Run fetch_vendor_tools.sh first (needs rtltool.py + imgtool_flashloader_amebad.bin
in this directory). Put the module in ROM download mode before running (see README:
CHIP_EN + PA7 dance), then:

    python3 uart_dump.py <serial-port> wbrg1-tuya-full-8mb.bin

rtltool's own handshake is too impatient (0.2 s/byte) — the loader goes quiet while
it reads a sector — so this drives rtltool's class with a 3 s wait.
"""
import io, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rtltool  # noqa: E402

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
OUT = sys.argv[2] if len(sys.argv) > 2 else "wbrg1-tuya-full-8mb.bin"
SIZE = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x800000


class Patient(rtltool.RTLXMD):
    def WaitResp(self, code=rtltool.ACK):
        saved = self._port.timeout
        self._port.timeout = 3.0
        try:
            for _ in range(4000):
                c = self._port.read(1)
                if not c:
                    return None
                if c == code:
                    return True
            return False
        finally:
            self._port.timeout = saved


rtl = Patient(PORT)
rtltool.rtl = rtl
p = rtl._port
p.write(rtltool.CAN * 2)
time.sleep(0.3)
p.reset_input_buffer()
if not rtl.Floader(115200):
    print("loader handshake failed — is the module in ROM download mode?")
    sys.exit(1)
print(f"loader ready; reading 0x{SIZE:X} bytes -> {OUT}", flush=True)
t0 = time.time()
with open(OUT, "wb") as f:
    ok = rtl.ReadBlockFlash(f, 0, SIZE)
print("result:", "OK" if ok else "FAILED", f"in {time.time()-t0:.1f}s")
sys.exit(0 if ok else 2)
