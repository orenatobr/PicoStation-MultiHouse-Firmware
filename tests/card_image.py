#!/usr/bin/env python3
"""Builds V2 (and legacy v1) PicoStation Multi House card images for the host tests.

Layout: docs/re/CARD_LAYOUT_CROSSCHECK.md. Blocks are 512 bytes, slots 5 blocks (0xA00).
Usage: card_image.py <out.img> [basic|legacy|empty|many|long]
Long names extension (docs/LONG_NAMES.md): build_v2(cases, long_names=True) splits names over 31 bytes.
"""
import struct
import sys
import zlib

BLK = 512
SLOT = 0xA00
USED = 0x930
HDR_A, HDR_B, CASE_LBA, DISC_LBA, ART_LBA, MENU_LBA, CFG_LBA, ART_SIZE = 0, 5, 10, 0x32, 0xB4, 0x14B4, 0x1685, 0x2800
CASE_REC, DISC_REC = 0x50, 0x100


def crc(b):
    return zlib.crc32(bytes(b)) & 0xFFFFFFFF


def u32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


def u64(v):
    return struct.pack("<Q", v)


def pad(b, n):
    b = bytes(b)
    assert len(b) <= n, (len(b), n)
    return b + bytes(n - len(b))


def header(cases, discs, gen, menu_sectors, flags=1):
    h = bytearray(SLOT)
    h[0:6] = b"PSXRAW"
    h[6] = 2
    h[7] = flags
    struct.pack_into("<HH", h, 8, cases, discs)
    struct.pack_into("<I", h, 0xC, gen)
    struct.pack_into("<I", h, 0x10, 0x800)
    for off, v in ((0x14, HDR_B), (0x1C, CASE_LBA), (0x24, DISC_LBA), (0x2C, ART_LBA), (0x34, MENU_LBA)):
        struct.pack_into("<Q", h, off, v)
    struct.pack_into("<I", h, 0x3C, menu_sectors)
    struct.pack_into("<Q", h, 0x40, CFG_LBA)
    struct.pack_into("<Q", h, 0x48, ART_SIZE)
    return h


def seal_header(h):
    struct.pack_into("<I", h, 0x92C, crc(h[:0x92C]))
    return h


def case_record(index, name, disc_ids, art=0xFFFF, flag=0):
    r = bytearray(CASE_REC)
    r[0:4] = b"CASE"
    r[4] = 1
    r[5] = len(disc_ids)
    r[6] = index
    r[8:8 + len(name)] = name
    r[0x3F] = flag
    struct.pack_into("<H", r, 0x38, art)
    r[0x3A:0x3A + len(disc_ids)] = bytes(disc_ids)
    struct.pack_into("<I", r, 0x4C, crc(r[:0x4C]))
    return r


def disc_record(did, case, slot, serial, title, base, sectors, tracks, flags=0, art=0xFFFF, title_ext=None, case_ext=None):
    r = bytearray(DISC_REC)
    r[0:4] = b"DISC"
    r[4] = 1
    r[5] = did
    r[6] = case
    r[7] = slot
    struct.pack_into("<I", r, 8, flags)
    r[0xC:0xC + len(serial)] = serial
    r[0x1C:0x1C + len(title)] = title
    struct.pack_into("<Q", r, 0x3C, base)
    struct.pack_into("<Q", r, 0x44, base)
    struct.pack_into("<Q", r, 0x4C, base + 0x100)
    struct.pack_into("<Q", r, 0x54, sectors * 5)
    struct.pack_into("<I", r, 0x5C, sectors)
    r[0x60] = len(tracks)
    r[0x61] = 1
    struct.pack_into("<H", r, 0x62, art)
    struct.pack_into("<I", r, 0x7C, 0xCAFE0000 + did)
    if title_ext is not None or case_ext is not None:  # LNX1 block
        te, ce = title_ext or b"", case_ext or b""
        r[0x94:0x98] = b"LNX1"
        r[0x98], r[0x99] = len(te), len(ce)
        r[0x9A:0x9A + len(te)] = te
        r[0xC2:0xC2 + len(ce)] = ce
    struct.pack_into("<I", r, 0xFC, crc(r[:0xFC]))
    return r


