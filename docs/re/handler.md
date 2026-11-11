# PS1 command handler (work package B)

Rebuild of the code that answers reads of "virtual sectors" for the PS1 menu in `PicoStation_MH_V3.uf2`.
Sources: `include/mh/handler.h`, `include/mh/handler_cheat.h`, `src/mh/handler.cpp`, `src/mh/handler_cheat.cpp`.
Tests: `tests/test_handler.cpp` (card image from `tests/card_image.py`), `tests/test_handler_cheat.cpp`.
Ledger: `re/ledger/handler.tsv`. All addresses are flash addresses (Ghidra `FUN_<addr>`). Nothing here was run on
a console.

## 1. How the handler is wired

V3 patched the sector read path by chaining hooks. The first hook that returns "handled" wins:

| Order | Function | Role |
|---|---|---|
| 0 | FUN_100300b8 | built-in fallback menu (package C) |
| 1 | FUN_1002d02c | `DEAD` hook for the library listing sector (section 7.1) |
| 2 | FUN_1002f118 | refuses N=900 after a confirmed 0xF9 (section 7.2) |
| 3 | FUN_1002e100 | cheat chunks 0..99 of version 6 blobs, menu phase only (section 5) |
| 4 | FUN_100245e0 | config/music server (package C), reached through FUN_1002e8c2 |
| 5 | FUN_100228c8 | main handler (called from the dispatcher at 0x100247be) |

Each hook falls through to the next with the original arguments `(ctx, dest, lba, location, scramble table)`.
The handler answers only when `location == 1`, the three pointers are non zero, `lba >= 0x96`, the state block
magic (`state+4 == 0x39313334`) is set. Its answer is always the 0x914 byte response buffer (0x20034000), sent by
the upstream DiscImage code (RAM 0x20000588). In the rebuild `read_sector()` returns true and a pointer to
`card::response()`; false means "not answered, read the sector from the disc image". The caller keeps the null
pointer checks. `N = lba - 0x96` is used in the original conditions.

Function classification:

| Function | Size | Class | Rebuild |
|---|---|---|---|
| 100228c8 | 2174 | main handler | `read_sector_main` (+ pieces) |
| 100229f8 | 64 | tail of 100228c8 (shares its frame): cache key, send, session counter | `finish()` |
| 10022bf4 | 8 | tail piece: copy 0x28 bytes then 100229f8 | `build_session_page()` |
| 10022518 | 96 (+ cave) | PGM1 disc info builder | `build_disc_info` |
| 10029000 / 10029020 | 384 | cave replacing the inline serial check of 10022518 | `build_disc_info`, `serial_valid` |
| 1002e100 | 1642 | v6 cheat fast path (active) | `hook_cheat_v6` + `v6_*` |
| 1002c0a0 | 1654 | unreferenced twin of 1002e100 (no pointer, no call). Differences: needs `cmd == 0xB`, never reads `0x20033800`, `size - 0x40 > 0x27c0` and a version 4 blob both fall through | not rebuilt |
| 1002d02c | 178 | DEAD hook | `hook_dead` |
| 1002f118 | 102 | N=900 refusal hook | `hook_reject_session` |
| 1002f000, 1002f018 | 14, 40 | getters: event session id; "game running and last event is 2 or 4" | `confirm_ready` |
| 1002f050, 1002f0b0 | 82, 84 | 0xF9 arm / judge | `on_confirm_command`, `finish_confirm` |
| 1002f190, 1002a000..1002a01a | patch shims | call 1002f0b0 once per poll (and 1002773c, package C, when r3 != 0); not functions | `finish_confirm` |
| 1002e0a0 | 64 | drop PCR2 request | `abort_cheat_request` |
| 100227cc | 218 | PS1 command decoder | `on_pad_command` |
| 1002b000 | 28 | cave of the main handler: DB CRC compare for version 4 | inside `v4_validate` |
| 1002c006, 1002c060, 1002c078, 1002e004, 1002e038, 1002e060, 1002e078, 1002e000 | small | copies of CRC-32, append, hex, load32 for the 1002c0a0/1002e100 caves | card.h helpers / `ChunkSink` |
| 1002d000 | 32 | wrapper: calls 1002d11c, clears `0x20032ef2`, `0x20008f74`, `0x20037fe0` | not handler (SD glue) |
| 1002d11c | 446 | tail call into RAM 0x20003424 (SD read) | not handler |
| 1002e8c2 | 20 | forwards to FUN_100245e0 | chain glue |
| 10023408, 10023514, 1002351c | libgcc divide | n/a |
| 10024450, 100245e0, 100270a8, 100273c4, 10030000, 100300b8 | | package C | not touched |

