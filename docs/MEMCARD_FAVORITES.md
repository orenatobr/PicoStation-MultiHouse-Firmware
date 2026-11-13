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

## The fixes that were made (Manager 0.5.1 and 0.5.2)

The one limit found that depends on how fast a device answers: the detection routine (menu text address `0x800d480c`)
waited about 180 microseconds for the ACK after each byte (budget 0x78), while the frame read and write routines wait
about 2.4 ms (2000). A device that acknowledges slower than 180 us passed the transfers but was reported as "no Memory
Card found". `addiu $a0, $zero, 0x78` at `0x800d48b4` and `0x800d48fc` became `0x7d0`; EDC and ECC of that raw sector
were rebuilt (`tools/patch_menu.py` in the Manager repository). Fast devices are unaffected. The Manager repository has `tools/sim_menu_memcard.py`, which runs the real MIPS code of the
original and the patched menu in an emulator against a modelled card: a card whose first ACK takes 150 us to 1 ms is found
only by the patched menu, a normal card is found by both, and frame read and write stay correct. This is still a hypothesis fix:
it was not tested on any device, and it cannot be ruled out that the real problem is elsewhere.

Manager 0.5.2 adds a second fix found with the same simulator (`--sweep`, device profiles): the menu sends the first byte
only 0x96 (detection), 0x64 (frame read) or 0x50 (frame write) units, about 1 microsecond each, after selecting the card.
A device that is not listening yet loses the byte: modelled, a device that needs 200 microseconds broke detection, read and
write, and 100 microseconds already broke writes. The three settle delays are now 1000 units (about 1 ms); they are minimum
waits, so they only cost about 0.9 ms per operation. A device that needs more than about 1 ms still fails.

## What the protocol documentation says (checked against the fixes)

