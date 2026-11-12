# Favorites and the Memory Card (analysis of the PS1 menu)

Status: **Nothing in the firmware or in this repository stores favorites.** One compatibility fix was made in the
menu program itself and ships with Manager 0.5.1 (see "The fix that was made"); the real cause of a failure with a
specific device is still unconfirmed. This document records what the menu does, so the cause can be found with a
device in hand.

## Where the code lives

Favorites and the menu settings are handled by the PS1 menu program (a PS-X EXE, 93 sectors, written to the card at
`menu_lba` by the Manager from `picostation_menu.bin`). The Pico only serves the sectors of that program. This
repository rebuilds the Pico firmware; the menu program is not part of it (the built-in fallback menu in
`binary/picostation-menu.bin` is a different, smaller program). Changing how favorites are saved therefore means
changing the menu binary (MIPS) and shipping it through the Manager, not the UF2.

## What the menu does (from the Ghidra decompile of `picostation_menu.bin`, text at 0x800d0000)

* It talks to the Memory Card slot **directly through SIO0** (`0x1F801040..`), without the BIOS card functions
  (only four BIOS stubs exist in the program and none belongs to the card API).
* Detection: for slot 0 then slot 1 it sends `81 52` and accepts the slot if at least one byte is acknowledged.
* Read frame (128 bytes): `81 52 00 00 MSB LSB 00 00 00 00`, 128 data bytes, then 10 more bytes where byte 1 must be
  `0x47` ("G", good). Write frame: `81 57 00 00 MSB LSB`, 128 data bytes, then 8 bytes where byte 3 must be `0x47`.
  Both follow the textbook protocol (replies delayed by one byte).
* Waits for the byte to arrive are bounded (about 2000 polls of the SIO status, then fail); after a write it waits
  about 30000 short delays before the next command.
* The save is one standard PS1 file in the card file system: a directory frame (state `0x51`, size `0x2000`, name
  `BASCES_31337PICO_CFG`, 20 bytes compared) and one 8 KB block of 64 frames. Load scans directory frames 1..15 for
  that name and for states `0x51..0x53`; save picks a free block (`0xA0`, deleted `0xA1..0xA3`, or an all-zero frame
  as a last resort), writes the 64 frames one by one with a **read-back compare** (up to 7 tries per frame) and then the
  directory frame (up to 9 tries). Errors shown: no card found, read error, write error, no free space.
* The block holds the settings (theme, language, layout, start in favorites, music, volume) and **up to 32 favorites**
  as 32 bit hashes plus an FNV checksum of the list.
* A favorite is identified by the **FNV-1a hash of the row text** of the library entry (flag byte first, then the text
  with ASCII lower case folded to upper case). The name is not stored.
* When a card answers `0x81 0x20` with `0x27 0xFF` (the MemCard PRO / SD2PSX family) the menu also sends the game id
  with command `0x81 0x21` before launching a game.

## What this means

1. **Longer or reformatted names change the key.** A favorite saved while a game was listed as
   `ALUNDRA USA REV 1` does not match `Alundra (USA) (Rev 1)`. After the long names change (and after Manager 0.4.5)
   favorites of games whose list text changed must be added again.
2. **Nothing in the code restricts the device type**: no ID check, no capacity check, no PS1/PS2 distinction. A
   device that implements the PS1 read/write frames correctly should work. alberonisse/PicoMemcard, whose state
   machine was read for this analysis, replies exactly like the frames above.
3. The most likely reasons a non-original device fails are therefore electrical/timing (bounded waits, the slow first
   ACK of some devices, 3.3 V parts), a device that is in a per-game "channel" mode and switches image after the game
   id command, or a PS2 only card that does not answer PS1 commands at all. None of them can be confirmed from the
   binary.

## The fix that was made (Manager 0.5.1)

The one limit found that depends on how fast a device answers: the detection routine (menu text address `0x800d480c`)
waited about 180 microseconds for the ACK after each byte (budget 0x78), while the frame read and write routines wait
about 3 ms (2000). A device that acknowledges slower than 180 us passed the transfers but was reported as "no Memory
Card found". `addiu $a0, $zero, 0x78` at `0x800d48b4` and `0x800d48fc` became `0x7d0`; EDC and ECC of that raw sector
were rebuilt (`tools/patch_menu.py` in the Manager repository). Fast devices are unaffected. This is a hypothesis fix:
it was not tested on any device, and it cannot be ruled out that the real problem is elsewhere.

## What is needed to go further

* Which device, which port, and which message the menu shows (`Nenhum Memory Card foi encontrado`, read error, write
  error, `sem espaco livre`) or whether favorites silently do not persist.
* Ideally a logic analyzer capture of one `81 52` read from the device.

Only the bounded wait above was changed: two constants that can only make the detection more patient. The frame
protocol, the file layout and the write path were left alone because a blind change there would affect every user of
original cards and cannot be tested here.
