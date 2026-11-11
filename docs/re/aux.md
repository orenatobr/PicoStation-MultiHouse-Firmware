# Package C (aux): config/music, legacy loaders, fallback menu, trace, Si5351

Sources: `include/mh/aux_*.h`, `src/mh/aux_*.cpp`, tests `tests/test_aux_*.cpp`, generator
`tests/legacy_image.py`, ledger `re/ledger/aux.tsv`, classification of the whole range in
`docs/re/function-map.md`. Function names are Ghidra's `FUN_<flash address>` of `PicoStation_MH_V3.uf2`.
Nothing here was run on a console.

## 1. PSTCFG2 loader and music server (`aux_cfg`)

State (RAM 0x20033800, 6 words, cleared by FUN_10024564): `music_enabled`, `loaded`, `music_lba`,
`music_size`, `music_crc`, `generation`. The card side data it reads from the 0x20033400 block: `cfg_lba`
(+0x2c), `art_size` (+0x30, 0x2800, the upper bound of both windows), `generation` (+0xc), state magic
`0x39313334`, phase `state[0] == 1`.

**Enable flag, FUN_10024588.** Called for every command word. When `(word & 0xF00000) == 0xF00000`,
`music_enabled = (state[0] == 1 && word == 0x00F80008)`; any other F-class word (also 0xF9xxxx etc.) clears
the flag. Then it continues into FUN_100227cc (B).

**Request server, FUN_100245e0** (signature: dst, buf, lba, count, answer buffer). Served only when all of:
pointers non null, `count == 1`, `lba >= 0x96`, state magic, `state[0] == 1`, and
`music_enabled ? (lba - 300) < 0x100 : lba == 300`. Everything else falls through to FUN_100228c8. The
answer buffer (0x914 bytes at 0x20034000) is zeroed first.

* Flag clear (config window, only LBA 300): reload PSTCFG2 on every request. Failure -> the buffer holds the
  4 ASCII bytes `DEAD` and zeros. Success -> the "CFG1" answer.
* Flag set (LBA 300 + n): if `loaded && generation == header generation` the cached values are used, else
  the block is reloaded (failure -> answer all zero). The loader's CFG1 answer is discarded (buffer zeroed
  again). The music is served only if `music_lba`, `music_size`, `music_crc` are all non zero,
  `music_lba < art_size` and `size <= (art_size - lba) * 0x200` (the CRC value is never compared).
  Otherwise the answer stays all zero.

**CFG1 answer** (FUN_10024450): `"CFG1"` at 0, u16 text length at +4, text at +0x114 (no NUL, the rest is 0).

**PSTCFG2 block validation** (FUN_10024450): `cfg_lba != 0` and `cfg_lba + 5 <= art_size`; 5 block read; bytes
0..7 = `"PSTCFG2\0"`; `b[8] == 1`, `b[9] == 0`; u32 at +0x20 (music lba high dword) == 0; CRC-32 of 0..0x92B
equals u32 at +0x92C; text at +0x2C must contain a NUL at index 1..0x7FF (index 0 or no NUL in 0x800
bytes rejects). Stored: `music_lba` (+0x1C), `music_size` (+0x24), `music_crc` (+0x28), `generation`. +0xA
and the text contents are opaque (no strings "theme"/"music" exist in the firmware).

**MUS1 answer** (chunk n = `lba - 300`): `"MUS1"` at 0, u16 0x18 at +4, u8 n at +6, 0 at +7, u16 chunk count
at +8, u16 chunk length at +0xA, u32 total size at +0xC, CRC-32 of the chunk data at +0x10, CRC-32 of bytes
0..0x13 at +0x14 (so it covers the data CRC), data at +0x18.

