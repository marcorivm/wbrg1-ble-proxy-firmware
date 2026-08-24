#!/usr/bin/env python3
"""3c test: connect via the ESPHome API, read/write/subscribe on the ESP32
test peripheral (RD=42, WR=44, NT=46) and print everything that comes back.
  usage: esphome_gatt_rw_test.py [host] [mac-hex] [addrtype]
"""
import socket, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.175"
ADDR = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x582abd7d3152
ATYPE = int(sys.argv[3]) if len(sys.argv) > 3 else 0
RD, WR, NT = 42, 44, 46


def vi(v):
    o = b""
    while True:
        b = v & 0x7F; v >>= 7; o += bytes([b | (0x80 if v else 0)])
        if not v: return o

def fr(t, p=b""): return b"\x00" + vi(len(p)) + vi(t) + p
def pu(f, v): return bytes([(f << 3) | 0]) + vi(v)
def pb(f, d): return bytes([(f << 3) | 2]) + vi(len(d)) + d

def rv(b, i):
    v = 0; sh = 0
    while True:
        x = b[i]; i += 1; v |= (x & 0x7F) << sh
        if not x & 0x80: return v, i
        sh += 7

def fields(pl):
    i = 0; out = []
    while i < len(pl):
        tag, i = rv(pl, i); f = tag >> 3; w = tag & 7
        if w == 0: v, i = rv(pl, i); out.append((f, v))
        elif w == 2: l, i = rv(pl, i); out.append((f, pl[i:i+l])); i += l
        elif w == 5: i += 4
        elif w == 1: i += 8
        else: break
    return out

buf = bytearray()
def recv_frames(s, secs, stop_type=None):
    end = time.time() + secs; got = []
    while time.time() < end:
        try: d = s.recv(4096)
        except socket.timeout: d = b""
        if d: buf.extend(d)
        i = 0
        while i < len(buf):
            if buf[i] != 0: return got
            try:
                j = i + 1; ln, j = rv(buf, j); ty, j = rv(buf, j)
            except IndexError: break
            if j + ln > len(buf): break
            got.append((ty, bytes(buf[j:j+ln]))); i = j + ln
        del buf[:i]
        if stop_type is not None and any(t == stop_type for t, _ in got):
            break
    return got

s = socket.create_connection((HOST, 6053), timeout=6); s.settimeout(0.5)
s.sendall(fr(1)); recv_frames(s, 1)

print(f"connect {ADDR:012x} ..."); s.sendall(fr(68, pu(1, ADDR) + pu(2, 4) + pu(4, ATYPE)))
got = recv_frames(s, 10, stop_type=69)
d = dict(fields([pl for t, pl in got if t == 69][-1]))
print(f"  connected={d.get(2)} mtu={d.get(3)} err={d.get(4)}")
assert d.get(2), "not connected"

print("discover services ..."); s.sendall(fr(70, pu(1, ADDR)))
recv_frames(s, 12, stop_type=72); print("  done")

print(f"READ handle {RD} ..."); s.sendall(fr(73, pu(1, ADDR) + pu(2, RD)))
got = recv_frames(s, 6, stop_type=74)
for t, pl in got:
    if t == 74:
        f = dict(fields(pl)); print(f"  ReadResponse handle={f.get(2)} data={f.get(3)!r}")
    if t == 82:
        f = dict(fields(pl)); print(f"  !! GATTError handle={f.get(2)} err={f.get(3)}")

payload = b"hola-wbrg1"
print(f"WRITE handle {WR} <- {payload!r} (with response) ...")
s.sendall(fr(75, pu(1, ADDR) + pu(2, WR) + pu(3, 1) + pb(4, payload)))
got = recv_frames(s, 6, stop_type=83)
for t, pl in got:
    if t == 83: print(f"  WriteResponse handle={dict(fields(pl)).get(2)}")
    if t == 82: print(f"  !! GATTError {dict(fields(pl))}")

print(f"NOTIFY-enable handle {NT} ...")
s.sendall(fr(78, pu(1, ADDR) + pu(2, NT) + pu(3, 1)))
got = recv_frames(s, 6, stop_type=84)
for t, pl in got:
    if t == 84: print(f"  NotifyResponse handle={dict(fields(pl)).get(2)}")
    if t == 82: print(f"  !! GATTError {dict(fields(pl))}")

print("collecting notifications for 6 s ...")
got = recv_frames(s, 6)
ntf = [pl for t, pl in got if t == 79]
for pl in ntf[:5]:
    f = dict(fields(pl))
    print(f"  notify handle={f.get(2)} data={f.get(3).hex() if isinstance(f.get(3), bytes) else f.get(3)}")
print(f"  total notifications: {len(ntf)}")

print("NOTIFY-disable + disconnect ...")
s.sendall(fr(78, pu(1, ADDR) + pu(2, NT) + pu(3, 0)))
recv_frames(s, 3, stop_type=84)
s.sendall(fr(68, pu(1, ADDR) + pu(2, 1)))
recv_frames(s, 3, stop_type=69)
s.close()
print("OK" if ntf else "NO NOTIFICATIONS (check peripheral)")
