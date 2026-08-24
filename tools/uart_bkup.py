#!/usr/bin/env python3
"""Read/clear the AmebaD backup register BKUP_REG0 through the ROM flashloader.

The flashloader (and the ROM's trap-pin path) leave BIT_UARTBURN_BOOT (bit 9) /
BIT_UARTBURN_DEBUG (bit 10) set in BKUP_REG0, which makes every CHIP_EN reset
come straight back to UART download mode. Clearing them lets `jig.py rst` boot.

  usage: uart_bkup.py <data-port> [--clear]        (module in download mode)
BKUP_REG0 = 0x480003C0 (from KM0 ROM BKUP_Read @0x1434 literal). Run from flashtool/.
"""
import struct, sys, time
import rtltool

BKUP0 = 0x480003C0
CMD_WWA, CMD_RWA, ACK = b'\x29', b'\x31', b'\x06'


class Patient(rtltool.RTLXMD):
    def WaitResp(self, code=rtltool.ACK):
        self._port.timeout = 3.0
        for _ in range(4000):
            c = self._port.read(1)
            if not c:
                return None
            if c == code:
                return True
        return False


def rd(rtl, addr):
    r = rtl.ReadRegs(addr, 4)
    return struct.unpack('<I', bytes(r))[0] if r else None


def wr(rtl, addr, val):
    p = rtl._port
    p.reset_input_buffer()
    p.write(CMD_WWA + struct.pack('<II', addr, val))
    p.timeout = 2.0
    return p.read(1) == ACK


port = sys.argv[1]
rtl = Patient(port)
rtltool.rtl = rtl
p = rtl._port
if not rtl.ReadRegs(0x00082000, 4):        # loader not resident -> upload it
    p.write(rtltool.CAN * 2); time.sleep(0.3); p.reset_input_buffer()
    if not rtl.Floader(115200):
        sys.exit("loader handshake failed — is the module in ROM download mode?")
v = rd(rtl, BKUP0)
print(f"BKUP_REG0 = 0x{v:08X}  UARTBURN_BOOT={int(bool(v & 0x200))} UARTBURN_DEBUG={int(bool(v & 0x400))}")
if '--clear' in sys.argv:
    ok = wr(rtl, BKUP0, v & ~0x600)
    v2 = rd(rtl, BKUP0)
    print(f"write {'ACK' if ok else 'NO-ACK'}; BKUP_REG0 now 0x{v2:08X}")
