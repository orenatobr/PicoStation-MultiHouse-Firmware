# Reverse engineering status

Subject: `PicoStation_MH_V3.uf2` (RP2040, family `0xe48bff56`, 265,216 byte image at `0x10000000`,
built Feb 5 2026 with pico-sdk 2.1.1, SHA-256 of the UF2
`ff90964299be3cd036751601655c53f68acf3d6d90bca292a7bcfb586d9146df`). The original is not
distributed here.

## What the original is

* An older or sibling build of the C++ PicoStation code base (megavolt85/PicoStation lineage) that was
  binary patched: linker veneers inside upstream functions jump to a Multi House layer placed after
  the normal code (`docs/re/deltas.md`).
* The Multi House layer (about 16 KB of machine code) implements: the V2 SD card layout (header A/B,
  Case and Disc tables, track maps), a "virtual sector" protocol the PS1 menu uses (library listing,
  art, configuration, music, cheat blobs v4 and v6, disc switch session), legacy PSXRAW v1 cards,
  a Si5351 clock table, an event trace and a built-in fallback menu (PS-X EXE, 29 raw sectors).

## What this repository contains

| Area | Files | Evidence |
|---|---|---|
| Upstream derived core adjusted to V3 | `src/*.cpp`, `include/*.h` | `docs/re/deltas.md` |
| V2 card layer | `src/mh/card*.cpp` | `docs/re/card-core.md`, `docs/re/CARD_LAYOUT_CROSSCHECK.md` |
| PS1 command handler, cheats, session | `src/mh/handler*.cpp` | `docs/re/handler.md` |
| Config, music, legacy, fallback, clock, trace | `src/mh/aux_*.cpp` | `docs/re/aux.md`, `docs/re/function-map.md` |
| Wiring into the firmware | `src/mh/hooks_*.cpp`, `include/mh/hooks*.h` | `docs/re/integration.md` |
| Built-in menu image | `binary/picostation-menu.bin` | copied bytes, see NOTICE |
| Menu sector overlay (new in 0.3.0, not in V3) | `src/mh/menu_overlay*.cpp`, `tools/gen_menu_overlay.py` | `docs/MEMCARD_FAVORITES.md` |

## How faithful it is (read this before flashing)

* **Behavior, not bytes.** The Multi House layer was rewritten as readable C++ from the Ghidra
  decompile and the card layout documentation. Accept and reject decisions, constants, response
  formats and CRCs follow the original and are covered by 4,515 host checks (ASan and UBSan) that run
  generated card images through the code (`make -C tests`).
* **Machine code is not identical.** Mnemonic similarity per function is high for the upstream
  derived core (many functions 1.00) and low for the Multi House layer (0.15 to 0.50 for most
  functions). Whole image coverage numbers are in `docs/re/integration.md`.
* **Not reproduced:** the exact RAM addresses, the patched call chain order of the original (the hook
  order is simplified), the trace flags, one reset path (see risks), the USB stdio descriptors
  (the original links TinyUSB but never starts it, so the rebuilt image is about 75 KB smaller).
* **Never run on hardware.** Nothing here was tested on a console, an RP2040 or a real SD card.
  The Pico can always be reflashed through BOOTSEL.

## Deliberate deviations from the original

* **Long names extension** (`docs/LONG_NAMES.md`): Case and Disc names up to 71 characters, stored in reserved bytes
  (Case +0x3F, Disc +0x94..+0xFB). The original refuses such records (it requires those bytes to be zero), so a card
  that uses the extension cannot be mounted by it. Cards without the extension list byte for byte as before.

## Known open risks

See `docs/re/integration.md` section 4. The highest one: after a PS1 reset on a V2 card the raw base
returns to its initial value and the original's remount path was not located.

## Method

`re/` has the scripts: Ghidra export and seeding, the literal pool resolver `show.py`, the mnemonic
similarity ledger `ledger.py`, `fuzzy.py` and `coverage.py`. Decompile output is not committed.