def track_map(tracks, sectors, sbi=0):
    """tracks: list of (start, index1, length, type) with type 0 audio, 1 or 2 data."""
    m = bytearray(SLOT)
    m[0:8] = b"PSTTRK1\0"
    m[8] = len(tracks)
    m[9] = 0x10
    struct.pack_into("<H", m, 0xA, sbi)
    for i, (start, idx1, length, typ) in enumerate(tracks):
        struct.pack_into("<IIIB", m, 0x10 + 16 * i, start, idx1, length, typ)
    struct.pack_into("<I", m, 0xC, crc(m[:USED]))  # field is zero while hashing
    return m


def cfg_slot(text=b"theme=umbrella\n"):
    c = bytearray(SLOT)
    c[0:8] = b"PSTCFG2\0"
    struct.pack_into("<H", c, 8, 1)
    c[0x2C:0x2C + len(text)] = text
    struct.pack_into("<I", c, 0x92C, crc(c[:0x92C]))
    return c


class Image:
    def __init__(self, blocks):
        self.d = bytearray(blocks * BLK)

    def put(self, lba, data, off=0):
        a = lba * BLK + off
        self.d[a:a + len(data)] = data


def pattern(tag, n):
    return bytes((tag * 7 + i) & 0xFF for i in range(n))


BASIC_CASES = [
    # name, discs: (serial, title, sectors, tracks)
    (b"Alundra 2", [(b"SLUS-01234", b"Alundra 2", 3, [(0, 0, 3, 2)])]),
    (b"Chrono Cross", [(b"SLUS-01041", b"Chrono Cross (Disc 1)", 5, [(0, 0, 3, 2), (3, 3, 2, 0)]),
                       (b"SLUS-01042", b"Chrono Cross (Disc 2)", 4, [(0, 1, 4, 1)])]),
    (b"", [(b"", b"", 2, [(0, 0, 2, 2)])]),
    (b"A" * 31 + b"B", [(b"X" * 15 + b"Y", b"T" * 32, 2, [(0, 0, 2, 2)])]),
    (b"Caf\xc3\xa9 \xff\x80!\x7f~", [(b"SLES-\x01\x02", b"\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0 ok", 2, [(0, 0, 2, 2)])]),
]


T1 = [(0, 0, 1, 2)]
# Long names fixtures (tests/test_card.cpp mirrors the lengths): case index -> (name, discs)
LONGS = [
    (b"N" * 31 + b"(" + b"x" * 38 + b")", [(b"SLUS-00001", b"D" * 31 + b"(" + b"y" * 38 + b")", 1, T1)]),  # 71 and 71
    (b"L" * 40, [(b"SLUS-00002", b"E" * 32, 1, T1), (b"SLUS-00003", b"F" * 71, 1, T1)]),
    (b"Case Two", [(b"SLUS-00004", b"G" * 61, 1, T1), (b"SLUS-00005", b"H" * 62, 1, T1)]),
    (b"Four", [(b"SLUS-00006", b"T" * 40, 1, T1)]),
    (b"Metal Gear Solid - VR Missions (Europe) (En,Fr,De,Es,It) (Rev 1)",
     [(b"SLUS-00007", b"Resident Evil 2 - Dual Shock Ver. (USA) (Disc 1) (Greatest Hits)", 1, T1)]),
]


