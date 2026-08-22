#!/usr/bin/env python3
"""Wrap an Arduino AmebaD km0_km4_image2.bin into the SDK 'OTA_All' container
that http_update_ota() / the OTA parser expects.

Format (from rtl8721d_ota.c / rtl8721d_ota.h):
  update_file_hdr (8B):  u32 FwVer; u32 HdrNum;
  update_file_img_hdr (24B): u8 ImgId[4]="OTA\\0"; u32 ImgHdrLen=24;
      u32 Checksum; u32 ImgLen; u32 Offset; u32 FlashAddr;
  then the image data (== the whole km0_km4_image2.bin; its first 8 bytes are
  the signature the device captures, the rest is the body).
  Checksum = sum of all image-data bytes (u32 wrap); the device recomputes
  sum(body)+sum(signature[8]) which equals sum(all bytes).

  usage: make_ota.py <km0_km4_image2.bin> <out OTA_All.bin> [fwver_hex]
"""
import struct, sys

src, out = sys.argv[1], sys.argv[2]
fwver = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0xFFFFFFFF
img = open(src, "rb").read()
img_len = len(img)
checksum = sum(img) & 0xFFFFFFFF
HDR_LEN = 24            # sizeof(update_file_img_hdr)
offset = 8 + HDR_LEN    # data starts right after the 32-byte header

file_hdr = struct.pack("<II", fwver, 1)              # FwVer, HdrNum=1
img_hdr = (b"OTA\x00" +
           struct.pack("<IIIII", HDR_LEN, checksum, img_len, offset, 0))
blob = file_hdr + img_hdr + img
open(out, "wb").write(blob)
print(f"wrote {out}: {len(blob)} bytes")
print(f"  FwVer=0x{fwver:08X} HdrNum=1 ImgId='OTA' ImgHdrLen={HDR_LEN}")
print(f"  ImgLen={img_len} Checksum=0x{checksum:08X} Offset={offset}")
print(f"  header(32B)= {blob[:32].hex(' ')}")