From the PSX protocol notes (https://kraptor.github.io/psx-docs/controllersandmemorycards, after Martin Korth's NO$PSX docs):

* A device must pull /ACK low within **100 microseconds** of the last clock pulse (the kernel times out after that) and
  should not reply before it has seen the address byte `81h`. A device that follows the protocol therefore already passed
  the original 180 microsecond detection budget; **the two fixes only help devices that are slower than the protocol allows**.
* Original Sony cards add a pause of about 31000 CPU cycles (about 0.9 ms) before the /ACK that follows the 7th byte of a read.
  The frame routines wait up to 2.4 ms for each ACK, so they already tolerate that; the detection routine now does too.
* The FLAG byte starts as `08h` ("directory not read") and is cleared by a write; the menu ignores it, which is harmless.
* PS2 cards do not fit a PSX slot and have a different directory structure, which matches the finding that the menu only
  speaks the PS1 frame protocol.

Decision taken under these assumptions: the device behind "ps2memcard" is a PS1-protocol emulator or adapter that is not
fully protocol conformant in its timing; the timing fixes stay, and nothing else in the frame protocol is changed because
the menu already matches the documented sequences byte for byte (the simulator runs its real read and write routines
against a card built from this specification).

## The device behind "ps2memcard" (BitFunx PSxMemCard Gen2) and the most likely cause

The device the issue was reported with is the BitFunx PSxMemCard Gen2 (https://www.bitfunxshop.com/products/bitfunx-psxmemcard-gen2-for-ps1-psone-and-ps2-games-memory-cards).
It is an SD2PSX based emulator (same family as the MemCard PRO): it presents virtual cards stored on a microSD card, 128 KB
cards for the PS1 and larger ones in PS2 mode; the name "PS2 memory card" comes from its PS2 mode, not from the protocol the
menu speaks. It is protocol conformant, so the timing fixes of Manager 0.5.1 to 0.5.3 are unlikely to be what matters for it.

What matters is the **Game ID**. This family switches to a different virtual card when it receives a Game ID
(`81 21 00 len+1 id... 00`), which the menu sends when it launches a game (the commands `81 53` and `81 20`, answered with
`27 FF`, detect the family first). These devices do not ACK the Game ID bytes, which is why the menu sends them without
waiting. The menu never sends an id for itself, so the card stays on the channel of the last game that was started (or
the channel selected on the device), and the settings and favorites file `BASCES_31337PICO_CFG` is saved to and loaded
from that channel. After a different game has been played the next menu session reads another virtual card and the
favorites seem to be gone; with an original card there is only one card, so it always works.
This is a hypothesis from the protocol and from the device documentation, not a verified fact.

Workarounds that need no change in the menu:

* Turn off the Game ID based card switching for PS1 in the device settings (SD2PSX / sd2psXtd: Game ID off), or use a
  fixed channel for the console, so that the device behaves like one card. Favorites then persist.
* Or keep the favorites on a channel that the device selects for the menu (for sd2psXtd the BootCard mechanism).

**Update (Manager 0.5.4): the menu change was made.** The embedded menu now sends its own Game ID (`SCES-31337`, from its
boot file `SCES_313.37`) to every slot that answers `81 20` with `27 FF`, before the first card detection of the save and load
state machine, waits about 1.5 s (Manager 0.5.5, was 0.8 s) for the device to switch card and continues into the original detection. A 564 byte routine
replaces an unreferenced CD-ROM helper of the menu (`0x800d42e8`) and one `jal` (`0x800d8db0`) is redirected to it; the code is
`tools/menu_gameid_asm.py` in the Manager repository. It was validated by running the real menu code against a model of
the device family in an emulator (virtual card per id, reconnect window, no ACK for the id): without the routine the device
stays on the last game's card and the favorites are not read, with it the device ends on `SCES-31337` and the favorites are read;
an original card receives no id. Settings saved earlier on a game's card must be saved once again. Not verified on the
BitFunx itself. The firmware UF2 is unchanged by this.

**What the SD2PSX source says** (ogg1e/Sd2psx-firmware, `ps1_odeman.c`, `ps1_cardman.c`; the BitFunx firmware may differ): a Game ID
(`81 21 00 <len> <id>`) makes the device close the card, wait 250 ms, open or create the `.mcd` and load it from the SD card.
An id found in its game database selects a card of its own; any other id selects the numbered card `Card1`, channel 1. So
`SCES-31337` should land on the ordinary default card, which is stable. That is why the wait was raised to 1.5 s: loading
128 KB from a slow SD card can take longer than the 250 ms delay.

**Firmware 0.3.0: the same fix from the UF2.** The Pico cannot talk to the Memory Card port, but it serves the raw sectors of
the menu program to the PS1, so it can serve the corrected ones. `src/mh/menu_overlay.cpp` replaces the five menu sectors
(33, 34, 35, 42 and 54 of the 93) that the Manager 0.5.5 patch changes when, and only when, the sector read from the SD card
is byte for byte the original (FNV-1a over the 588 words, then a copy of the patched sector). A menu that is already
patched by the Manager, or any other menu, passes through unchanged. The table is generated by `tools/gen_menu_overlay.py`
from the Manager's menu blob before and after `tools/patch_menu.py`. The check runs only for those five sectors while the
menu phase is active, so game reads are not touched. The result is the same as writing the SD with Manager 0.5.5: the
menu sends `SCES-31337` and waits about 1.5 s before looking for its settings and favorites. Host tests: `tests/test_menu_overlay.cpp`
and the `menu93` scenario of `tests/test_integration.cpp` (run with `MH_MENU_ORIG` and `MH_MENU_PATCHED` set to the blobs).
Not verified on a console or on the BitFunx.

**Where the menu code lives:** the menu program is stored on the SD card (written by the Manager) and the firmware serves its
sectors; the small emergency menu inside the UF2 (shown when the SD card is missing or invalid) has no favorites and no
Memory Card code.

## What is needed to go further

* Which device, which port, and which message the menu shows (`Nenhum Memory Card foi encontrado`, read error, write
  error, `sem espaco livre`) or whether favorites silently do not persist.
* Ideally a logic analyzer capture of one `81 52` read from the device.

Only the bounded wait above was changed: two constants that can only make the detection more patient. The frame
protocol, the file layout and the write path were left alone because a blind change there would affect every user of
original cards and cannot be tested here.

## Provenance of the patched menu sectors

The five menu sectors that the firmware overlays (33, 34, 35, 42 and 54 of the 93 sector menu) are generated, not hand written:

* The routine itself is built by `tools/menu_gameid_asm.py` and applied to the menu blob by `tools/patch_menu.py` in the Manager repository,
  <https://github.com/orenatobr/PicoStation-MultiHouse-macOS> (tests in `core/tests/test_menu.c`, device model in `tools/sim_menu_memcard.py`).
* `tools/gen_menu_overlay.py` in this repository turns the original and the patched menu blobs into `src/mh/menu_overlay_data.cpp`.
* Both repositories pin the same FNV-1a hashes of the five patched sectors (`tests/test_menu_overlay.cpp` here, `core/tests/test_menu.c` there), so the two code paths cannot drift apart without a CI failure.
