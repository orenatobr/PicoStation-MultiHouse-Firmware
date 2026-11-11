<!-- Copied from orenatobr/PicoStation-MultiHouse-macOS docs/FIRMWARE_CROSSCHECK.md. FUN_xxxxxxxx are Ghidra names of PicoStation_MH_V3.uf2 (flash address). -->
# Firmware cross-check of the V2 card layout

Second, independent oracle for `core/include/mh/v2format.h` and `docs/map/v2.md`: the layout the
port writes is compared with what the V3 firmware (`PicoStation_MH_V3.uf2`) actually reads.
The Manager's decompilation says what the Manager writes; the firmware says what the Pico accepts.

## Method

1. `python3 -I tools/uf2_to_bin.py PicoStation_MH_V3.uf2 mh_v3.bin` gives a flat 0x40C00 byte image
   based at 0x10000000 (RP2040 XIP flash, boot2 `boot2_w25q080`, vector table at 0x10000100, built
   "Feb 5 2026", pico-sdk 2.1.1, board `picostation_pico1`).
2. Ghidra headless, language `ARM:LE:32:Cortex`, `BinaryLoader`, base 0x10000000. The loader finds
   only the vector targets, so a post script seeds a function at every `push {..,lr}` and every `bl`
   target (about 920 functions), then decompiles all of them. Literal pool words (`DAT_xxxx`) were
   resolved against the flat image so every constant below was read from the binary, not guessed.
   Work files: `<scratchpad>/fwre/` (`scripts/Seed.java`, `Dec.java`, `show.py`, `out/all2.c`).
3. The reset handler copies flash `0x100183cc..` to RAM `0x200000c0..0x20008f68` (hot SD and PIO
   code, called through RAM pointers such as 0x20003415); addresses below are flash VAs, names are
   Ghidra's `FUN_<flash address>`.
4. `core/tests/test_v2_firmware.c` transcribes the firmware checks (each `fw_*` names its function)
   and runs them on images produced by the V2 writer (`make test-v2`). It also contains a negative
   set (one damaged byte per region must make the oracle refuse the card).

Verdicts: CONFIRMED (the firmware does exactly what the port assumes), REFUTED (they differ),
NOT FOUND (no firmware code looks at it, so it cannot make a card unreadable).

## Firmware functions that matter

| Function | Role |
|---|---|
| FUN_10021280 | mount: reads slot LBA 0 and LBA 5, chooses A/B, fills the state block at 0x20033400. Also contains the legacy "PSXRAW version 1, single disc" branch |
| FUN_10021aa4 | V2 header validator |
| FUN_1002200c | reads a 5 block slot (up to 16 tries) and requires bytes 0x930..0xA00 to be zero |
| FUN_10021960 / FUN_10021c50 / FUN_10021d50 | Case record validator / n-th used Case / Case by index |
| FUN_10021d84 / FUN_10021fbc | Disc record validator / Disc of (Case, slot) |
| FUN_10022084 | track map "PSTTRK1" validator and loader |
| FUN_100222e0 | library listing (Case names, "NAME - DISCO n") |
| FUN_100228c8 | PS1 command handler: case art, disc art, version 4 cheat blobs, disc switch (case 7) |
| FUN_1002c0a0, FUN_1002e100 | same handler with the version 6 (compact) cheat blob |
| FUN_10024450 / FUN_100245e0 | "PSTCFG2" loader / music server |
| FUN_100217e0 | default disc = the menu (raw base = header menu_lba) |
| RAM stub 0x20033000 (flash copy of 0x10021640..0x100216d8) | sector read: `(lba - 0x96) * 5`, then the raw base |
| FUN_100273c4 / FUN_100270a8 | legacy multi disc "PSXRAW version 1" loader (not used by V2 cards) |
| FUN_10030000 / FUN_100300b8 | built-in fallback menu (flash 0x10030190), used when no card layout is valid; text "SD CARD MISSING OR INVALID" at 0x100400a8 |

CRC everywhere (FUN_10021748, FUN_10021774, FUN_1002441a): reflected, polynomial 0xEDB88320,
init 0xFFFFFFFF, final xor 0xFFFFFFFF, i.e. the plain CRC-32 used by `mh_crc32`.
All LBAs are 512 byte blocks. Records are always read through 5 block (0xA00) transfers or byte
ranges inside them. 64 bit LBA fields are read as the low dword; the high dword is checked to be zero.

## Header (slot at LBA 0 = header A, LBA 5 = header B)

