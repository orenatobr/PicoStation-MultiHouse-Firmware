# Long names extension (card format 2.1)

The V2 card stores a Case name and a Disc title in 0x20 byte fields: 31 characters plus NUL. The
original firmware shows exactly that and refuses a record whose reserved bytes are not zero. This
extension keeps those fields and adds up to 40 more characters in reserved bytes that are covered by
the record CRC, so a name can have **71 characters**, the length of one library list row
(`kListNameCap = 0x47`). Parentheses and every printable ASCII character (0x20..0x7E) were always accepted.

## Compatibility

* A card without the extension (all reserved bytes zero) reads exactly as before.
* A record that uses the extension is refused by the original V3 firmware (it requires those bytes to be zero),
  and a card with such a record cannot be mounted by it. The writer therefore emits the extension only for
  names longer than 31 characters. Firmware from v0.2.0 of this project reads both.
* The PS1 menu stores the list text whole (up to 255 characters per row) and clips it by pixel width when it
  draws, selecting a row scrolls it like a marquee, so longer rows need no menu change.
* **Favorites are keyed by a case-insensitive FNV-1a hash of the row text.** A name that changes (because it is
  no longer cut, or because parentheses were added) is a different key: favorites saved for that game must be
  added again. Names of 31 characters or less keep their key.

## Disc record (0x100 bytes), bytes +0x94..+0xFB

| Offset | Size | Field |
|---|---|---|
| +0x94 | 4 | magic `LNX1` (`4C 4E 58 31`) |
| +0x98 | 1 | `title_ext_len`, 0..40 |
| +0x99 | 1 | `case_ext_len`, 0..40, only meaningful in the record of slot 0 of a Case, zero elsewhere |
| +0x9A | 40 | `title_ext`: characters 32..71 of the Disc title |
| +0xC2 | 40 | `case_ext`: characters 32..71 of the Case name (slot 0 record only) |
| +0xEA | 18 | zero |

Rules (a violation makes the Disc record invalid, like any other malformed field):

* Without the magic, +0x94..+0xFB must be entirely zero (unchanged rule).
* With the magic: both lengths <= 40; bytes of `title_ext` after `title_ext_len` are zero, same for `case_ext`;
  extension bytes are printable ASCII 0x20..0x7E; `case_ext_len` is zero unless `slot == 0`.
* `title_ext_len > 0` requires the 0x20 byte title field (+0x1C) to hold 31 printable characters followed by NUL,
  so the full title is `title[0..30] + title_ext`. Same for `case_ext_len > 0` and the Case name field (the Case
  that owns the record).
* The CRC at +0xFC already covers these bytes.

## Case record (0x50 bytes), byte +0x3F

* `+0x3F` = 0: unchanged. `+0x3F` = 1: the name is longer than 31 characters; the continuation is `case_ext`
  of the Disc record `ids[0]` (slot 0). Bytes +0x40..+0x4B stay zero. Any other value is invalid.
* The name field (+8, 0x20 bytes) then holds 31 printable characters followed by NUL.
* If the Disc record cannot supply a valid `case_ext` (unreadable, `case_ext_len` = 0, wrong slot), a reader shows
  the 31 character base name instead of failing.

## Display

* Library list of Cases: the Case name (base plus `case_ext` when flagged), up to 71 characters.
* Library list of the Discs of a Case: `title` (base plus `title_ext`), then ` - DISCO n` in Cases with several
  Discs, the whole row cut at 71 characters. Writers should therefore limit titles of multi-disc Cases to 61
  characters so the disc number is never cut off.

## Why not just a longer field

The record sizes, offsets and CRCs are part of the format that the Manager, the firmware and the stored
cards share; widening a field would move every later field. Using reserved bytes keeps all existing offsets.
