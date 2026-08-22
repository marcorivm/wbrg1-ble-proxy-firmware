#!/usr/bin/env python3
"""Erase flash sectors through the ROM flashloader (module must already be in
download mode and polling NAK). Used e.g. to kill the OTA2 signature so the
bootloader falls back to OTA1:
    python3 uart_erase.py <port> 0x106000 0x1000
Run from flashtool/ (rtltool.py + imgtool_flashloader_amebad.bin live here).
"""
import io, sys, time
import rtltool


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


port = sys.argv[1]
off = int(sys.argv[2], 0)
size = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x1000

rtl = Patient(port)
rtltool.rtl = rtl
p = rtl._port
p.write(rtltool.CAN * 2)
time.sleep(0.3)
p.reset_input_buffer()
if not rtl.Floader(115200):
    sys.exit("loader handshake failed")
print("loader ready", flush=True)
if not rtl.SetFlashStatus(0, 0):
    sys.exit("SetFlashStatus failed")
print(f"erasing 0x{off:06X}..0x{off+size:06X}", flush=True)
if not rtl.EraseSectorsFlash(off, size):
    sys.exit("erase failed")
buf = io.BytesIO()
if not rtl.ReadBlockFlash(buf, off, 64):
    sys.exit("readback failed")
rb = buf.getvalue()
print("readback:", rb[:16].hex(" "), "->", "ERASED OK" if rb == b"\xff" * len(rb) else "NOT blank!")
rtl.RestoreBaud()