| Fact | Port value | Firmware evidence | Verdict |
|---|---|---|---|
| Magic | "PSXRAW" at 0 | FUN_10021aa4: compare 6 bytes with 0x10023525 | CONFIRMED |
| Version | byte 6 = 2 | `buf[6] == 2` | CONFIRMED |
| Flags | byte 7 bit0 required | `(buf[7] << 31) < 0` | CONFIRMED |
| Flags bit1 / hybrid MBR (type 0xDA, 0x55AA, 0x1B8..0x1FF) | written, validated by the port | no read of bit1, of 0x1BE..0x1FF or of 0x55AA anywhere; only the header CRC covers those bytes | NOT FOUND |
| case_count, disc_count | u16 at +8, +0xA, each <= 0x100 | `<= 0x100` both; firmware also requires `(case_count == 0) == (disc_count == 0)` (port `mh_v2_header_valid` does not) | CONFIRMED (firmware slightly stricter) |
| generation | u32 at +0xC | only used by the A/B choice below | CONFIRMED |
| unit | u32 at +0x10 = 0x800 | `== 0x800` | CONFIRMED |
| b_lba | u64 at +0x14 = 5 | `u32 == 5`, high dword (+0x18) `== 0`; slot B is really read at LBA 5 | CONFIRMED |
| case_lba | u64 at +0x1C = 10 | `== 10`, high 0; Case table read at LBA 10 + idx*0x50 | CONFIRMED |
| disc_lba | u64 at +0x24 = 0x32 | `== 0x32`, high 0 | CONFIRMED |
| art_lba | u64 at +0x2C = 0xB4 | `== 0xB4`, high 0 | CONFIRMED |
| menu_lba | u64 at +0x34 = 0x14B4 | `== 0x14B4`, high 0 | CONFIRMED |
| menu_sectors | u32 at +0x3C, 1..93 | `(v - 1) < 0x5D` | CONFIRMED |
| cfg_lba | u64 at +0x40 = 0x1685 | `== 0x1685`, high 0 | CONFIRMED |
| art_size | u64 at +0x48 = 0x2800 | `== 0x2800`, high 0. The firmware reuses this value as "first package LBA" (Disc base >= it) and as the upper bound of the config and music windows | CONFIRMED |
| Header CRC | CRC-32 of 0..0x92B stored at 0x92C | `crc32(buf, 0x92C) == u32(buf + 0x92C)` | CONFIRMED |
| Padding | 0x930..0xA00 zero | FUN_1002200c: `FUN_10021714(buf + 0x930, 0xd0)` (all zero) required, retried up to 16 times | CONFIRMED |
| crc_cases (+0x50), crc_discs (+0x54), crc_art (+0x58), crc_menu (+0x5C), menu SHA-1 (+0x60) | written, verified by the Manager | stored in the state block (crc_cases, crc_discs, crc_menu) and never compared; crc_art and the SHA-1 are not even read | NOT FOUND |
| Header B | identical copy at 0xA00 | read with `FUN_1002200c(5, 0x10)` | CONFIRMED |
| A/B selection | newer generation wins, A on a tie | FUN_10021280: both valid -> if equal keep A; if A < B keep A only when `(int)(B - A) < 0`; if A > B keep A only when `A - B < 0x80000001`; only one valid -> that one. Equivalent to "B iff `(int32)(B - A) > 0`", which is the Manager rule in `v2_load.c`. Tested exhaustively on boundary values in `test_generation_rule` | CONFIRMED |
| Table CRC based choice (Manager) | the loader prefers the header whose table CRCs match | the firmware never reads the tables to choose | NOT FOUND (Manager only, harmless: the writer publishes identical A and B) |
| Both headers invalid | card is "uncommitted" | FUN_10021280 returns 0, FUN_100273c4 (legacy v1) fails on the magic/version, the built-in fallback menu shows the error text | CONFIRMED |

## Case table (0x1400, 256 x 0x50)