**Chunk arithmetic**: `size < 0x831` -> one chunk of `size` bytes (n must be 0). Else `count = ((size - 0x31) >> 11) + 1`,
must be `<= 0x100` and `> n`, so `size <= 0x80030` (524336). Chunk 0 is 0x830 bytes at offset 0, chunk n > 0 is
`min(0x800, size - off)` at `off = n * 0x800 + 0x30`. The file is read from block `music_lba + (off >> 9)`,
`(len + (off & 0x1FF) + 0x1FF) >> 9` blocks (at most 5, into the 0xA00 buffer) and copied from `off & 0x1FF`.
The embedded 478880 byte Umbrella music gives 234 chunks, the last at offset 0x74830 with 0x670 bytes
(checked in `test_aux_cfg`).

Deviations from the original (documented, external behaviour for valid input is unchanged):
`cfg_lba + 5` is computed in 64 bits (the original wraps for `cfg_lba >= 0xFFFFFFFB`); the 64 bit window test
`size <= (art_size - lba) * 0x200` replaces a 32 bit multiply that cannot overflow for `art_size <= 0x2800`
anyway.

## 2. Legacy loaders (`aux_legacy`)

**Mount chain** (FUN_10030000 -> FUN_100273c4 -> FUN_10021280). FUN_100273c4 zeroes its state (0x274 bytes at
0x20033900, magic `0x36325753`), runs the V2 mount (package A) and copies a 0x38 byte thunk from flash
0x1002a000 to RAM 0x20033100. Mount ok: mode = `state[0] == 0 ? 1 : 2` (1 = single disc v1 handled by
FUN_10021280, 2 = V2 card). Mount failed: legacy v1 header (below); if that fails too the legacy state gets
error 1 and FUN_10030000 installs the fallback menu.

**v1 multi disc header** (one 0xA00 slot at LBA 0, no B copy; FUN_100273c4):

| Offset | Check |
|---|---|
| 0..5 | `PSXRAW` |
| 6 | 1 (version) |
| 7 | disc count n, 2..6 |
| 8 | default disc, `< n` |
| 9 | 0 |
| 0xA | u16 0x930 |
| 0x10 | CRC-32 of 0..0x92F with 0x10..0x13 read as zero |
| 0x14..0x12F | zero |
| 0x130 + 0x100 i | disc record i, i < n |
| 0x130 + 0x100 n .. 0x92F | zero |
| 0x930..0x9FF | zero |

Disc record (0x100 bytes): `"DISC"`, +4 = 1, +5 = i, +6 = 0xFF, +7 = i, u32 +8 = 0, serial 16 bytes at +0xC, title
32 bytes at +0x1C, u32 +0x3C base, +0x44 track map lba, +0x4C raw lba, +0x54 raw blocks, +0x5C sectors, +0x60
track count, u16 +0x62 = 0xFFFF, high dwords +0x40/+0x48/+0x50/+0x58 = 0, +0x64..+0x7B zero, +0x94..+0xFB zero,
CRC-32 of 0..0xFB at +0xFC. Conditions: `base == track == next`, `raw == next + 5`, `blocks == sectors * 5`,
`1 <= sectors <= 450000 (0x6DDD0)`, `1 <= tracks <= 0x62`, `next = raw + blocks` without 32 bit overflow;
`next` starts at 5. Each accepted record is copied to a 100 byte entry (`[0]` i, `[1]` 0xFF, `[2]` i, `[3]` track count,
u16 `[4]` 0xFFFF, `[0xC]` base, `[0x10]` track lba, `[0x14]` raw lba, `[0x18]` raw blocks, `[0x1C]` sectors,
serial at `[0x30]`, title at `[0x40]`). The same 100 byte layout is `mh::card::DiscInfo` of package A.

