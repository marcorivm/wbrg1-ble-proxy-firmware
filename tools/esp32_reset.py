#!/usr/bin/env python3
"""Reset the ESP32 test peripheral into run mode (boot 0x13) via DTR/RTS,
then print whatever it says for a couple of seconds."""
import serial, time, sys
p = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-0001"
s = serial.Serial(p, 115200, timeout=1)
s.setDTR(False); s.setRTS(True); time.sleep(.15); s.setRTS(False)
time.sleep(2.5)
print(s.read(s.in_waiting or 1).decode("latin1", "replace"))
s.close()
