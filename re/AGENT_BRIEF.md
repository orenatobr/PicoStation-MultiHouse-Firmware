# Reverse engineering brief (shared by all work packages)

Goal: rebuild, as readable C++ that compiles into the firmware, the code of
`PicoStation_MH_V3.uf2` that is NOT in the upstream PicoStation code base (megavolt85 c48754d,
already vendored in `src/`, `include/`, `pio/`). Behaviour must match the original on the wire
(what the PS1 menu sees) and on the card (which records are accepted or rejected).

## Facts

* Image: RP2040, flash 0x10000000, 265216 bytes. crt0 copies flash `0x100183cc..0x10021274` to RAM
  `0x200000c0..0x20008f68` (functions in this range have two copies: use the RAM address). A fixed
  RAM state block lives at 0x20033400 (0x1b0 bytes), a 0xA00 byte block buffer at 0x2000a39c, a
  response sector buffer at 0x20034000. Those RAM addresses are internal: do NOT reproduce them,
  only the layout and behaviour.
* V3 specific code is mostly flash 0x10021274..0x10030190 (about 16 KB, 42 real functions) plus
  small edits inside upstream functions. Everything else matches upstream (mnemonic similarity
  about 1.00 for DiscImage, EDC/ECC, FatFs, SD driver, unwind, libgcc).
* The V2 card layout and the validators are documented in `docs/re/CARD_LAYOUT_CROSSCHECK.md`
  (authoritative for field offsets, verified against the Manager). Function names there are
  `FUN_<flash address>`.

## Files (read only, never commit them)

* `re/mh_v3.bin`            flat original image (base 0x10000000)
* `re/ghidra/out-mhv3/all.c`  decompile of 1042 functions; `functions.tsv`; `strings.tsv`
* `python3 -I re/show.py re/ghidra/out-mhv3/all.c re/mh_v3.bin <addr> [<addr>...]` prints functions
  with every `DAT_xxxxxxxx` literal replaced by the real constant. Addresses are lower case hex
  without 0x (`10021280`, `20000fa0`).
* `re/dis.sh <addr> [len]` disassembles the original (flash or RAM addresses).
* Ghidra output is imperfect: jump tables and `switch8_r0` are mangled, stack variables show up as
  `unaff_*`/`in_stack_*`, inlined atomics (`isIRQinterruptsEnabled`, `DataMemoryBarrier`) are
  noise from pico_sync/pseudo atomics. Confirm anything odd with `dis.sh`.
* Ghidra names functions `FUN_<addr>`; two helpers of the same shape (CRC, zero test, compare) are
  tiny leaf functions, rewrite them as such so the machine code can match.

## Build and measure

```sh
export TOOLCHAIN_BIN=$HOME/.cache/picostation-fw/tc/bin
export PICO_SDK_PATH=$HOME/.cache/picostation-fw/pico-sdk
PY=$HOME/.cache/picostation-fw/venv/bin/python        # has capstone + pyelftools
B=$HOME/.cache/picostation-fw/work-build              # configured full build (headers exist)
$PY tools/objbuild.py $B src/mh/yourfile.cpp /tmp/yourpkg/yourfile.o     # one object, same flags
$PY re/ledger.py /tmp/yourpkg/yourfile.o re/mh_v3.bin re/ledger/yourpkg.tsv      # similarity table
$PY re/ledger.py /tmp/yourpkg/yourfile.o re/mh_v3.bin re/ledger/yourpkg.tsv --show <symbol>  # diff
```
Ledger line: `<mangled symbol>\t<v3 address hex>\t[v3 size]\t[note]`. Use `extern "C"`-free C++ and read
mangled names with `arm-none-eabi-nm`. Similarity is the difflib ratio of instruction mnemonics:
aim for >= 0.90 per function, but correctness and readability come first; iterate on the C++
shape (loop form, early returns, types, inlining with `static inline` / `[[gnu::noinline]]`)
until the compiler output follows the original control flow. Report honestly what stays below.
Do NOT edit `CMakeLists.txt`; every `src/mh/*.cpp` and `src/mh/*.c` is compiled automatically,
`include/mh/*.h` is on the include path. Do not run the full firmware link unless your package
is complete (another package may be mid-edit); when needed use a private build dir, e.g.
`BUILD_DIR=$HOME/.cache/picostation-fw/build-<pkg> tools/build.sh`.

Host tests (no hardware): `tests/Makefile` builds every `tests/test_*.cpp` against `src/mh/*.cpp`
with ASan/UBSan (`make -C tests`). Keep logic that can run on the host free of pico headers (use
`include/mh/platform.h`). Write tests that feed card images (built by a generator you write, per the
layout doc) and check accept/reject decisions and produced bytes.

## Rules

* Ownership: touch only the files listed for your package. Never run `git commit`, `git add`,
  `git checkout`, `git stash`, `git reset`, never touch `third_party/`, never write into other
  repositories. Other packages edit the same checkout at the same time.
* EN-US everywhere. Never use the em dash character (U+2014) in code, comments or docs.
* Comments: say what the code does and which original function it reproduces
  (`// V3 FUN_10021960`); no narration of your process. Match the surrounding C++ style.
* Behaviour first: reproduce every accept/reject decision, constant and byte of output of the
  original. Where the original reads or writes outside a buffer (it validates little), keep the
  external behaviour for valid input but add the bounds checks needed so malformed card data
  cannot corrupt memory (the card is untrusted input); note each such deviation in your doc.
* Evidence: write findings (constants, struct layouts, state machine, anything surprising) to
  `docs/re/<package>.md`, in English, with function addresses.
* Finish with a report: files written, per function ledger similarity, what is not done or
  uncertain, and test results. Do not claim hardware verification, nothing here was run on a
  console.