## 2. Sector windows (absolute LBA, `N = lba - 0x96`)

| LBA | N | Answer | Request that selects it |
|---|---|---|---|
| 0x00..0x95 | | never answered | |
| 0x96..0xF2 | 0..0x5C | SD menu image, not answered in phase 1 | |
| 0xFA | 100 | library listing (card.h `library_listing`) | cmd 1..4 |
| 0x12C | 0x96 | `CFG1` page | none |
| 0x15E..0x167 | | art chunks `PART` / `FART` | cmd 5 (0xB) |
| 0x1C2..0x3C1 | | cheat text chunks `PCH2` | PCR2 request (cmd 11) |
| 0x226 | 400 | disc info `PGM1` (inside the cheat window, see quirk Q1) | cmd 6 (0xC) |
| 0x41A | 900 | session events `PLS1` | cmd 7..10 |
| any other | | all zero page | |

Gate before answering (`LAB_100229be`): not answered when `phase == 2 && N != 900`, or `phase == 3`, or
`phase == 1 && N < 0x5D`. A cached response is sent as is when `state.cache_key == N` (the key is set after
every build; `reset_response_cache` / `post_event` store 0xFFFFFFFF). Any other `N` gets a zero page.

## 3. PS1 commands and pending request (FUN_100227cc)

The PS1 sends 24 bit command words; only `(word & 0xF00000) == 0xF00000` is looked at, state magic and
`phase != 0` required. `arg = word & 0xFFFF` is stored in `state.cmd_arg` (+0x44). `sub = (word >> 16) & 0xF`:

| sub | `state.cmd` (+0x40) | other effect |
|---|---|---|
| 0 | 0 | |
| 1, 2 | 1, 2 | `list_offset = 0` |
| 3 | 3 | arg = n-th used Case |
| 4 | 4 | `list_offset = arg` |
| 5 | 7 | switch request, arg = Disc slot |
| 6 | unchanged | PCR2 control, `pcr2_mode = arg`. arg 2: zero the 0x24 byte packet, `pcr2_fill = 0`, `cheat_pending = 0`, `pcr2_ready = 0`. arg 0: `pcr2_ready = 0`, `cmd = 0` |
| 7 | unchanged | two PCR2 bytes (big endian) when `pcr2_mode == 2` and fill < 0x24 (a lone last byte is stored when only one fits). At 0x24: `pcr2_ready = 1`, `cmd = 11`, `cheat_pending = 0`, `pcr2_mode = 0` |
| 0xB | 5 | art select, arg = Case (n-th used) or Disc slot |
| 0xC | 6 | disc info, arg = Disc slot |
| 0xD | 8 | confirm, arg = session id |
| 0xE | 9 or 10 | `arg = word & 0x7FFF`; bit 15 set: 10 (cancel), clear: 9 (finish) |
| 8, 9, A, F | unchanged | 9 is the 0xF9 channel (section 7.2) |

Every accepted word ends with `reset_response_cache()`.

Consumption (main handler, before building): cmd 1..4 are consumed by a read of N=100, cmd 5 by a read in the art
window, cmd 6 by N=400 (build `PGM1`, `cache_key = 400`, answer), cmd 11 by any read of N=400 or of the cheat
window, cmd 7..10 by a read of N=900. Consuming means `cmd = 0` and `reset_response_cache()`. A request is not
consumed by a read of another window.

