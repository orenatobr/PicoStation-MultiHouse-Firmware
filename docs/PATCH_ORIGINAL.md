# Long names on the original V3 firmware

The rebuilt firmware (releases v0.1.0 to v0.3.1) did not boot on the console it was tried on, so long names are also
offered as a **patch of your own copy of the original `PicoStation_MH_V3.uf2`**. The boot path stays the original one:
only four places of the library listing code change and 508 bytes of new code are appended after the image.

## Use

```
curl -fsSL -o patch_original.py https://github.com/orenatobr/PicoStation-MultiHouse-Firmware/releases/latest/download/patch_original.py
python3 patch_original.py PicoStation_MH_V3.uf2 PicoStation_MH_V3_longnames.uf2
```

Then flash `PicoStation_MH_V3_longnames.uf2` through BOOTSEL like any UF2. To go back, flash the original again. The
tool refuses any file that is not the unmodified V3 image (it hashes the 265,216 flash bytes) and checks the expected
original bytes at every patched site.

## What changes

| Where (original address) | Original | Patched |
|---|---|---|
| `0x1002199e` Case validator, FUN_10021960 | bytes `+0x3F..+0x4B` must be zero | only `+0x40..+0x4B` must be zero (`+0x3F` is the long names flag) |
| `0x10021f0c` Disc validator, FUN_10021d84 | bytes `+0x94..+0xFB` must be zero | not required (the record CRC still is) |
| `0x1002236e` Case row of the library listing | `bl FUN_10022284` | `bl case_row_tramp` (cave): base name plus `case_ext` of the Disc record `ids[0]` |
| `0x10022412` Disc row title | pointer to the 32 character title | `bl disc_title_tramp` (cave): title plus `title_ext` |

The new code is `patch/cave.c`, linked at `0x10041000` (free flash after the image) and embedded in the tool. It reads
the Disc record with the original SD read function (`0x20003414`, three tries), checks magic `DISC`, the id, the CRC-32 of
the first `0xFC` bytes, the `LNX1` magic, the lengths (at most 40) and that the extension characters are printable,
and falls back to the 31/32 character name on any failure. It uses 80 bytes of RAM at `0x2003C000`, which nothing in the
original references. The layout of the extension is `docs/LONG_NAMES.md`.

## How it was checked

`tools/verify_patch_emu.py` runs the **real machine code** of the patched image in Unicorn: it mounts generated V2 SD
images (`tests/card_image.py`: `basic`, `long`, `manylong`), runs the library listing (FUN_100222e0) for the whole
library and for each Case, with page offsets 0 to 2, and compares the listed names and the trailer with `card.cpp` running
on the host. Patched: 54 of 54 listings equal. The unpatched original: 33 of 54 differ, because it drops the records that
use the extension. `tests/patch_original_test.py` (in CI) checks the tool itself, and CI rebuilds the cave and fails if the
embedded copy is stale.

Not checked: a console. Names are longer than the original ever showed, the menu code on the PS1 stores up to 255
characters per row (`docs/LONG_NAMES.md`), and the original uses flag 0 for Disc rows where the rebuild uses 1 (the
patch keeps the original's value).