| Fact | Port value | Firmware evidence (FUN_10021960) | Verdict |
|---|---|---|---|
| Position | record i at `case_lba*512 + i*0x50`, 32 records per 5 block read | FUN_10021c50 reads groups of 5 blocks (8 groups), FUN_10021d50 reads `idx * 0x50` | CONFIRMED |
| Magic | "CASE" (4 bytes) | compare 4 bytes with 0x10023520 | CONFIRMED |
| used (+4) | 0 = empty | non zero = used; an empty record must be entirely zero (`FUN_10021714(rec, 0x50)`), otherwise the whole listing fails | CONFIRMED (port validator ignores the bytes of an unused record, the writer zero fills them) |
| disc_count (+5) | 1..5 | `(v - 1) < 5` | CONFIRMED |
| index (+6) | equals table position | `rec[6] == idx` | CONFIRMED |
| variant (+7) | opaque | not read | NOT FOUND |
| name (+8, 0x20) | NUL terminated, <= 31 chars | copied by FUN_10021924(.., 0x21, rec + 8, 0x20): at most 31 characters, every byte outside 0x20..0x7E becomes a space (so UTF-8 accents display as blanks) | CONFIRMED (cosmetic caveat, see discrepancies) |
| code (+0x28, 0x10) | Case serial | not read | NOT FOUND |
| art id (+0x38) | 0xFFFF or == index | `(u16)(v + 1) < 0x101`, i.e. 0xFFFF or <= 0xFF; case art is read at `art_lba + art_id * 0x14` (FUN_100228c8) | CONFIRMED (firmware looser) |
| disc ids (+0x3A, 5) | first n distinct, rest 0 | pairwise distinct for the first n, `rec[0x3A + i] == 0` for i >= n | CONFIRMED |
| bytes +0x3F..+0x4B | zero | `FUN_10021714(rec + 0x3f, 0xd)` must be zero | CONFIRMED (the encoder zero fills) |
| CRC (+0x4C) | CRC-32 of 0..0x4B | `crc32(rec, 0x4c) == u32(rec + 0x4c)` | CONFIRMED |
| Count | header case_count == used records | the listing walks used records in order and stops after case_count | CONFIRMED |

## Disc table (0x6400, 256 x 0x100)

All from FUN_10021d84 (record read with `FUN_10021ce4(disc_lba, id << 8, buf, 0x100)`).

| Fact | Port value | Firmware evidence | Verdict |
|---|---|---|---|
| Magic | "DISC" | compare 4 bytes with 0x1002352C | CONFIRMED |
| used (+4), id (+5) | id == position; empty record all zero | `rec[5] == id`; empty => `FUN_10021714(rec, 0x100)` | CONFIRMED |
| case_index (+6), slot (+7) | position inside the Case | FUN_10021fbc: `disc.case_index == case.index` and `disc.slot == slot` | CONFIRMED |
| flags (+8) | bit0 TRACKS_FIXED, bit1 EXT, < 4 | `flags < 4`; bit1 selects 0x19 / 0x2800 / art +0x1E versus 0xF / 0x1400 / art +0x14. Bit0 is not interpreted by any code found | CONFIRMED (bit0 NOT FOUND) |
| serial (+0xC, 0x10), title (+0x1C, 0x20) | text | printable filter, at most 15 and 31 characters (menu text) | CONFIRMED |
| base_lba (+0x3C) | >= 0x2800, multiple of 256 | `base >= header.art_size (0x2800)`, `(base & 0xff) == 0`, high dword (+0x40) zero | CONFIRMED |
| track_lba (+0x44) | == base | `== base`, high (+0x48) zero | CONFIRMED |
| raw_lba (+0x4C) | == base + 0x100 | `== base + 0x100`, high (+0x50) zero | CONFIRMED |
| raw_blocks (+0x54) | == sectors * 5 | `== sectors * 5`, high (+0x58) zero | CONFIRMED |
| sectors (+0x5C) | > 0 | `!= 0` and `<= 0x19999999` (overflow guard of the *5) | CONFIRMED |
| track_count (+0x60) | 1..0x63 | `(v - 1) < 99` and equal to the track map count | CONFIRMED |
| bin_count (+0x61) | informational | not read | NOT FOUND |
| art id (+0x62) | 0xFFFF or == id | `(u16)(v + 1) < 0x101`; disc art read at `base + 0x1E` (EXT) or `base + 0x14` | CONFIRMED (firmware looser) |
| cheat_lba (+0x64) | == base + 5 | 0 (then cheat_size must be 0) or `base + 5`; high dword (+0x68) zero | CONFIRMED |
| cheat_size (+0x6C) | <= 0x2800 (0x1400 without EXT) | `<= 0x2800` when bit1 else `<= 0x1400` | CONFIRMED |
| extra_lba (+0x70), extra_size (+0x78) | base + 0x19 (+0xF), 0xA00 | 0/0 or `base + 0x19` (EXT) / `base + 0xF`, size `<= 0xA00`; the slot is never read | CONFIRMED |
| crc32 (+0x7C), sha1 (+0x80) of the RAW data | written, verified by the Manager | crc32 is copied to the disc state (a token sent to the PS1 side), neither is verified | NOT FOUND |
| bytes +0x94..+0xFB | zero | `FUN_10021714(rec + 0x94, 0x68)` must be zero | CONFIRMED |
| CRC (+0xFC) | CRC-32 of 0..0xFB | `crc32(rec, 0xfc) == u32(rec + 0xfc)` | CONFIRMED |