List/art requests (before the listing is built): cmd 1, 2: `sel_case = 0xFFFFFFFF`, `list_offset = 0`. cmd 3:
`sel_case = index of the n-th used Case` or 0xFFFFFFFF, `list_offset = 0`. cmd 4: `list_offset = arg`.
cmd 5: `art_block` = (a Case is selected, `sel_case < 0x100`) `disc.base_lba + (flags & 2 ? 0x1E : 0x14)` for
Disc slot `arg` when its art id is not 0xFFFF, else (no Case selected) `art_lba + case.art_id * 0x14`; 0 when none.

## 4. Responses

All little endian. The buffer is zero filled (0x914 bytes) before a response is built.

**Config page (N=0x96), `CFG1`**: `+0 "CFG1"`, `+4 u16 0x800`, `+0x114` first 0x800 bytes of the block at
`state.cfg_lba`. When the read fails the page is `"DEAD"` (4 bytes) and nothing else.

**Art chunk k = lba - 0x15E (`PART` / `FART`)**: chunk bytes `0x842`, image 0x2800 bytes at `art_block`.
`off = k * 0x842`, `len = min(0x842, 0x2800 - off)` (0 for k >= 5). Good: `+0 "PART"`, `+4 u8 k`, `+5 u8 5`,
data at `+6`. No art, `len == 0` or read failure: `+0 "FART"` only.

**Disc info (N=400), `PGM1`** (FUN_10022518, serial path FUN_10029020). Valid card and Disc of the selected Case
(else the card layer error page: `PGM1`, `+4 = 1`, `+5 = 3`, `+6 u16 slot`, `+0x100` CRC, event 8 with arg 10).
Selected disc record copied to `state.disc`, `sel_disc_id`, `sel_slot = slot`, `game_id` (0x80 bytes) cleared.

| Offset | Valid serial `ABCD-12345` | Other serial |
|---|---|---|
| +0 | "PGM1" | "PGM1" |
| +4, +5 | 1, 1 | 1, 2 |
| +6 | u16 slot | u16 slot |
| +8 | u32 token | u32 token |
| +0xC | 3 if `cheat_size != 0` else 2 | 2 |
| +0x10 | 16 bytes of serial | zero |
| +0x20 | title, up to 32 bytes | same |
| +0x80 | boot path (also in `game_id`), up to 0x7F bytes | zero |
| +0x100 | CRC-32 of 0..0xFF | same |

