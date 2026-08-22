#!/usr/bin/env python3
"""First-time (bootstrap) flash of the WBRG1 over UART, via the RTL ROM flashloader.

Writes the three Arduino build images into their slots and verifies each by
read-back. Needed only ONCE — after this, update over the air (see make_ota.py).
A failed OTA is also recovered with this.

Run fetch_vendor_tools.sh first. Build the sketch so the images exist in the
Arduino AmebaD tools dir (the ..._tools/<ver> folder), then put the module in ROM
download mode (README: CHIP_EN + PA7 dance) and run:

    python3 uart_flash.py <serial-port> <arduino-ameba_d_tools-dir> [--wait]

--wait polls for the ROM banner first so the download dance can be done anytime in
a 120 s window. Layout written (Realtek AmebaD stock):
    0x000000 km0_boot_all.bin
    0x004000 km4_boot_all.bin
    0x006000 km0_km4_image2.bin      (system data 0x2000-0x3FFF untouched)
"""
import io, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rtltool  # noqa: E402

SECTOR = 4096
IMAGES = [
    (0x000000, "km0_boot_all.bin"),
    (0x004000, "km4_boot_all.bin"),
    (0x006000, "km0_km4_image2.bin"),
]


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


def die(msg, code=1):
    print("ABORT:", msg)
    sys.exit(code)


args = [a for a in sys.argv[1:] if not a.startswith("--")]
port = args[0] if len(args) > 0 else "/dev/ttyUSB0"
imgdir = args[1] if len(args) > 1 else "."

plan = []
for off, name in IMAGES:
    path = os.path.join(imgdir, name)
    if not os.path.exists(path):
        die(f"missing {path} — build the sketch first")
    data = open(path, "rb").read()
    if not data:
        die(f"{name} is empty")
    plan.append((off, name, data))
    print(f"  {name:<22} 0x{off:06X}..0x{off+len(data):06X}  {len(data)} B")
for off, name, data in plan:
    if off in (0x000000, 0x004000) and len(data) > 0x2000:
        die(f"{name} would overrun its 8 KB boot slot")

if "--wait" in sys.argv:
    import serial
    w = serial.Serial(port, 115200, timeout=0.3)
    print("waiting up to 120s for '#Flash Download Start' — do the entry dance now",
          flush=True)
    dl, buf, seen = time.time() + 120, b"", False
    while time.time() < dl:
        d = w.read(512)
        if d:
            buf += d
            if b"Flash Download Start" in buf:
                seen = True
                break
    w.close()
    if not seen:
        die("download banner never appeared")
    print("banner seen; flashing", flush=True)
    time.sleep(0.3)

rtl = Patient(port)
rtltool.rtl = rtl
p = rtl._port
p.write(rtltool.CAN * 2)
time.sleep(0.3)
p.reset_input_buffer()
if not rtl.Floader(115200):
    die("loader handshake failed — is the module in ROM download mode?")
print("loader ready", flush=True)
if not rtl.SetFlashStatus(0, 0):
    print("warning: could not clear flash status register; continuing", flush=True)

for off, name, data in plan:
    print(f"\n{name}: erase + write {len(data)} B @0x{off:06X} ...", flush=True)
    if not rtl.EraseSectorsFlash(off, len(data)):
        die(f"erase failed for {name}", 2)
    t0 = time.time()
    if not rtl.WriteBlockFlash(io.BytesIO(data), 0x08000000 | off, len(data)):
        die(f"write failed for {name}", 2)
    print(f"{name}: written in {time.time()-t0:.1f}s", flush=True)

print("\nverifying ...", flush=True)
ok = True
for off, name, data in plan:
    buf = io.BytesIO()
    if not rtl.ReadBlockFlash(buf, off, len(data)):
        die(f"read-back failed for {name}", 3)
    same = buf.getvalue()[: len(data)] == data
    ok &= same
    print(f"  {name:<22} {'MATCH' if same else 'MISMATCH'}")
print("\nRESULT:", "all images verified" if ok else "VERIFY FAILED")
sys.exit(0 if ok else 3)
