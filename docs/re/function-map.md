# Function map of the V3 specific range

Classification of every function listed by `re/ghidra/out-mhv3/functions.tsv` between flash
`0x10021274` and `0x10030190` (plus the raw header reader `0x10018ff2` in the RAM-copied block), for
all work packages. Addresses are flash VAs, names are Ghidra's `FUN_<address>`. Ghidra seeds a
function at every `bl` target, so a few entries are fragments or merges of larger code (marked); the
`fuzzy.tsv` names ("strcpy", "abort", ...) in that range are accidental and mean nothing.

Owners: **A** card core (mount, validators, track map, listing), **B** command handler (PS1
commands, cheat blobs, session machine), **C** this package (config/music, legacy loaders, fallback
menu, trace, Si5351), **upstream-library** (libgcc, matches the toolchain). "shared leaf" are tiny
copies of one helper (each region has its own copy of memcpy, memcmp, zero test, CRC-32), rewrite
them as leaf functions. "glue" is pico-only wiring that is not host testable.

The sector request handlers form a chain; each one answers its LBA window and otherwise calls the next
one through a literal pointer: FUN_100245e0 (LBA 300..555) -> FUN_100228c8; FUN_100300b8 (built-in menu)
-> FUN_1002d02c (0xFA) -> FUN_1002f118 (0x41A) -> FUN_1002e100 (v6 cheats). The head of the chain and the
other links are inside code owned by A/B (the Ghidra decompile hides the jump tables).

