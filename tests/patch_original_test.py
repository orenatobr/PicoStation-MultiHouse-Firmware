#!/usr/bin/env python3
"""Checks of tools/patch_original.py that need no copy of the original image (python3 -I tests/patch_original_test.py)."""
import importlib.util, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("po", ROOT / "tools" / "patch_original.py")
po = importlib.util.module_from_spec(spec)
spec.loader.exec_module(po)

# BL encoder against instructions of the original: bl 0x10022284 at 0x1002236e is fff789ff
assert po.bl(0x1002236E, 0x10022284) == bytes.fromhex("fff789ff")
assert po.bl(0x1002199E + 0x20, 0x10021714) == po.bl(0x100219BE, 0x10021714)

# UF2 round trip
flat = bytearray(bytes((i * 7) & 0xFF for i in range(1000)))
assert bytes(po.uf2_to_flat(po.flat_to_uf2(flat)))[:1000] == bytes(flat)

# apply() on an image that carries the expected original bytes at the four sites
img = bytearray(b"\xff" * 265216)
sites = {0x1002199E: "0d213f30", 0x10021F0C: "fff702fc", 0x1002236E: "fff789ff", 0x10022412: "39ab1a78002a"}
for a, h in sites.items():
    img[a - po.BASE:a - po.BASE + len(h) // 2] = bytes.fromhex(h)
cave, syms = po.embedded_cave()
out = po.apply(bytearray(img), cave, syms)
assert len(out) == po.CAVE_ADDR - po.BASE + len(cave)
assert bytes(out[0x1002199E - po.BASE:][:4]) == bytes.fromhex("0c214030")
assert bytes(out[0x10021F0C - po.BASE:][:4]) == bytes.fromhex("0120c046")
assert bytes(out[0x1002236E - po.BASE:][:4]) == po.bl(0x1002236E, syms["case_row_tramp"])
assert bytes(out[0x10022412 - po.BASE:][:6]) == po.bl(0x10022412, syms["disc_title_tramp"]) + bytes.fromhex("002a")
assert bytes(out[po.CAVE_ADDR - po.BASE:]) == cave
assert all(po.CAVE_ADDR <= s < po.CAVE_ADDR + len(cave) for s in syms.values())

# an image that is not the V3 one is refused before anything is changed
bad = bytearray(img)
bad[0x1002236E - po.BASE] ^= 1
try:
    po.apply(bad, cave, syms)
    raise AssertionError("modified image accepted")
except SystemExit:
    pass
print("patch_original_test: ok")