## Package contents

| Fact | Port value | Firmware evidence | Verdict |
|---|---|---|---|
| Track map at base + 0 | "PSTTRK1\0" (8 bytes), slot 0xA00 | FUN_10022084: `FUN_1002200c(track_lba, 8)` then 8 byte compare with 0x100235D4; padding 0x930..0xA00 must be zero | CONFIRMED |
| Track count byte (+8), entry size (+9) | count, 0x10 | `buf[8] == disc.track_count`, `buf[9] == 0x10` | CONFIRMED |
| Flags (+0xA) | bit0 SBI | `u16 < 2` | CONFIRMED |
| CRC (+0xC) | CRC-32 of 0x930 bytes with the field zeroed | three forms accepted: crc over 0x930 with `[0xC,0x10)` zeroed (the port's), over 0x640 likewise, or over `buf + 0x10` for 0x630 bytes | CONFIRMED |
| Entries (+0x10, 16 bytes) | start, index1, length, type, kind, flags, 0 | `start == running end` (so entry 0 starts at 0), `length != 0`, `index1 >= start`, `index1 < start + length`, `start + length <= sectors`, `type <= 2`, `kind (+0xD) <= 0xF`, `+0xF == 0`; entry 0 must be a data track (`type != 0`); at least one data track; the last end must equal `sectors`; entries after track_count (up to 99) all zero. MODE1 versus MODE2 is not distinguished, only audio versus data | CONFIRMED |
| flag_bits (+0xE) | bits 0..2 | not checked by FUN_10022084 (the legacy FUN_100270a8 requires `<= 7`) | CONFIRMED |
| Writer check of INDEX 01 inside its track | not checked by `mh_v2_build_package_metadata` | `uVar8 <= uVar6 => reject` (end <= index1). A CUE where the pregap of track n+1 starts before INDEX 01 of track n gave a map the firmware (and our loader) refuse | REFUTED, fixed (`fix(v2)`, test `test_trackmap_index_inside_track`) |
| SBI block at +0x640 | u16 length 4..0x2E8, u32 CRC at +0x644, "SBI\0" at +0x648 | only the legacy loader FUN_100270a8 reads it, with the same layout (`len - 4 < 0x2E5`, magic at 0x10027879 "SBI\0", crc over the data); the V2 mount ignores it | CONFIRMED (layout) |
| Cheat blob at base + 5 | up to 0x2800 bytes | `cheat_lba = base + 5`; read with `FUN_10021ce4(cheat_lba, 0, buf)` | CONFIRMED |
| Blob header | "PSTCHT1\0", version u16 (4 or 6), +0xA = 0x40, +0xC = 0, count 1..0x100, +0x20 = 0x40, +0x3A = 0, scope (+0x38) 2 for a disc, +0x28 == size, CRC at +0x3C over the blob with the field zeroed | FUN_100228c8 (v4) and FUN_1002c0a0 (v6): same constants; `+0x38 == 2` is required in both, so only disc scope blobs (scope 2) are served, which is what `write_package.c` writes | CONFIRMED |
| v6 compact blob | 8 byte entries, title index, DB CRC 0xA06CFC31 | FUN_1002c0a0: `+0x1C == 8`, `+0x24 == (n + 8) * 8`, title `< 0x3EC`, `+0x30 == title`, `+0x34 == 0xA06CFC31` (equals `MH_CHEAT_DB_CRC`), `+0x39 == 1`, names limited to space & ' - . 0-9 A-Z and <= 47 characters, line opcodes 0x20/0x21/0x30/0x31/0x00/0x80/0x70/0x90/0xC1, positions unique, total lines `< 0x1FF` | CONFIRMED (`mh_cheat_disc_bank_valid` is the same test) |
| v4 wire blob | 0x60 byte entries, game 0 or 1, audited xor custom flags | FUN_100228c8: `+0x1C == 0x60`, game byte `<= 1`, flag rules (bit3 xor bit2, bit4 needs bit3, bit1 needs bits 0 and 3), custom entries `game == 0`, ids `-1`, audited entries need `entry.game == header.game` and `variant == header +0x30`, header DB CRC 0xA06CFC31 when game != 0, entry CRC of the code, name set without `[` `]`, zero padding | CONFIRMED |
| v4 total lines | profile limit 510 (`MH_CHEAT_MAX_LINES`), validator `< 0x1FF` | `0x100 < u32(+0x18)` rejects: a v4 blob with more than 256 lines is refused (cheat status 3, no cheats for that disc; the card still mounts) | REFUTED, not fixed (description below) |
| 31 active lines limit | port: `active < 0x1FF` | no such limit in the blob loaders | NOT FOUND |
| Reserved slot at base + 0x19 | zero | never read | NOT FOUND |
| Disc art at base + 0x1E | 0x2800 bytes | FUN_100228c8: `art_id != 0xFFFF` then block `base + 0x1E` (EXT, flags bit1) else `base + 0x14`, 0x2800 bytes in chunks of 0x842 | CONFIRMED |
| Gap base + 0x32 .. base + 0xFE | zero | not read | NOT FOUND |
| Resume marker "PSTRSM1" at base + 0xFF, "PSTVFY1", session block "PSTWIP1" at block 0 | Manager only | the strings "PSTRSM1", "PSTWIP1", "PSTVFY1" do not exist in the image. While PSTWIP1 sits in block 0 (and B is zeroed) both headers are invalid, so the card is refused, as intended | NOT FOUND (firmware ignores them) |
| RAW data at base + 0x100 | `sectors` slots of 0xA00, 2352 bytes used, 208 zero | state block: `raw_base = disc.raw_lba` (case 7 of FUN_100228c8 stores `*0x200087F0 = disc+0x14`); the sector read stub computes `(lba - 0x96) * 5` blocks from it (`lsls r0,r3,#2 ; add r0,r12` at 0x100216B0). The tail 208 bytes are not looked at | CONFIRMED |
| Zero length of the slot tail | zero | not checked | NOT FOUND |

## Fixed zone regions

| Fact | Port value | Firmware evidence | Verdict |
|---|---|---|---|
| Case art | 0x16800 + i * 0x2800, id == Case index | `art_lba + art_id * 0x14` blocks, 0x2800 bytes (exactly ends at menu_lba, 0xB4 + 256 * 0x14 = 0x14B4) | CONFIRMED |
| crc_art | verified by the Manager | not read | NOT FOUND |
| Menu | LBA 0x14B4, n slots of 0xA00 (2352 used), n <= 93 | default disc (FUN_100217e0) sets the raw base to header menu_lba; sector k is block `menu_lba + 5 * k`; the state keeps menu_sectors (non zero selects the SD menu instead of the built-in one, FUN_1003015c tests `state[0x28] == 0`) | CONFIRMED |
| Menu CRC / SHA-1 | verified by the Manager | not verified | NOT FOUND |
| PSTCFG2 | LBA 0x1685, 0xA00 slot | FUN_10024450: `cfg_lba != 0`, `cfg_lba + 5 <= 0x2800`, "PSTCFG2\0" (8 bytes), `buf[8] == 1`, `buf[9] == 0` (version u16 1), music lba high dword (+0x20) == 0, CRC-32 of 0..0x92B at 0x92C; the text starts at +0x2C, NUL terminated within 0x800 and non empty | CONFIRMED |
| PSTCFG2 +0xA = 1 | written | not read | NOT FOUND |
| Config text | `theme=umbrella` ... | stored and forwarded to the PS1 side as is; the firmware has no strings "theme", "music" | NOT FOUND (opaque) |
| Music lba (+0x1C), length (+0x24), CRC (+0x28) | 0x168A, <= 0x22EC00 | FUN_100245e0 serves music only if lba, size and CRC are all non zero, `lba < 0x2800` and `size <= (0x2800 - lba) * 0x200` (0x22EC00 for 0x168A); the CRC value is not checked | CONFIRMED |
| Music size window | up to 0x22EC00 | in addition the server splits the music in 0x800 byte chunks and refuses more than 0x100 of them: `((size - 0x31) >> 11) + 1 <= 0x100`, i.e. size <= 0x80030 (524336) bytes. The embedded Umbrella music is 478880 bytes (234 chunks) | CONFIRMED for the embedded file; limit stricter than `MH_V2_MUSIC_MAX` |
| Fixed zone end | 0x500000, packages from LBA 0x2800 | same constant 0x2800 | CONFIRMED |
| Hidden bytes 0x16400..0x16800 | zero | not read | NOT FOUND |

## Discrepancies

Anything that could make a card written by the port unreadable:

1. **Track map with an INDEX 01 outside its own track (REFUTED, fixed).** The writer built
   `end <= index1` entries for a CUE whose next track has a pregap longer than the length of the
   current track from INDEX 01; FUN_10022084 rejects that map, the disc becomes unplayable and the
   Manager's own `mh_track_sector_valid` also refuses it on reload. Fix: `write_package.c` now runs
   `mh_track_sector_valid` on the map before using it (the same predicate as the loader and the
   firmware). Test: `test_trackmap_index_inside_track` in `core/tests/test_v2_firmware.c`. Side effect:
   the old `test_v2_write` case "the last track is not checked against the end of the disc (original
   behaviour)" (INDEX 01 of the last track beyond the disc) now expects a refusal, because the
   firmware refuses that map too; this deliberately departs from the original Manager.
