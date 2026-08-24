#!/usr/bin/env python3
"""Sanity-check the 8 MB WBRG1 backup before anything is written to the module.

- size must be exactly 8 MiB
- first 64 KB must match the earlier independent test read (read-path consistency)
- map the Realtek image headers and see how much of the chip is blank
- record sha256 next to the image
"""
import hashlib, re, sys, os

BK = os.environ.get('WBRG1_BACKUP',
    os.path.expanduser('~/Projects/personal/wbrg1-tuya-backup/wbrg1-tuya-full-8mb.bin'))
T64 = os.environ.get('WBRG1_PROBE',
    os.path.join(os.path.dirname(__file__), 'test64k.bin'))

d = open(BK, 'rb').read()
print(f"size: {len(d)} bytes ({'OK' if len(d) == 0x800000 else 'WRONG - expected 8388608'})")

t = open(T64, 'rb').read()
same = d[:len(t)] == t
print(f"first 64 KB vs independent test read: {'MATCH' if same else 'MISMATCH'}")
if not same:
    for i, (a, b) in enumerate(zip(d, t)):
        if a != b: print(f"  first diff at 0x{i:X}: dump {a:02X} vs test {b:02X}"); break

print("\nimage headers:")
for sig, name in ((b'\x99\x99\x96\x96\x3f\xcc\x66\xfc', 'boot image (99999696)'),
                  (b'8711', 'image2 header (..8711)')):
    hits = [m.start() for m in re.finditer(re.escape(sig), d)]
    for h in hits[:12]:
        off = h - 4 if name.startswith('image2') else h
        print(f"  {name:<24} @ 0x{off:06X}")
    if len(hits) > 12: print(f"  ... {len(hits)-12} more")

print("\nblank (0xFF) ratio per 512 KB:")
step = 0x80000
for off in range(0, len(d), step):
    blk = d[off:off+step]
    ff = blk.count(b'\xff') * 100 // len(blk)
    bar = '#' * ((100 - ff) // 5)
    print(f"  0x{off:06X}-0x{off+step-1:06X}  used {100-ff:3d}%  {bar}")

last = len(d)
while last > 0 and d[last-1] == 0xFF: last -= 1
print(f"\nlast non-0xFF byte at 0x{last-1:06X}")

h = hashlib.sha256(d).hexdigest()
open(BK + '.sha256', 'w').write(f"{h}  {os.path.basename(BK)}\n")
print(f"sha256: {h}")
print("written:", BK + '.sha256')
sys.exit(0 if (len(d) == 0x800000 and same) else 1)
