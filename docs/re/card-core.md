# Card core (work package A)

Sources: `include/mh/card.h`, `src/mh/card.cpp`, `card_io.cpp`, `card_util.cpp`. Tests: `tests/test_card.cpp`
(image generator `tests/card_image.py`). Ledger: `re/ledger/card.tsv`. Function names are Ghidra's `FUN_<flash address>` of
`PicoStation_MH_V3.uf2`. Field offsets of the on-card records are in `CARD_LAYOUT_CROSSCHECK.md`; this file adds what the
firmware does with them.

## Map

| Rebuild | V3 |
|---|---|
| `zero_fill copy equal all_zero read_u32 store_u32 crc32_update crc32 retry_delay` | 100216d8 100216e6 100216f8 10021714 1002172c 10021742 10021748 10021774 10021788 |
| `copy_printable` | 10021924 |
| `text_append_char/str/hex` | 100218ac 100218d4 100218f4 (length variable + 0x3000 byte buffer) |
| `read_blocks read_bytes read_slot` | 10021c14 10021ce4 1002200c |
| `header_valid` | 10021aa4 |
| `validate_case nth_used_case case_by_index` | 10021960 10021c50 10021d50 |
| `validate_disc disc_by_id disc_of_case` | 10021d84 10021fbc |
| `crc32_window` | 10022040 |
| `load_track_map` | 10022084 |
| `mount default_disc` | 10021280 100217e0 |
| `post_event post_event_ack reset_response_cache` | 10021858 1002189c 100217b0 |
| `listing_append library_listing` | 10022284 100222e0 (trailer = 10021a70) |
| `disc_info_request` | 10022518 (failure path only) |
| `sector_route sector_block` | stub 0x10021640..0x100216d8 |

## What the firmware does that the layout document does not say

* `mount(param_1)`: `param_1` is the disc table (`picostation::DiscImage`, 0x147C bytes in V3: the `CueDisc` with `MAXINDEX 2`,
  so a track is 0x34 bytes and `CueDisc` is 0x1470 bytes, then `m_hasData` at 0x1470, current track at 0x1474, the two skip
  flags at 0x1478/0x1479 and a V3 byte at 0x147A that is set to 1 whenever the card supplies the sectors, tested by the sector
  read function FUN_20000fa0 before it jumps to the stub). `CardDisc` mirrors this layout with static_asserts.
* Order inside mount: self check of the RAM stub copy (not reproduced, see below), zero the 0x1b0 state block, magic
  `0x39313334`, disc table pointer, `cache_key = sel_case = 0xFFFFFFFF`, SD init (nonzero = fail), read slot 0 (16 tries).
  If the version byte (+6) is 1 the legacy single disc branch runs and nothing else. Otherwise slot A is read AGAIN
  (the original reads LBA 0 twice) and validated, then slot B, then the choice (B iff `(int32)(B - A) > 0`), then the state
  fields, `valid = 1`, the event record zeroed with flags 8, `post_event(0, 0)`, `default_disc`, and last `phase = 1`.
* Legacy PSXRAW v1: header 0x930 bytes in slot 0 with `u16 @+0xA == 0x930`, version 1, bytes 7..9 = 1,0,0, CRC at +0x10 over
  0x930 bytes (or the first 0x130) with `[0x10,0x14)` read as zero; one DISC record at +0x130 (`used 1, id 0, case 0xFF, slot 0`,
  high dwords zero, `track_lba == base`, `raw_lba == base + 5` (not +0x100), `raw_blocks == sectors * 5`, track_count 1..99,
  record CRC). It builds a Disc info (id 0, case 0xFF, art 0xFFFF) and calls the track map loader. Result: `phase` stays 0,
  `raw_base = raw_lba`, total sectors `sectors + 2 * 0x122A`. The state block keeps only magic and disc pointer.
* Default disc (the menu): one data track of 0x385 (901) sectors with the 0x122A lead-in, lead-out marker track at 0x385,
  `raw_base = menu_lba`, `total = 0x27D9`. The PS1 reads the menu as `raw_base + (lba - 0x96) * 5`.
* Track map loader: three accepted CRC forms (0x930 or 0x640 bytes with `[0xC,0x10)` zeroed, or `crc32(buf+0x10, 0x630)`).
  Track i+1 of the table gets `size = len, file_offset = start, index_count = 1, indices = {start, index1} + 0x122A,
  type = audio ? 1 : 2`; track 0 gets `indices[1] = 0x122A`; the lead-out marker track n+1 gets `file_offset = end` and both
  indices `end + 0x122A`.
* Case validator quirks: used flag is any nonzero byte; the index compared is the table position, not masked; the name copy
  (FUN_10021924) writes up to 32 characters (indices 0..0x1F) into a 0x21 byte buffer, so a Case name that fills the whole 0x20
  byte field shows all 32. The Manager's writer stops at 31. Serial copies stop at 15.
* nth_used_case (10021c50) returns false as soon as ONE record is invalid (-1) while the library listing (100222e0) just skips
  invalid records. Both stop after `case_count` valid used records.
