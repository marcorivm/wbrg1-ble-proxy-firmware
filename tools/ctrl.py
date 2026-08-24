#!/usr/bin/env python3
"""Send a command to the WBRG1 control socket (:6054) — the reliable out-of-band
channel that replaces the MQTT command topic.
  usage: ctrl.py "<command>" [host] [port]
  e.g.:  ctrl.py "reboot"
         ctrl.py "conn 582abd7d3152 0"
         ctrl.py "ota 192.168.0.148 8000 /wbrg1_ota.bin"
"""
import socket, sys

cmd = sys.argv[1] if len(sys.argv) > 1 else "stat"
host = sys.argv[2] if len(sys.argv) > 2 else "192.168.0.175"
port = int(sys.argv[3]) if len(sys.argv) > 3 else 6054

s = socket.create_connection((host, port), timeout=6)
s.settimeout(3)
try:
    print(s.recv(128).decode("latin1", "replace").strip())   # greeting
except socket.timeout:
    pass
s.sendall((cmd + "\n").encode())
try:
    print("reply:", s.recv(128).decode("latin1", "replace").strip())
except socket.timeout:
    print("(no reply)")
s.close()