2. **Version 4 cheat blob with more than 256 lines (REFUTED, fixed).** The profile limit is 510
   lines and `mh_cheat_blob_validate` accepts `< 0x1FF`, but FUN_100228c8 rejects `+0x18 > 0x100`
   (re-read in a second decompile of the same UF2, same build of Feb 5 2026). The card mounted and
   played, but the cheats of that disc were silently ignored by the firmware. Only custom profiles need
   the v4 (wire) format; compact v6 blobs are accepted up to 0x1FE lines. Fix: the wire branch of
   `mh_cheat_profile_serialize` now refuses more than 256 lines with a clear message. The validator is
   left as is, so cards written by the original Manager still open. Test:
   `test_wire_firmware_line_limit` in `core/tests/test_cheat_profile.c`.
3. Non-ASCII names (informational): every byte outside 0x20..0x7E of a Case name, Disc title or
   serial is shown as a blank by the Pico. The card stays readable, but accented titles lose letters.
   The writer copies the UTF-8 bytes unchanged. Printable ASCII, parentheses included (0x28, 0x29), is
   shown as is, so the firmware has no restriction on them; the Manager's title normaliser (a port of
   the original) is what turns "(USA)" into "USA". The display limit is 31 characters for a Case name
   and for a Disc title: the port cuts the game name and keeps the tags after it (region, revision,
   disc) via `mh_fit_name`.
   The limit is 31, not 41: both fields are 0x20 bytes and the Manager keeps one for the terminator
   (FUN_10021960 and FUN_10021d84 copy at most 0x20 bytes into a 0x21 byte buffer, so the Pico could show
   a 32nd character, but the original writer never fills it). The 0x47 cap seen in FUN_100222e0 only bounds
   the disc title plus the " - DISCO n" suffix the firmware appends in Cases with several discs. Example:
   "Alundra 2 - A New Legend Begins (USA)" is stored as "Alundra 2 - A New Legend Begins" (31 characters, the region dropped).
   The PS1 menu font has a glyph for every ASCII character from 0x21 to 0x7E (FUN_800d033c, 6 pixels wide),
   so "(" and ")" are drawn. Since v0.4.5 the Manager writes the name the way the CUE file has it, with
   `mh_card_title` instead of `mh_normalize_title`: case and parentheses are kept, a language list such
   as "(En,Fr,De)" is dropped. The parentheses cost 4 characters, so more names need a cut: the region
   goes first ("Chrono Cross (USA, Canada) (Disc 1)" is stored as "Chrono Cross (Disc 1)"), and only if the
   name is still too long is the game name cut, keeping the revision and disc tags. The lookup keys used by the cheats and the database still use
   `mh_normalize_title`. Cards written before v0.4.5 keep their old upper-case names until the game is
   added again.
4. Music longer than 0x80030 bytes would be accepted by the Manager but never played (see table). The
   embedded resource is fine; do not replace it by a longer file.

Where the firmware is looser than the port (safe): art ids other than the index, table and menu
CRCs, SHA-1, MBR fields, markers, and the CRC forms of the track map. Where it is stricter than
`mh_v2_header_valid` but the writer already complies: `(case_count == 0) == (disc_count == 0)`,
empty records entirely zero, zero bytes at Case +0x3F..0x4B and Disc +0x94..0xFB.

Other layouts the firmware also understands (not produced by the port): PSXRAW version 1 single
disc (first branch of FUN_10021280, header 0x130 bytes with a "DISC" record at +0x130), version 1
multi disc (FUN_100273c4, up to 6 discs) and an older raw card header read from sector 0 (FUN_10018ff2:
"PSXRAW", track count at +0xC, 10 byte track entries).