Serial rule: bytes 0..3 `A`..`Z`, byte 4 `-`, bytes 5..9 digits, byte 10 NUL. Boot path:
`cdrom:\` + serial[0..3] + `_` + serial[5..7] + `.` + serial[8..9] + `;1` (SLUS-01042 gives
`cdrom:\SLUS_010.42;1`).

Token: `x = (slot << 8) ^ state.generation ^ disc.record_crc ^ disc.data_crc32 ^ (disc.id << 24)`;
`token = (x == 0x314C4553 "SEL1") ? 1 : x ^ 0x314C4553`.

Event bookkeeping after the response (same in both paths): `event.reserved14 = token`; `event.session =
((token >> 16) ^ token) & 0x7FFF`, or 1 when that is 0; `event.token = slot`; `event.reserved18 = generation`;
`event.reserved1c = data_crc32` (record_crc when 0); `event.flags = 9`; then `post_event(0, 0, 0)` (seq + 1,
flags become 0xD, code 0, CRC refreshed, cache dropped). The "token" at +0xE of the event is the slot in V3 (the
card.h comment calls it a token).

**Event page (N=900), `PLS1`**: `event.crc = crc32(event, 0x24)`, then the 0x28 byte event record:
`+0 "PLS1"`, `+4 u16 1`, `+6 u16 0x28`, `+8 u32 seq`, `+0xC u16 session`, `+0xE u16 slot`, `+0x10 code`, `+0x11 arg`,
`+0x12 u16 flags`, `+0x14 token`, `+0x18 generation`, `+0x1C data crc`, `+0x20 extra`, `+0x24 crc`.
Serving it with `event.code == 9` increments `switch_count` and moves `phase` to 4.

**PCR2 request** (PS1 to Pico through cmd 6/7, 0x24 bytes, big endian words):
`+0 "PCR2"`, `+4 u16 0x24`, `+6 u16 sel_slot`, `+8 u32 token (event.reserved14)`, `+0xC u32 seq (event.seq)`,
`+0x10 16 byte serial (state.disc.serial)`, `+0x20 u32 CRC-32 of 0..0x1F`. Valid also needs `pcr2_ready`,
`event.code == 0` and `event.flags & 1`.

**Cheat chunk, `PCH2`** (chunk k = lba - 0x1C2, 0x800 text bytes per chunk):

| Offset | Field |
|---|---|
| +0 | "PCH2" |
| +4 | u16 0x30 (header size) |
| +6 | u16 k |
| +8 | u16 length of the data (0..0x800) |
| +0xA | status: 0 ok, 1 no cheats (cheat_lba 0, size 0 or out of range), 2 past the end or SD read failure, 3 invalid request/session/blob |
| +0xB | 1 when this chunk reaches the end of the text (always 1 with status != 0) |
| +0xC | u16 `sel_slot` |
| +0x10 | u32 token, +0x14 u32 seq, +0x18 16 byte serial |
| +0x28 | CRC-32 of the data |
| +0x2C | CRC-32 of header bytes 0..0x2B |
| +0x30 | text |

Errors have length 0, last = 1 and an otherwise zero page.

Main handler (version 4): the first chunk request of a PCR2 packet (`cheat_pending == 0`) loads the blob:
`cheat_pending = 1`, `cheat_status = 3`, `cheat_bytes = 0`; `cheat_lba == 0`, `cheat_size < 0x40` or
`cheat_size > (disc flags bit1 ? 0x2800 : 0x1400)` gives status 1; SD read failure 2; failed validation or text
overflow 3; success: `cheat_bytes` = text length, status 0. Later chunks serve slices of the rendered text; the
verdict (status != 0) sticks until the next PCR2 start.

## 5. Cheat blobs

Common header (0x40 bytes), all fields little endian:

| Off | Field | v4 (wire) | v6 (compact) |
|---|---|---|---|
| 0 | magic | "PSTCHT1\0" | same |
| 8 | u16 version | 4 | 6 |
| 0xA | u16 header size | 0x40 | 0x40 |
| 0xC | u32 | 0 | 0 |
| 0x10 | u32 entry count | 1..0x100 | 1..0x100 |
| 0x14 | u32 | entries with flag bit0 | same |
| 0x18 | u32 | total code lines, <= 0x100 | total code lines, < 0x1FF |
| 0x1C | u32 entry size | 0x60 | 8 |
| 0x20 | u32 table offset | 0x40 | 0x40 |
| 0x24 | u32 first payload | 0x40 + n*0x60 | (n + 8) * 8, <= size |
| 0x28 | u32 total size | table end <= total <= cheat_size | == cheat_size |
| 0x2C | u32 title | game 0: 0xFFFFFFFF, game 1: != -1 | < 0x3EC |
| 0x30 | u32 variant | game 0: 0xFFFFFFFF, game 1: != -1 | == title |
| 0x34 | u32 database CRC | game 0: 0; game 1: 0xA06CFC31 | 0xA06CFC31 |
| 0x38 | u8 scope | 2 | 1 or 2 in the header test, finally 2 |
| 0x39 | u8 game | 0 or 1 | 1 |
| 0x3A | u8 | 0 | 0 |
| 0x3C | u32 CRC-32 | of 0..total-1 with 0x3C..0x3F read as 0 | same |

v4 extras: bytes between `total` and `cheat_size` must be zero. 0xA06CFC31 is the cheat database CRC
(`MH_CHEAT_DB_CRC` in the Manager).

**v4 entry** (0x60 bytes at 0x40 + i*0x60): `+0 name[0x40]`: printable 0x20..0x7E except `[` `]`, NUL terminated
within 0x40, non empty, bytes after the NUL zero; `+0x40 u32` variant or -1; `+0x44 u32` id or -1; `+0x48 u32`
offset of its lines (running sum starting at table end); `+0x4C u16` line count (> 0); `+0x4E u8` game;
`+0x4F u8` flags (<= 0x3F); `+0x50 u32` CRC-32 of its lines; `+0x54..0x5F` zero. Flag rules: bit3 (audited) xor
bit2 (custom); bit4 needs bit3; bit1 needs bits 0 and 3 (bit1 prints " (M)"). Audited entries (bit3): header game
non zero, entry game == header game, `+0x40 == header variant`, `+0x44 != -1`, bit2 clear. Custom entries: game 0,
`+0x40 == +0x44 == -1`. Totals must match header +0x14 (bit0 entries), +0x18 (lines), `cursor == total`; with
game 1 at least one audited entry. Lines are 6 bytes: u32 big endian address then u16 big endian value; opcodes are
not checked in v4.

**v6 entry** (8 bytes at 0x40 + i*8): `u16 name_off`, `u16 lines_off`, `u16 id` (<= 0xFF, unique), `u8 lines` (> 0),
`u8 flags` (`flags & 0xCC == 8`, `flags & 3 != 2`). `name_off` = running cursor starting at `(n+8)*8`; the name
alphabet is space `&` `'` `-` `.` `0`-`9` `A`-`Z` (mask 0x3FF60C1 over 0x20..0x39), 1..47 characters, NUL
terminated; `lines_off` = byte after the NUL, `size - lines_off >= lines * 6`; the first byte of each line must be
one of 0x00 0x80 0x20 0x21 0x30 0x31 0x70 0x90 0xC1 (`(b & 0x7F) == 0 || (b & 0xEE) == 0x20 || (b - 0x70) & ~0x20 == 0 || b == 0xC1`).
After the table: cursor == size, header +0x14 and +0x18 equal the totals.

