#!/usr/bin/env python3
"""Generator for the legacy card images used by tests/test_aux_legacy.cpp.

usage: legacy_image.py v1|rawhdr <out file>

v1     "PSXRAW" version 1 multi disc card: 5 block header at LBA 0, then per disc a 5 block "PSTTRK1" map
       (raw data is not written; the loaders never read it). Three discs, default disc 1; disc 2 has an SBI block.
rawhdr 512 byte sector 0 of the older raw card header (track count at +0xC, 10 byte entries at +0xD).
"""
import struct
import sys
import zlib


def crc(b):
    return zlib.crc32(bytes(b)) & 0xFFFFFFFF


def track_map(tracks, flags=0, sbi=None):
    """tracks: list of (start, index1, length, type, kind, flag_bits)."""
    b = bytearray(0xA00)
    b[0:8] = b"PSTTRK1\0"
    b[8] = len(tracks)
    b[9] = 0x10
    struct.pack_into("<H", b, 0xA, flags)
    for i, (start, idx1, length, typ, kind, bits) in enumerate(tracks):
        struct.pack_into("<IIIBBBB", b, 0x10 + 0x10 * i, start, idx1, length, typ, kind, bits, 0)
    if sbi is not None:
        data = b"SBI\0" + sbi
        struct.pack_into("<H", b, 0x640, len(data))
        struct.pack_into("<I", b, 0x644, crc(data))
        b[0x648:0x648 + len(data)] = data
    struct.pack_into("<I", b, 0xC, crc(b[:0x930]))  # field is zero while the CRC is computed
    return bytes(b)


DISCS = [
    # (serial, title, sectors, tracks, sbi)
    ("SLUS-00001", "FIRST DISC", 20, [(0, 0, 20, 2, 0, 0)], None),
    ("SLUS-00002", "SECOND DISC", 18, [(0, 0, 12, 2, 1, 0), (12, 15, 6, 0, 2, 1)], None),
    ("SLUS-00003", "THIRD DISC", 12, [(0, 0, 12, 1, 0, 0)], bytes(range(1, 21))),
]


def v1_image():
    n = len(DISCS)
    hdr = bytearray(0xA00)
    hdr[0:6] = b"PSXRAW"
    hdr[6] = 1
    hdr[7] = n
    hdr[8] = 1  # default disc
    struct.pack_into("<H", hdr, 10, 0x930)
    base = 5
    maps = []
    for i, (serial, title, sectors, tracks, sbi) in enumerate(DISCS):
        r = bytearray(0x100)
        r[0:4] = b"DISC"
        r[4] = 1
        r[5] = i
        r[6] = 0xFF
        r[7] = i
        r[0xC:0xC + len(serial)] = serial.encode()
        r[0x1C:0x1C + len(title)] = title.encode()
        struct.pack_into("<I", r, 0x3C, base)
        struct.pack_into("<I", r, 0x44, base)
        struct.pack_into("<I", r, 0x4C, base + 5)
        struct.pack_into("<I", r, 0x54, sectors * 5)
        struct.pack_into("<I", r, 0x5C, sectors)
        r[0x60] = len(tracks)
        struct.pack_into("<H", r, 0x62, 0xFFFF)
        struct.pack_into("<I", r, 0xFC, crc(r[:0xFC]))
        hdr[0x130 + 0x100 * i:0x130 + 0x100 * (i + 1)] = r
        maps.append((base, track_map(tracks, 1 if sbi else 0, sbi)))
        base = base + 5 + sectors * 5
    struct.pack_into("<I", hdr, 0x10, crc(hdr[:0x930][:0x10] + b"\0\0\0\0" + hdr[:0x930][0x14:]))
    img = bytearray((maps[-1][0] + 5) * 512)
    img[:0xA00] = hdr
    for b, m in maps:
        img[b * 512:b * 512 + 0xA00] = m
    return bytes(img)


def rawhdr():
    s = bytearray(512)
    s[0:6] = b"PSXRAW"
    entries = [(2, 0, 2000, 0), (1, 2000, 300, 150), (1, 2300, 200, 0)]  # type, start, length, pregap
    s[0xC] = len(entries)
    for i, (typ, start, length, pre) in enumerate(entries):
        struct.pack_into("<BIIB", s, 0xD + 10 * i, typ, start, length, pre)
    return bytes(s)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("v1", "rawhdr"):
        sys.exit(__doc__)
    open(sys.argv[2], "wb").write(v1_image() if sys.argv[1] == "v1" else rawhdr())