**Track map and disc table, FUN_100270a8.** Reads 5 blocks at the entry's track lba and requires, in this
order: `"PSTTRK1\0"`, `b[8] == track_count`, `b[9] == 0x10`, u16 flags `< 2` (bit 0 SBI), u32 at +0xC ==
CRC-32 over 0..0x92F with +0xC..+0xF read as zero (only this form, unlike FUN_10022084), 0x930..0x9FF zero;
per entry (16 bytes at 0x10 + 16 i: start, index1, length, type, kind, flag bits, 0): `length != 0`, `start ==
running end`, no 32 bit overflow, `end <= sectors`, `index1 >= running`, `index1 < end`, `type <= 2`, `kind <=
0xF`, `flag bits <= 7`, byte 0xF == 0; the first track must be data (`type != 0`); the final end equals
`sectors`; the entries from n up to 99 (0x10 + n*16 .. 0x640) are zero. With the SBI flag: u16 length at +0x640 in
4..0x2E8, magic `"SBI\0"` at +0x648, u32 at +0x644 equals the CRC-32 of the `length` bytes starting at +0x648
(the magic included); the SBI data is only validated, never used. `track_count` is limited to 1..0x62 here
(V2 accepts 1..99).

Table built (identical tail in FUN_10030000 and FUN_100217e0): the in-RAM DiscImage object (`mh::card::CardDisc`):
u32 track count at 0; lead-in record 0 (index 1 at record +0x14 = 0x122A); record i (1-based, at 4 + 0x34 i): size
(+4) = length, file offset (+8) = start, index count (+0xC) = 1, index 0 (+0x10) = start + 0x122A, index 1 (+0x14) =
index1 + 0x122A, type (+0x1C) = 2 for data and 1 for audio; lead-out record n + 1: file offset = total sectors,
index 0 and 1 = total + 0x122A. Tail: byte 0x1470 = 1, u32 0x1474 = 0, byte 0x147A = 1. Globals: raw base
(0x200087f0) = entry raw lba, sector cursor (0x20009a68) = 0, `total_sectors` (0x200088c0) = sectors + 0x2454,
then the sector cache is invalidated (FUN_100217b0, package A).

**Disc rotation, FUN_1002773c** (hooked through the thunks FUN_1002784c and FUN_1002a004). Counts calls in the
legacy state. Mode 1: needs at least 2 discs, next = (current + 1) mod n, rebuild the table from that entry.
Mode 2 (only when the V2 state is 3): the current entry's Case (`[1]`, 0xFF = none) is fetched with FUN_10021d50; it
needs 2..5 discs, `slot < disc_count` and `disc_ids[slot] == entry id`; next slot (wrapping) is fetched with
FUN_10021d84 and must have the same Case index and slot. Result code in the legacy state: 0 ok, 2 Case lookup
failed, 3 disc lookup failed, 4 track map failed. On success mode 2 also copies the entry to the current disc
(0x20033524), stores slot and id, and sets state 3. The mode 1 position and counters are updated by the caller.

**Older raw header, FUN_10018ff2** (RAM copy FUN_20000ce6, in `DiscImage::load`). Sector 0: `"PSXRAW"` (the check
reads 4 + 2 bytes), track count at +0xC, entries of 10 bytes at +0xD: type (1 = 1, 2 = 2 + `hasData`, other =
0), u32 start, u32 length, u8 pregap. Fills the table: record i: size = length, file offset = start, index 0 = pos -
pregap, index 1 = pos with `pos` starting at 0x122A and growing by each length; index count is not written. Lead-out:
file offset = pos - 0x122A, index 0/1 = pos; `total_sectors` = pos + 0x122A; byte 0x147A = 1. (With 0 tracks this gives
0x2454.) Deviation: the sector holds at most 49 entries; more are rejected (the original would read past the
512 byte buffer).

## 3. Built-in fallback menu (`aux_fallback`)

FUN_10030000: `r = FUN_100273c4(table)`; when it fails and `table != null`, the table is the fallback one
(`fallback_table`): one data track of 0x5D (93) sectors, start 0, total 0x5D, `total_sectors` = 0x24B1, raw base 0.
State block stores: `state[0] = 1`, magic `0x39313334`, `+0x24` (raw base) = 0, `+0x28` (menu_sectors) = 0.
A V2 card always has `menu_sectors >= 1`, so `menu_sectors == 0` selects the embedded menu.