**Rendered text** (what the PS1 receives):

```
MHDialect=V5ORIG_BANK_V01\n
[NAME]\nType=V5Native1\nMHFlags=XX\n          (v4 with bit1: [NAME (M)])
AAAAAAAA VVVV\n                                one per line, upper case hex, XX = flags & 3
\n                                             blank line after every entry
```

v4 renders the whole text into the 0x3000 byte text buffer (refused above 0x2FFF characters, status 3); v6
renders only the requested 0x800 byte window and skips entries that end before it. The exact length of an entry
is `30 + name_len + 14 * lines`, which the skip logic uses.

## 6. Session machine (N=900 with a pending cmd)

States are `CardState.phase`: 1 menu, 2 game, 3 fault (nothing answered), 4 switch acknowledged. Events are
posted with `post_event(code, arg, extra)`; every post bumps `seq`; `post_event_ack(arg, extra)` is code 8.

| cmd | Condition | Result |
|---|---|---|
| 7 switch | `flags & 1` clear or `cmd_arg != sel_slot` | ack(2, arg) |
| | else: event 1; `load_track_map(state.disc)` succeeds and a disc table exists | `raw_base = disc.raw_lba`, `sector_cursor = 0`, `total_sectors = disc.sectors + 2*0x122A`, phase 2, `flags |= 2`, event 2 |
| | else | `default_disc` (menu), phase 1, ack(7, arg) |
| 8 confirm | `event.session != (u16)arg` | ack(3, arg) |
| | `((code - 2) & 0xFD) != 0` (code not 2 or 6) | ack(4, arg) |
| | else | event 3, event 4 |
| 9 finish | session mismatch | ack(3, arg) |
| | `code != 4` | ack(4, arg) |
| | else | `flags |= 0x10`, event 5, `flags |= 0x30`, event 9, `switch_count = 0` |
| 10 cancel | session mismatch | ack(3, arg) |
| | else | event 6, `default_disc`, phase 1, `flags &= 0xFFCD`, event 7 |

After the request: `cmd = 0`, cache dropped. Event 9 on the served page moves to phase 4 (section 4).
Event code meanings: 1 armed, 2 disc started, 3/4 confirm steps, 5 confirmed, 6 closing, 7 back at the menu,
8 rejected (arg 2 slot/flag, 3 session, 4 phase, 7 load failure, 10 disc info error), 9 done.

## 7. Side hooks