* Library listing (100222e0): response buffer zeroed (0x914 bytes); entries `[len][1][chars]` appended while
  `pos + 6 + len <= 0x914` (room for the 4 byte trailer); trailer `[0][err][count hi][count lo]`. `err = 1` when an entry did not
  fit. Count rules: Case list = `case_count` when `list_offset == 0`, else the number appended; Disc list = `disc_count`
  of the Case when `list_offset == 0`, else the number appended. Texts: `ERRO RAW INVALIDO` (state.valid != 1),
  `CASE INVALIDA` (selected Case unreadable), `CASE PS1` (empty Case name), `JOGO PS1` (empty Disc title). Disc entries get
  ` - DISCO n` (n = slot + 1) when the Case has more than one disc, cut at 0x47 characters. A Disc whose record is invalid or
  whose back references (case index, slot) do not match is skipped.
* PLS1 event record (state +0x188, 0x28 bytes): magic, version 1, length 0x28, sequence (wraps to 1, never 0), code, arg,
  flags (OR 4 on every post), `extra`, CRC-32 of the first 0x24 bytes; each post also clears the response cache. The code
  argument of FUN_1002189c is code 8 with `arg` = first parameter.
* Sector stub (flash 0x10021640..0x100216d8) is not a function: it is the SD branch of FUN_20000fa0 (taken when the V3 byte
  `CardDisc::sd_menu` is set), entered with live registers. Phase 1: sector >= 0xF3 goes to the built-in decoder
  (FUN_100300b8, the fallback menu) first; phase 2: only sector 0x41A; phase 4: sector 0x41A, any other sector moves the phase to
  3 and invalidates the response cache table (not `cache_key`). Afterwards, if phase 1 and menu_sectors == 0 (no menu on the
  card) the built-in path is used, else the card is read at `raw_base + (lba - 0x96) * 5`. `sector_route` returns that decision,
  `sector_block` the block. Both are `__not_in_flash_func`. Mount's copy and compare of this stub to RAM has no
  equivalent and is dropped.
* FUN_10022518 spans 0x10022518..~0x100227c0 (Ghidra stops at an indirect jump to 0x10029000). The part rebuilt is the failure
  response ("PGM1", status 3, slot, CRC at +0x100, event 8/10). The success part builds the disc selection response and the
  `game_id` text (state +0xA4, 128 bytes: 4 upper case letters, 6 digits, derived from the serial) and belongs to the handler.

## State block 0x1b0

`CardState` in `card.h` (every field has offset, source and meaning). Fields marked "inferred" came from the PS1 command
handler FUN_100228c8 and FUN_100227cc; names for +0x40..+0x7C and the phase meanings are guesses from usage. Phase values:
0 none/legacy, 1 menu, 2 game, 3 fault, 4 switch acknowledged. `disc` (+0x124, 0x64 bytes) is the validator output of the
selected Disc, `event` (+0x188) the PLS1 record; the 16 bytes at disc +0x30 are the serial that the cheat request (`PCR2`,
state +0x80) must echo.

## Deviations from the original (all on untrusted card data)

* `load_track_map` validates the whole map first and only then fills the disc table; the original writes the table while it
  validates, so a bad map leaves a half built table.
* `read_blocks` refuses 0 or more than 5 blocks and a missing SD function; `read_slot` with 0 tries returns false (the original
  would loop); `nth_used_case`/`library_listing` bound every index; `load_track_map` re-checks `track_count` in 1..99.
* `read_u32/store_u32` are alignment safe (memcpy) where the original uses word loads.
* `post_event(0, 0)` in mount passes `extra = 0`; the original leaves the third argument register undefined (the CRC of
  that first event therefore may differ).
* `copy_printable` for serials is called with a source limit of 0x10; the original passes an undefined register, the 15
  character limit is reached first either way.
* `disc_image_addr` is a 32 bit field (the device pointer); on a 64 bit host it holds the truncated address.

## Tests

`make -C tests test_card` (clang, ASan/UBSan): 1617 checks: CRC vector, name/text helpers, chunking of reads, retries,
A/B rule over boundary generations (11 x 11 pairs, one header damaged, B unreadable), every damaged header region refused,
header values the firmware ignores still accepted, Case and Disc table field-by-field rejection (bit flip and CRC-resealed
logical errors), empty records must be all zero, 31/32 character names, non ASCII bytes become blanks, track map rules and
three CRC forms, library listing text for the V2 image generated by `tests/card_image.py` (paging, suffix, overflow with 80
Cases), legacy PSXRAW v1 mount, stub routing, event record.

## Not done / uncertain

* Similarity is size weighted 0.47 (33 functions): the small leaves are 0.75 to 1.00, the large functions 0.24 (track map) to
  0.64. The compiler shape of the big validators was not iterated further. Ledger sizes use the rebuilt size (original sizes
  differ for the multi function bodies).
* Not rebuilt: the wrapper at 0x10021624 (command hook that calls FUN_10024588), the success part of FUN_10022518, and
  everything in `handler`/`aux` packages. The semantics of the state fields +0x40..+0x7C other than `cmd`, `cmd_arg`,
  `list_offset`, `cache_key`, `sel_case`, `art_block`, `sel_slot` are only inferred.