FUN_100300b8 (sector request handler; falls through to FUN_1002d02c otherwise): serves when state magic ok,
`state[0] == 1`, `menu_sectors == 0`, pointers non null, `count == 1`, `lba >= 0x96` and `lba - 0x96 < 0x5D`.
Sector index `k = lba - 0x96`; `k < 0x1D` reads sector k of the embedded image (flash 0x10030190, 29 x 0x930 =
68208 bytes, identical to `binary/picostation-menu.bin`), `k >= 0x1D` reads zeros. For each 16 bit input word
`w = (image ^ scrambler) << 8`; if bit 8 of `w` is set, `w |= 0xFF`; stored as one 32 bit word (0x1260 bytes out).

The text "SD CARD MISSING OR INVALID" (flash 0x100400a8, together with "MULTI HOUSE V3") is inside sector 27
of the embedded PS1 menu program (file offset 0xFF18) and is drawn by the PS1 side. No Pico code references it.

## 4. Event trace "BT01" (`aux_trace`)

FUN_10024044 clears two RAM areas (0x20040000..0x20040800 and 0x20041000..0x20041800), sets header
`{magic "BT01" (written last), version 1, boot timer}` at 0x20041000. FUN_10024090 wraps FUN_10021624 (the
command hook): event n < 128 goes to the array at 0x20040000, later ones cycle through 64 entries at 0x20041200;
an entry is `{timer before, counter (word at 0x2000cb88) after, status before, status after}`, header words
+0xC/+0x10 count started/finished events, +0x20 is non zero while a call runs. FUN_10024188 wraps the RAM function
0x20000484 the same way into a 16 entry table at 0x20041040 (24 byte entries, with UART1 flags 0x40038018 and
a GPIO control register 0x40014024), counts in +0x14/+0x18, overflow in +0x1C. FUN_10024000 packs the status word.
Uncertain: the second word of an irq entry is an unnamed register (r11) at entry; the meaning of the globals
0x200093ec, 0x20008820 and of the wrapped functions is not known beyond the above. Purely diagnostic.

## 5. Si5351 boot sequence (`aux_clock`)

V3 does not call upstream's computed `si5351_Init/SetupCLK1/SetupCLK2/EnableOutputs`; FUN_10025108 programs the
chip from a fixed 49 entry (register, value) table at flash 0x100252ec (`kClockTable`), recording progress in a
"PSI1" block at 0x20033840: reset I2C0 and wait for reset done (2000 us), controller set to master, fast mode
(CON 0x165, TAR 0x60, FS counts 0x110/0x196), GPIO 12/13 function I2C with pull-ups, enable, then wait until
register 0 bit 7 (SYS_INIT) clears (100000 us), write the table, with a second wait for `reg0 & 0xE8 == 0` (PLL
lock) before the last entry (`0x03 = 0xF9`, CLK1 and CLK2 enabled). The block also stores the clock targets
53203425 and 53693175 Hz, 400000 baud and 271200000 Hz (VCO) as constants; they are not used to compute
anything. Status 1 running, 2 done, 3 failed; error codes 2..8 name the failing poll (2 enable status, 3 TX
abort, 4 TX empty, 5 RX count, 6 stop detect, 7 SYS_INIT wait, 8 lock wait). `clock_program` models the register
part with an abstract bus; the I2C0/pad bring-up is pico-only and is not rebuilt (it needs `pico` headers and the
exact resets/pad writes listed above). `clock_program` maps every bus write failure to code 4 (the original has
separate codes). Uncertain: the table was not decoded into frequencies.

## 6. Tests

`make -C tests` builds `test_aux_cfg` (loader accept/reject incl. every checked field, chunk arithmetic and the
0x100 chunk limit, request routing, cached versus reloaded config, zero answers), `test_aux_legacy` (images from
`tests/legacy_image.py`: v1 header and per record rejects, track map rejects incl. SBI, table contents, rotation
modes 1 and 2, older raw header, fallback table/decision/conversion) and `test_aux_misc` (Si5351 sequence with a
fake bus, trace ring behaviour). The generator needs `python3` on the path (the test runs it).