7.1 FUN_1002d02c (DEAD): `location == 1`, lba 0xFA, phase 1, state magic, `cmd` is 1 or 2. It clears the v6
validation cache; if `0x20032ef2 == 0` it runs the SD init (RAM 0x20003188, `SdIo::init`), sets
`cache_key = 0xFFFFFFFF`, and when init fails (non zero) clears `cmd`, zero fills the page, writes "DEAD" and
answers. Otherwise it falls through.

7.2 0xF9 confirmation. FUN_1002f050 (after the aux command decoder): command `0xF9xxxx` with
`(word & 0x7FFF) == event.session != 0`, a game running with last event 2 or 4 (`FUN_1002f018`), `cmd == 0`: stores
`cmd_arg = word & 0xFFFF`, `cmd = 12`. FUN_1002f0b0 (once per poll): if `cmd == 12`: drop the cache; when still
confirmable, session matches and bit 15 of the word is set, `cmd_arg |= 0x80000000`, else `cmd_arg &= 0xFFFF`;
`cmd = 0`. FUN_1002f118: `location == 1`, lba 0x41A, confirmable, `cmd == 0`, `cmd_arg` bit 31 set and
`cmd_arg & 0x7FFF == event.session`: the sector is not answered (the disc image provides it).

7.3 FUN_1002e0a0: phase 1, `cmd == 11`: `cmd = 0`, v6 cache dropped, response cache dropped.

## 8. Globals outside the state block

`0x20032ef2` (byte, `Env::service_busy`), `0x20033800` (word, `Env::hold`), `0x20008f74`, `0x20037fe0..0x20037ff4`
(v6 validation cache: tag "PHC6" 0x36434850, cheat_lba, size, token, seq, slot), `0x20035000` (0x2800 byte blob
buffer), `0x20038000` (text). Their writers outside this package were not identified; `service_busy` and `hold`
are inputs of the rebuild (`handler::env()`).

## 9. Quirks reproduced and deviations

* Q1: LBA 0x226 (N=400) is inside the cheat window (chunk 100). Only the cached copy or cmd 6 produces PGM1; an
  uncached read answers as cheat chunk 100 (status 3 without a request). The block `if (N == 400)` in the original
  build stage is dead code. The v6 fast path stops at chunk 99, which is exactly the chunk before it.
* Q2: the 0xFA listing entries for Discs carry flag 0 in the original (`FUN_10022284` is called with r2 = 0 at
  0x100224c0), Cases carry flag 1. `src/mh/card.cpp` (package A) writes 1 for Discs; to be checked by A. The
  handler tests do not depend on it.
* Q3: `FUN_1002e100` leaves `cmd == 11` set; only the main handler (or `abort_cheat_request`) consumes it.
* Q4: `FUN_1002e100` re-reads and re-validates the blob on every chunk 0, and caches only chunk >= 1.
* Q5: the event byte `0x198` used as "phase" in the decompile is `event.code`; `0x19A` is `event.flags`.
* Deviation D1: validation and text rendering are separate (`v4_validate`, `v4_render`); the original renders while
  validating. Observable result is the same (a text overflow is status 3).
* D2: the v4/v6 validators take the blob size explicitly and never read outside it; the original validates
  against the size it read. The record validator in card.h already limits `cheat_size` to 0x1400 / 0x2800.
* D3: `serial_valid` stops at the first bad byte (the original also tested fixed positions).
* D4: `read_sector` returns a pointer instead of calling the sender; the 900 post-send accounting runs before
  returning. Pointer null checks stay with the caller.
* D5: the disc table pointer (`state+8`) is passed with `handler::init()`; it is a host pointer, not an address.
* Not rebuilt: FUN_1002c0a0 (unreferenced), FUN_1002d000/1002d11c (SD glue), the 0x1002a000 shims.

## 10. Uncertain

* Which code sets `Env::service_busy` and `Env::hold` (and 0x20008f74).
* Who calls the shims at 0x1002a000/0x1002f190 (poll location); `finish_confirm` is exposed as a plain function.
* Whether `*0x2003400C = 2` in FUN_10029020 is a byte or a word store: disassembly shows `strb`.
* The meaning of the tag "SEL1": only the arithmetic is known.
