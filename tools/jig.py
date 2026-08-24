#!/usr/bin/env python3
"""Drive the Pico test jig (flashtool/pico_jig/code.py).

  jig.py ports              show console/data port names
  jig.py rst | dl | hold | stat | baud <n>

The console port answers "ok ..." to a command; the data port is the raw
bridge to the gateway's LOG UART (use it as the serial port for listen.py,
uart_flash.py, etc. -- see jig_data_port()).
"""
import glob, sys, time, serial

def _probe(port):
    try:
        s = serial.Serial(port, 115200, timeout=0.5)
        s.reset_input_buffer()
        s.write(b"stat\n")
        r = s.read(200)
        s.close()
        return b"ok chip_en" in r
    except Exception:
        return False

def jig_ports():
    cands = sorted(glob.glob("/dev/cu.usbmodem*"))
    con = next((p for p in cands if _probe(p)), None)
    data = next((p for p in cands if p != con), None) if con else None
    return con, data

def jig_data_port():
    return jig_ports()[1]

def cmd(line):
    con, _ = jig_ports()
    if not con:
        raise SystemExit("jig console port not found")
    s = serial.Serial(con, 115200, timeout=1)
    s.reset_input_buffer()
    s.write((line + "\n").encode())
    time.sleep(0.6 if line.startswith(("dl", "rst")) else 0.1)
    r = s.read(300).decode("latin1", "replace").strip()
    s.close()
    return r

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] == "ports":
        print("console=%s data=%s" % jig_ports())
    else:
        print(cmd(" ".join(sys.argv[1:])))