| Address | Size | Role | Owner |
|---|---|---|---|
| `10018ff2` | 300 | older raw card header reader (inside upstream DiscImage::load, RAM copy FUN_20000ce6) | C |
| `10021280` | 748 | mount: reads slot LBA 0 and 5, picks A/B, fills state block; also single disc v1 branch | A |
| `10021624` | 22 | command hook thunk: runs FUN_10024588 (music flag) then tail-calls RAM 0x200000c8 (upstream command handler) | C (hook glue) |
| `100216d8` | 14 | zero fill (memset 0) | A (shared leaf) |
| `100216e6` | 18 | byte copy (memcpy) | A (shared leaf) |
| `100216f8` | 28 | byte compare, 1 = equal | A (shared leaf) |
| `10021714` | 24 | all-zero test | A (shared leaf) |
| `1002172c` | 22 | load u32 | A (shared leaf) |
| `10021742` | 4 | store u32 | A (shared leaf) |
| `10021748` | 40 | CRC-32 update (no init, no final xor) | A (shared leaf) |
| `10021774` | 18 | CRC-32 (init and final xor 0xFFFFFFFF) | A (shared leaf) |
| `10021788` | 30 | busy-wait backoff delay for retries | A |
| `100217b0` | 34 | invalidates the sector cache (state[0x4c] = -1, 32 words = -2) | A |
| `100217e0` | 76 | default disc = menu: DiscImage table with 901 sector data track, raw base = header menu_lba | A |
| `10021858` | 54 | posts the PLS1 status event record (0x28 bytes) and invalidates the cache | A |
| `1002189c` | 14 | status event with kind 8 | A |
| `100218ac` | 32 | text buffer: append char (cap 0x2fff) | A |
| `100218d4` | 30 | text buffer: append C string | A |
| `100218f4` | 44 | text buffer: append hex digits | A |
| `10021924` | 58 | copies a name, non printable bytes become blanks, at most 31 (15 for serials) | A |
| `10021960` | 268 | Case record validator | A |
| `10021a70` | 32 | response builder: 4 byte prefix (0, kind, u16 value) at offset in the response buffer | A |
| `10021aa4` | 270 | V2 header validator | A |
| `10021c14` | 52 | read n blocks into the block buffer, 3 tries with backoff | A |
| `10021c50` | 134 | n-th used Case | A |
| `10021ce4` | 104 | read a byte range (up to 5 blocks per transfer) | A |
| `10021d50` | 48 | Case by index | A |
| `10021d84` | 556 | Disc record validator | A |
| `10021fbc` | 74 | Disc of (current Case, slot) | A |
| `1002200c` | 48 | read a 5 block slot, tail 0x930..0xA00 must be zero, up to 16 tries | A |
| `10022040` | 64 | CRC-32 over the block buffer with a 4 byte window read as zero (track map CRC forms) | A |
| `10022084` | 458 | track map "PSTTRK1" validator and loader | A |
| `10022284` | 74 | listing helper: scans a string, rejects empty or longer than 255 characters | A |
| `100222e0` | 526 | library listing (Case names, "NAME - DISCO n") | A |
| `10022518` | 96 | builds the "PGM1" kind 3 answer for a disc request (falls through to FUN_10029000 when the disc is valid) | B |
| `100227c0` | 12 | thunk: byte copy returning its first argument | B (helper) |
| `100227cc` | 218 | PS1 command word decoder: stores the argument and the selected state (cases 0..0xE), handles the 0x24 byte blob upload (cmd 6/7) | B |
| `100228c8` | 2174 | PS1 command/sector handler: case art, disc art, version 4 cheat blobs, disc switch (case 7) | B |
| `100229f8` | 64 | tail fragment of FUN_100228c8 (Ghidra split): sends the answer, case 900 switch acknowledge | B |
| `10022bf4` | 8 | tail fragment of FUN_100228c8 (Ghidra split): copy + FUN_100229f8 | B |
| `100233f4` | 18 | libgcc __gnu_thumb1_case_uqi (switch table helper) | upstream-library (libgcc) |
| `10023408` | 266 | libgcc __udivsi3 / __aeabi_uidiv (thumb1 shift-subtract divide) | upstream-library (libgcc) |
| `10023514` | 6 | libgcc __aeabi_uidiv entry (zero divisor check) | upstream-library (libgcc) |
| `1002351c` | 2 | libgcc __aeabi_idiv0 (empty) | upstream-library (libgcc) |
| `10024000` | 60 | trace status word: packs flags for the BT01 trace | C |
| `10024044` | 50 | BT01 trace init (clears 0x20040000 and 0x20041000 areas, magic last), then chains to FUN_10014ca0 | C |
| `10024090` | 96 | BT01 trace wrapper around FUN_10021624 (timer and flags before and after) | C |
| `10024188` | 108 | BT01 irq trace wrapper around RAM 0x20000484 (16 slot table) | C |
| `10024400` | 18 | byte copy (memcpy) | C (shared leaf) |
| `10024412` | 4 | load u32 | C (shared leaf) |
| `10024416` | 4 | store u32 | C (shared leaf) |
| `1002441a` | 48 | CRC-32 (init and final xor 0xFFFFFFFF) | C (shared leaf) |
| `10024450` | 196 | PSTCFG2 loader (writes the "CFG1" answer) | C |
| `10024564` | 24 | state reset: runs FUN_10024044 and clears the 0x18 byte config state at 0x20033800 | C |
| `10024588` | 62 | F-class command word hook: sets or clears the music flag, then runs FUN_100227cc | C |
| `100245e0` | 496 | config/music sector server (request LBA 300..555), falls through to FUN_100228c8 | C |
| `10025000` | 70 | I2C0 poll helper: register, mask, wanted value, error code, 2000 us timeout, aborts on TX_ABRT_SOURCE | C |
| `1002505c` | 20 | I2C0: push one IC_DATA_CMD word and wait for TX_EMPTY | C |
| `10025074` | 26 | I2C0: wait for STOP_DET | C |
| `10025094` | 98 | Si5351: poll register 0 until (value & mask) == 0, 100000 us timeout | C |
| `10025108` | 364 | Si5351 boot: I2C0 bring-up on GPIO 12/13, writes the 49 entry register table, "PSI1" diagnostic block | C |
| `100252dc` | 12 | wrapper: FUN_10025108 then tail-calls RAM 0x20002148 (upstream init continuation) | C (glue) |
| `10027000` | 18 | byte copy (memcpy) | C (shared leaf) |
| `10027012` | 4 | load u32 | C (shared leaf) |
| `10027016` | 28 | byte compare, 1 = equal | C (shared leaf) |
| `10027032` | 24 | all-zero test | C (shared leaf) |
| `1002704a` | 72 | CRC-32 with a window read as zero (header and record CRC of the v1 layout) | C (shared leaf) |
| `10027098` | 16 | store u32 little endian bytes | C (shared leaf) |
| `100270a8` | 670 | legacy track map "PSTTRK1" validator + disc table builder | C |
| `100273a8` | 12 | thunk: memcpy returning its first argument | C (glue) |
| `100273c4` | 724 | mount chain: V2 mount FUN_10021280, else legacy v1 multi disc header ("SW26" state at 0x20033900) | C |
| `1002773c` | 228 | disc rotation tick (v1 list or V2 Case), builds the next disc table | C |
| `1002784c` | 16 | thunk: runs FUN_1002773c then tail-calls RAM 0x20001aa8 (upstream) with r3 = 0x88 | C (glue) |
| `10029020` | 384 | serial parse and "SEL1" selection answer ("PGM1" kind 2 record) | B |
| `1002a004` | 780 | Ghidra merge: thunk (runs FUN_1002773c, tail call FUN_1002f190 with r3 = 0x88) followed by data | C (glue) |
| `1002c006` | 46 | CRC-32 update with a negated seed (v6 blob CRC) | B (helper) |
| `1002c060` | 24 | text append of a C string (v6 handler) | B (helper) |
| `1002c078` | 36 | text append of hex digits (v6 handler) | B (helper) |
| `1002c0a0` | 1654 | PS1 handler with version 6 compact cheat blob | B |
| `1002d000` | 32 | clears the 0xFA debug state when FUN_1002d11c reports done | B |
| `1002d02c` | 178 | debug request 0xFA: answers "DEAD" when armed, else falls through to FUN_1002f118 | B |
| `1002d11c` | 446 | Ghidra fragment: tail call into RAM 0x20003424 | B |
| `1002e000` | 4 | load u32 | B (helper) |
| `1002e004` | 48 | CRC-32 (init and final xor 0xFFFFFFFF) | B (helper) |
| `1002e038` | 34 | text append of one char (cap 0x800) for the second handler | B (helper) |
| `1002e060` | 24 | text append of a C string | B (helper) |
| `1002e078` | 36 | text append of hex digits | B (helper) |
| `1002e0a0` | 64 | clears state 0xB (cheat session) and the cache | B |
| `1002e100` | 1642 | PS1 handler with version 6 compact cheat blob (second copy) | B |
| `1002e8c2` | 20 | thunk: tail call into FUN_100245e0 with a frame argument | B (glue) |
| `1002f000` | 14 | load the u16 at state +0x194 | B (helper) |
| `1002f018` | 40 | true when state is 2 and the phase byte is 2 or 4 | B (helper) |
| `1002f050` | 82 | F9 command hook: arms the 0xC state when the value matches | B |
| `1002f0b0` | 84 | completes the 0xC state (sets the result word) and clears it | B |
| `1002f118` | 102 | request 0x41A handler, falls through to FUN_1002e100 | B |
| `10030000` | 118 | mount chain top: FUN_100273c4, then the built-in fallback menu table and state | C |
| `100300b8` | 138 | fallback menu sector provider (29 embedded sectors, XOR with the scrambler) | C |

Counts: 97 functions. Ranges: A `0x10021280..0x10022517`, B `0x100227c0..0x10023600` and
`0x10029000..0x1002f200`, C `0x10024000..0x10025300`, `0x10027000..0x1002785f`, `0x10030000..0x10030190`,
libgcc `0x100233f4..0x1002351d`.

Notes for the other packages:

* `FUN_10021624` is the command hook: PIO command word -> `FUN_10024090` (trace wrapper) -> `FUN_10021624`
  -> `FUN_10024588` (music flag, package C) -> `FUN_100227cc` (decoder, package B), and a tail call to the
  upstream handler in RAM.
* `FUN_10030000` is the real top of the mount chain: it calls `FUN_100273c4`, which calls the V2 mount
  `FUN_10021280` first and tries the legacy v1 header only when that fails. Package A's `mount()` therefore
  maps to `FUN_10021280` only.
* The tiny leaf helpers repeated in the C regions (`FUN_10024400/10024412/10024416/1002441a`,
  `FUN_10027000/10027012/10027016/10027032/1002704a/10027098`) are what the compiler emitted for the same
  inline helpers in each translation unit.