def build_v2(cases=BASIC_CASES, gen_a=1, gen_b=1, menu_sectors=2, long_names=False):
    ndisc = sum(len(d) for _, d in cases)
    img = Image(ART_SIZE + 0x100 * (ndisc + 1) + 0x400)
    ca = bytearray(256 * CASE_REC)
    dt = bytearray(256 * DISC_REC)
    did = 0
    pkgs = []
    for ci, (name, discs) in enumerate(cases):
        ids = []
        flag = 0
        if long_names and len(name) > 31:
            assert len(name) <= 71
            name, case_ext, flag = name[:31], name[31:], 1
        else:
            case_ext = None
        for slot, (serial, title, sectors, tracks) in enumerate(discs):
            base = ART_SIZE + 0x100 * did
            title_ext = None
            if long_names and len(title) > 31:
                assert len(title) <= 71
                title, title_ext = title[:31], title[31:]
            dt[did * DISC_REC:(did + 1) * DISC_REC] = disc_record(
                did, ci, slot, serial, title, base, sectors, tracks, title_ext=title_ext,
                case_ext=case_ext if slot == 0 else None)
            img.put(base, track_map(tracks, sectors))
            for s in range(sectors):
                img.put(base + 0x100 + 5 * s, pattern(did * 16 + s, 0x930))
            pkgs.append(base)
            ids.append(did)
            did += 1
        ca[ci * CASE_REC:(ci + 1) * CASE_REC] = case_record(ci, name, ids, art=ci, flag=flag)
    img.put(CASE_LBA, ca)
    img.put(DISC_LBA, dt)
    for ci in range(len(cases)):
        img.put(ART_LBA + ci * 0x14, pattern(0x40 + ci, 0x2800))
    for s in range(menu_sectors):
        img.put(MENU_LBA + 5 * s, pattern(0x80 + s, 0x930))
    img.put(CFG_LBA, cfg_slot())
    ha = seal_header(header(len(cases), ndisc, gen_a, menu_sectors))
    hb = seal_header(header(len(cases), ndisc, gen_b, menu_sectors))
    img.put(HDR_A, ha)
    img.put(HDR_B, hb)
    return img.d


def build_legacy():
    sectors, base = 4, 0x40
    img = Image(0x200)
    h = bytearray(SLOT)
    h[0:6] = b"PSXRAW"
    h[6], h[7] = 1, 1
    struct.pack_into("<H", h, 0xA, 0x930)
    rec = bytearray(DISC_REC)
    rec[0:4] = b"DISC"
    rec[4], rec[5], rec[6], rec[7] = 1, 0, 0xFF, 0
    struct.pack_into("<I", rec, 0x3C, base)
    struct.pack_into("<I", rec, 0x44, base)
    struct.pack_into("<I", rec, 0x4C, base + 5)
    struct.pack_into("<I", rec, 0x54, sectors * 5)
    struct.pack_into("<I", rec, 0x5C, sectors)
    rec[0x60] = 1
    struct.pack_into("<I", rec, 0xFC, crc(rec[:0xFC]))
    h[0x130:0x230] = rec
    hashed = bytearray(h[:USED])
    hashed[0x10:0x14] = bytes(4)
    struct.pack_into("<I", h, 0x10, crc(hashed))
    img.put(0, h)
    img.put(base, track_map([(0, 0, sectors, 2)], sectors))
    for s in range(sectors):
        img.put(base + 5 + 5 * s, pattern(s + 1, 0x930))
    return img.d


if __name__ == "__main__":
    out, kind = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else "basic")
    many = [(b"Game %03d " % i + b"X" * 22, [(b"SLUS-%05d" % i, b"Title %03d" % i, 1, [(0, 0, 1, 2)])]) for i in range(80)]
    longs = LONGS
    manylong = [((b"Long Case %02d " % i).ljust(71, b"z"), [(b"SLUS-%05d" % i, b"Title %02d" % i, 1, [(0, 0, 1, 2)])])
                for i in range(40)]
    data = {"basic": build_v2, "legacy": build_legacy, "empty": lambda: build_v2([], menu_sectors=1),
            "many": lambda: build_v2(many), "menu93": lambda: build_v2(menu_sectors=93), "long": lambda: build_v2(longs, long_names=True),
            "manylong": lambda: build_v2(manylong, long_names=True)}[kind]()
    open(out, "wb").write(data)
