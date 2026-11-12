#!/usr/bin/env python3
"""Run the real machine code of a (patched) V3 image in Unicorn and compare its library listing with the rebuild.

usage: verify_patch_emu.py FLAT_IMAGE.bin      (needs: pip install unicorn; a C++ compiler; tests/card_image.py)

FLAT_IMAGE is the flash contents from 0x10000000 (tools/patch_original.py output converted with its uf2_to_flat, or the
original V3 image). Mounts generated V2 SD images, sets the selection and runs FUN_100222e0, then compares the listed
names and the trailer with card.cpp running on the host. Flags are not compared: the original uses 0 for Disc rows.
"""
import struct, subprocess, sys, tempfile
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC

ROOT = Path(__file__).resolve().parent.parent


def listing(flash, img, sel_case, list_offset):
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    mu.mem_map(0x10000000, 0x400000)
    mu.mem_write(0x10000000, bytes(flash))
    mu.mem_map(0x20000000, 0x42000)
    mu.mem_write(0x200000C0, bytes(flash[0x183CC:0x183CC + 0x8EA8]))   # what crt0 copies to RAM
    mu.mem_map(0x40000000, 0x1000000)
    mu.mem_map(0xD0000000, 0x1000)
    mu.mem_map(0, 0x1000)

    def hook(uc, addr, size, ud):
        if addr == 0x20003188:                                   # SD init
            uc.reg_write(UC_ARM_REG_R0, 0)
        elif addr == 0x20003414:                                 # SD read(lba, blocks, dst)
            lba, blocks, dst = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R0 + 1, UC_ARM_REG_R0 + 2))
            if (lba + blocks) * 512 > len(img):
                uc.reg_write(UC_ARM_REG_R0, 1)
            else:
                uc.mem_write(dst, img[lba * 512:(lba + blocks) * 512])
                uc.reg_write(UC_ARM_REG_R0, 0)
        else:
            return
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    mu.hook_add(UC_HOOK_CODE, hook, begin=0x20003000, end=0x20003500)

    def call(fn, *args):
        for i, a in enumerate(args):
            mu.reg_write(UC_ARM_REG_R0 + i, a)
        mu.reg_write(UC_ARM_REG_SP, 0x20041000)
        mu.reg_write(UC_ARM_REG_LR, 0x801)
        mu.emu_start(fn | 1, 0x800, count=20_000_000)

    mu.mem_write(0x2003A000, b"\0" * 0x1500)
    call(0x10021280, 0x2003A000)                                 # mount (FUN_10021280)
    if struct.unpack("<I", mu.mem_read(0x20033400, 4))[0] != 1:
        raise SystemExit("mount failed")
    mu.mem_write(0x20033400 + 0x50, struct.pack("<I", sel_case))
    mu.mem_write(0x20033400 + 0x48, struct.pack("<I", list_offset))
    call(0x100222E0)                                             # library listing (FUN_100222e0)
    return bytes(mu.mem_read(0x20034000, 0x914))


def rows(b):
    p, out = 0, []
    while p < len(b) and b[p] != 0:
        out.append(b[p + 2:p + 2 + b[p]].decode("latin1"))
        p += 2 + b[p]
    return out, b[p:p + 4].hex()


def main():
    flash = Path(sys.argv[1]).read_bytes()
    tmp = Path(tempfile.mkdtemp())
    dump = tmp / "dump_listing"
    subprocess.run(["c++", "-std=c++20", "-O1", "-I" + str(ROOT / "include"), "-I" + str(ROOT),
                    str(ROOT / "tools/dev/dump_listing.cpp"), str(ROOT / "src/mh/card.cpp"), str(ROOT / "src/mh/card_util.cpp"),
                    str(ROOT / "src/mh/card_io.cpp"), "-o", str(dump)], check=True)
    bad = checks = 0
    for kind in ("basic", "long", "manylong"):
        img_path = tmp / (kind + ".img")
        subprocess.run([sys.executable, "-I", "card_image.py", str(img_path), kind], check=True, cwd=ROOT / "tests")
        img = img_path.read_bytes()
        for sel in (0x100, 0, 1, 2, 3, 4):
            for off in (0, 1, 2):
                host = bytes.fromhex(subprocess.run([str(dump), str(img_path), str(sel), str(off)], check=True,
                                                    capture_output=True, text=True).stdout.strip())
                checks += 1
                if rows(listing(flash, img, sel, off)) != rows(host):
                    bad += 1
                    print("MISMATCH", kind, hex(sel), off)
    print("%d listings compared, %d mismatches" % (checks, bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
