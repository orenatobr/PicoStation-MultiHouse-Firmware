# V3 versus upstream: deltas outside the card layer

Scope: everything in `PicoStation_MH_V3.uf2` that comes from patching or replacing code of the upstream PicoStation
(megavolt85 c48754d, `src/`, `include/`, `pio/`). Addresses are V3 addresses; RAM functions use the RAM address
(`0x200000c0..0x20008f68`, flash copy at `+0x183cc`). Nothing here was run on a console.

## 1. What V3 is, relative to upstream

* V3 is **not** a compile of the vendored c48754d source. It is an older or sibling upstream build, then
  **binary patched**. Evidence:
  * Six RAM sites hold the linker veneer pattern `ldr rN,[pc]; bx rN; .word target` in the middle of upstream code:
    `0x200000c0` (processLatchedCommand entry, displaced prologue `push {r4-r7,lr}; mov lr,r8; movs r2,#0; push {lr}`
    is re-executed by the trampoline at `0x10021624`), `0x20000304`, `0x20000cdc`, `0x20001010`, `0x20001a8c`,
    `0x20001aa0`. Targets: `0x1002f050`, `0x10028000`, `0x10030000`, RAM stubs `0x20033000`, `0x2003311c`, `0x20033100`.
  * Debug code of the base is patched out in place (`bf00` nops over `stdio_init_all`, `stdio_set_chars_available_callback`,
    `puts("Initializing...")` in initHW, `bl time_us_64` in the I2S loop, a `b.n` over the "read time" block).
    The strings "Initializing...", "RESET!", "OPENING", "CLOSING", "read time: %lluus (%d)" stay in `.rodata` but are not
    reachable (no references), "RESET2" and "x%i\n" are still called but print nothing because `stdio_init_all` never runs.
  * The base differs from c48754d in places that no patcher would touch (the custom command switch of
    `processLatchedCommand` only has 8 and 10, XBUSY polarity, hard coded `UNIROM.cue` loader at `0x20000d38`,
    function order of `load`/`makeDummyCue`). So the C++ here reproduces behaviour, not bytes, for those.
* Machine code similarity (mnemonic sequence, branch targets and literal pool ignored) of the rebuilt upstream files
  against V3 is in section 2. Ledger: `re/ledger/deltas.tsv`.

## 2. Function table

Similarity is the difflib ratio of instruction text with registers normalised (`objdump` based, not capstone: capstone
stops at the first literal pool and over reports). "V3 patch" = the difference is a hook or a stub, not C++.

| Function | V3 address | Ratio | Note |
|---|---|---|---|
| `MechCommand::processLatched` (upstream body) | `0x200000c8` | 0.92 | rest: displaced prologue and the cancel path stub `0x10028000` |
| `MechCommand::processLatchedCommand` (wrapper) | `0x1002f050` | n/a | hook chain, see 4.1 |
| `MechCommand::setBootSectorPattern` | `0x20000484` | 1.00 | |
| `MechCommand::setSens` / `updateMech` | `0x200004e0` / `0x20000504` | 1.00 / 1.00 | |
| `DiscImage::buildSector` | `0x20000588` | 1.00 | submode bytes, see 3.3 |
| `DiscImage::generateSubQ` | `0x200007c0` | 1.00 | unchanged |
| `DiscImage::unload` | `0x20000f4c` | 1.00 | unchanged |
| `DiscImage::readSector` | `0x20000fa0` | 0.59 | V3 tail after `bx` is the inline card read, see 3.3 |
| `DriveMechanics::moveToNextSector` / `setSector` / `moveSled` / `startSled` | `0x2000112c..0x200013c8` | 1.00 / 0.998 / 1.00 / 1.00 | setSector: one store removed |
| `ecc_generate`, `eccedc_init`, `eccedc_generate` | `0x200013e0..0x20001650` | 1.00 | unchanged |
| `I2S::start` | `0x20001820` | 0.43 | first 250 instructions aligned, rest is layout and stubs |
| `mech_irq_hnd`, `initPWM` | `0x20001f98`, `0x200020d4` | 1.00 | unchanged |
| `interruptHandler` | `0x20001fb0` | 0.995 | `s_doorPending` store is pseudoatomic |
| `initHW` | `0x20002148` | 0.985 | |
| `reset` | `0x200024c0` | 0.97 | |
| `resetToMenu` (new) | `0x20002760` | 0.93 | |
| `core0Entry` | `0x200029dc` | 0.82 | register allocation of the inlined `updatePlaybackSpeed` |
| `SubQ::start_subq` | `0x20002ce0` | 1.00 | unchanged |
| `ModChip::sendLicenseString` | `0x10000440` | 0.98 | V3 adds an `aux_trace` irq event after the license strings |
| `ModChip::init`, `core1Entry`, `main` | see 3.5 | | |

## 3. Changes, per upstream file

### 3.1 `src/cmd.cpp`, `include/cmd.h`

* `processLatchedCommand` is now a wrapper: it reads `m_latched`, runs the upstream body (`processLatched`, V3 RAM
  `0x200000c8`) and calls `mh::hooks::on_mech_command(*this, raw)`. V3: `0x200000c0 -> 0x1002f050`.
* XBUSY polarity (`SENS::XBUSY`, index 4): CANCEL (`ASEQ_CMD_CANCEL`) now sets true (V3 stub `0x10028000`: store 1,
  `OUT_SET` of the SENS pin when it is the selected sens, then the common `i2s_set_state(1)` tail at `0x20000316`);
  FOCUS_ON sets false (then the 15 ms alarm, `0x3a98` us, unchanged, calls `resetXBUSY`); CLV modes STOP/BRAKE/KICK
  unchanged, `CLVS/CLVH/CLVP/CLVA` set true (V3 `0x20000350`).
* `MECH_CMD_CUSTOM`: only `g_fileArg = arg` remains, then `COMMAND_EXTENDED` (arg>>1 == 1 skip_bootsector, == 2 skip_edc, bit0 is
  the value; `EXTENDED_GET_CFG` is gone) and `COMMAND_BOOTLOADER` (arg 0xBEEF, `rom_reset_usb_boot_extra(25,0,0)`). The
  file browser commands (GOTO_ROOT, GOTO_PARENT, GOTO_DIRECTORY, GET_NEXT_CONTENTS, MOUNT_FILE, GET_COVER) do nothing
  in the body. `needFileCheckAction` and `listReadyState` are no longer written here.
* New `MechCommand::resetState()` = V3 `FUN_20002f9a` for this file: members to power-on values (sens table
  `{1,1,1,1,1,0,1,1,1,1,0,0,0,1,0,1}`, `m_currentSens` 0, SOCT off), statics `sled_break`, `dir`, `prev_dir` 0,
  `setBootSectorPattern`'s last value -1 and the command being processed (`0x2000cb88`) 0.
  `s_command` became a class static so it can be cleared (a function static in V3).

### 3.2 `src/drive_mechanics.cpp`

* `setSector`: the upstream `m_i2s.i2s_set_state(0)` at the top is gone (V3 `0x20001174` is a nop). A seek no longer
  stops the I2S stream. Everything else identical.

### 3.3 `src/disc_image.cpp`, `include/disc_image.h`

* Layout: `DiscImage` is `0x147C` bytes, a new `bool m_cardMode` at `+0x147A` after `skip_edc` (`+0x1479`). The card layer
  `CardDisc` (`mh/card.h`) has the same layout; `static_assert(sizeof(DiscImage) == 0x147C)`.
* `buildSector`: first sub-header copy (`header[18]`) is `0x28` (Form | Data) for every sector, the second copy
  (`header[22]`) is set to `0x28` in the pre-gap only (upstream: 0x08 pre-gap both, 0x20 otherwise).
* `readSector` (`0x20000fa0`), exact order: (1) location != `SDCard` returns (the RAM location and `readSectorRAM` /
  the boot sector injection from `loaderImage` are gone, `s_dataLocation` starts as `SDCard`); (2) `sector - 150 < 0`:
  `tracks[1]` data -> `buildSector(.., pregap=true)`, else zero fill 4704 bytes; (3) `m_cardMode` set:
  `mh::hooks::card_read_sector` and return; (4) track walk. In (4) the first track whose end is past the sector owns
  it, a track without an open file is served as pre-gap, the seek is `f_lseek(file, (u32)((adj - fileOffset) * 2352))`
  without a sign test, a failed seek does `f_rewind`, `skip_edc || type != DATA` uses `f_read_scramble`, otherwise
  `f_read` + `eccedc_generate` + `scramble_data`. No result checks, no "readed %llu" message.
  The V3 inline continuation of the card path is at `0x20001018..0x2000105a` (not mine, package A): block =
  `(sector - 150) * 5 + raw_base (word 0x200087f0)`, 5 blocks into `0x2000a39c` with `sd_readblocks` (RAM `0x20003414`),
  copy 0x930 bytes to `0x20009a6c`, when the sector begins with the sync pattern `00 FF*10 00` and its mode byte (+15)
  is 1 or 2 the scrambler table is passed to `scramble_data`, otherwise a null table.
* `makeDummyCue()` now returns `bool` and calls `mh::hooks::mount_default_disc(*this)` (V3 veneer `0x20000cdc` ->
  `FUN_10030000`: legacy/V2 mount chain `FUN_100273c4`, on failure the built-in menu; returns true except for a null
  disc). The upstream body survives as `buildPlaceholderCue()` and is the weak default of that hook.
* `load()` is unchanged. V3 contains an older `load` (reads a raw header from SD sector 2 and, failing that, parses a hard
  coded `UNIROM.cue`) at `0x20000ce6`/`0x20000d38` that nothing calls any more; it was not reproduced.
* `loaderImage` / `loaderImageSize` are no longer referenced. `CMakeLists.txt` (integrator) should drop
  `addBinaryFileWithSize(... loaderImage ...)`.

### 3.4 `src/i2s.cpp`, `include/i2s.h`

* `I2S` flags `menu_active` (+4), `s_doorPending` (+5) and the new `s_resetPending` (+6) are `pseudoatomic<bool>` (V3
  accesses them under the pseudoatomic spinlock; layout `+0x88 lastSector`, `+0x8c i2s_state`, `+0x90 m_sectorSending`).
* `start()`: the three browser globals are still defined and initialised (`needFileCheckAction = 0`, `listReadyState = 1`,
  `g_entryOffset = 0`) but nothing reads them. `menu_active = false`, `s_doorPending = false` (upstream: menu true).
  After `modChip.init()`: `if (g_discImage.makeDummyCue()) { loadedImageIndex = 0; img_count = 1; reinitI2S();
  g_driveMechanics.resetDrive(); g_discImage.set_skip_bootsector(true); mechCommand.resetBootSectorPattern(); }`
  (V3 `0x20001e06`). The `DirectoryListing::init()/getDirectoryEntries(0)` calls are gone.
* Main loop: after `sendLicenseString`, V3 test-and-clears `s_doorPending` (stub `0x2003311c`), calls the legacy disc
  rotation `mh::hooks::on_door()` when it was set (`FUN_1002773c`), then `mh::hooks::poll()` (`FUN_1002f190` ->
  `FUN_1002f0b0`) every iteration. The whole `needFileCheckAction` state machine, the PROCESS_FILES/cover/cfg sector
  generators (sectors 4750, 4850..4857, 4800) and the old door handler (`++loadedImageIndex`, `DirectoryListing::getPath`,
  `load`) are gone from the live path. The sector cache, DMA start and LRCK sync are identical.
* The "read time" instrumentation: V3 skips it with a branch, so it is not reproduced (`DEBUG_I2S` stays 0).

### 3.5 `src/picostation.cpp`, `src/main.cpp`, `include/picostation.h`

* `initHW`: starts with `sleep_ms(1250)` (the debug block of the base, with the stdio calls patched out; the delay
  remained); no `Initializing...`/`ON!`. After `SD_CS`: `gpio_init(LED); gpio_put(LED,1); gpio_set_dir(LED,OUT)` (GPIO25 on).
  The `i2c_init`, SDA/SCL pin setup and `si5351_*` calls are gone from `initHW`: the same sequence runs earlier, from
  `main` (see below). Everything else identical.
* `main` (`0x10000394`): after `initPseudoAtomics` V3 calls `FUN_10024564` (aux_trace init `FUN_10024044`, which chains to
  `eccedc_init`, then clears 24 bytes at `0x20033800`), then `FUN_100252dc` (Si5351 table `aux_clock`, then `initHW`),
  then `multicore_launch_core1` and `core0Entry`. Here: `mh::hooks::boot_trace(); eccedc_init(); mh::hooks::boot_clock();
  initHW();`. `boot_clock` has the upstream I2C/Si5351 sequence as weak default.
* `core0Entry` (`0x200029dc`): extra test before `getSector()`: `if (m_i2s.s_resetPending) resetToMenu();`.
* `reset()` (`0x200024c0`): the first statement (where upstream prints "RESET!") is `resetRuntimeState()` (`FUN_20002f9a`);
  a long press (`s_resetPending == 2`) only does `set_skip_bootsector(true); set_skip_edc(false); resetBootSectorPattern()`
  (no unload/makeDummyCue/menu/`gotoRoot`); `gpio_put(SCOR,0)` is gone (SQSO only); at the end `resetRuntimeState();
  gpio_put(SENS,1)` (`FUN_20002ff2`), then the MECHACON state machine is enabled and `s_resetPending = 0`.
* `resetToMenu()` (`0x20002760`, new; calls "RESET2"): same teardown as `reset()` up to `updatePlaybackSpeed`, then
  `unload(); makeDummyCue(); skip_bootsector=1; skip_edc=0; resetBootSectorPattern();` mechacon init, `g_subqDelay=false`,
  SOCT off, SQSO low, `resetDrive`, `reinitI2S`, `skip_bootsector=0`, `resetBootSectorPattern`, the two 30 ms waits, then
  `s_resetPending = 0` and `I2S::s_resetPending = false`. It does **not** enable MECHACON again and does **not**
  re-arm the reset interrupt. Nothing found in V3 sets `I2S+6`; the setter is outside upstream code (card handler).
* `resetRuntimeState()` (`FUN_20002f9a`): `MechCommand::resetState()`, `g_subqDelay = false`, `s_doorPending = false`,
  `s_resetPending = false`, `i2s_state = 0`, then `mh::hooks::reset_card_state()` (card side: raw base word
  `0x200087f0 = 2`, word `0x20009a68 = 0`; also zeroed by the original: word `0x20031798`, not identified).
* `updatePlaybackSpeed` still prints "x%i" in V3 (inlined, live `printf`), output goes nowhere. Not reproduced.
* `interruptHandler`: RESET and XLAT unchanged; DOOR sets `m_i2s.s_doorPending` (pseudoatomic). No "OPENING"/"CLOSING".
* Unchanged and not touched: `SubQ::start_subq`, `edc.c`, `modchip.cpp`, `pio/main.pio`, `boards/*`, `include/values.h`.

### 3.6 `src/directory_listing.cpp`, `include/directory_listing.h`, `include/listingBuilder.h`

V3 contains none of the `DirectoryListing` functions (no `config.ini`/cover/`readCfg`, no filelist builder): the closest
V3 functions score under 0.5 and are the card layer. The `.cpp` is an empty unit (the file list in `CMakeLists.txt` is
unchanged), both headers were deleted. `FileListingStates` and `g_fileListingState` stay in `picostation.h/.cpp`
(V3 still has the symbols).

## 4. Hook table (`include/mh/hooks.h`)

All have weak defaults in `src/main.cpp`; the integration provides the strong ones. Signatures are final.

| Hook | V3 | Where it is called | What the original does |
|---|---|---|---|
| `bool mount_default_disc(DiscImage&)` | `FUN_10030000` via `0x20000cdc` | `DiscImage::makeDummyCue` | `FUN_100273c4(disc)` (V2 header A/B or legacy v1 mount, copies the RAM stubs: `0x20033100` from flash `0x1002a000..0x1002a038`, `0x20033000` from `0x10021640..0x100216d8`); on failure builds the fallback disc (tracks `0x5D` sectors, card mode on, raw base 0, state=1, `menu_sectors=0`, c_sectorMax `0x24b1`); returns false only for null `disc` |
| `void card_read_sector(DiscImage&, void* buf, int sector, const uint16_t* scr)` | stub `0x20033000` | `DiscImage::readSector` (3) | see 3.3; the call replaces the rest of `readSector` |
| `void on_mech_command(MechCommand&, uint32_t raw)` | `FUN_1002f050` | `processLatchedCommand` after the body | `FUN_10024090` logs timing in the aux_trace rings and runs the body; then, when `(raw & 0xFF0000) == 0xF90000` and the card state is armed (`0x20033404 == 0x39313334`, `0x20033400 == 2`, byte `0x20033598` is 2 or 4 (`FUN_1002f018`), `0x20033440 == 0`, the 16 bit word at `0x20033594` (`FUN_1002f000`) is non zero and equals `raw & 0x7FFF`): `0x20033444 = raw & 0xFFFF`, then `0x20033440 = 12` (command pending) |
| `void on_door()` | `FUN_1002773c` via `0x20033100` | `I2S::start` loop | legacy disc rotation (`aux_legacy::legacy_swap_v1/v2`) when the legacy state `0x20033900 == "SW62"` |
| `void poll()` | `FUN_1002f190 -> FUN_1002f0b0` | every `I2S::start` iteration | when `0x20033404 == 0x39313334 && 0x20033440 == 12`: `reset_response_cache` (`0x100217b0`); `x = 0x20033444`; the value stays `x & 0xFFFF` unless `FUN_1002f018` is true and `(x & 0x7FFF) == FUN_1002f000()` and bit 15 of `x` is set, then bit 31 is added; store it back, `0x20033440 = 0` |
| `void reset_card_state()` | part of `FUN_20002f9a` | `resetRuntimeState` | `0x200087f0 = 2`, `0x20009a68 = 0` |
| `void boot_trace()` | `FUN_10024564` | `main` | `aux::trace_init` + zero `0x20033800..0x20033818` |
| `void boot_clock()` | `FUN_100252dc` | `main` | `aux::clock_program`; V3 then calls `initHW` (done by `main` here) |

`on_mech_command` runs in interrupt context (XLAT edge): keep it short and in RAM if it touches flash-only code
paths used elsewhere. `card_read_sector` runs on core 1.

## 5. Not done, unknown, or deliberately different

* `setBootSectorPattern`, `processLatched`: the V3 base differs in places that only exist in the base binary (see 3.1); the
  C++ reproduces behaviour.
* V3 `0x2000cb88` is the file scope `command` of the body; `0x20031798` is cleared by `FUN_20002f9a` and not identified.
* `I2S+6` writer unknown (section 3.5). `FUN_20002ff2` also clears nothing else than what `resetRuntimeState` does.
* `ModChip::sendLicenseString`: V3 inserts an `aux_trace` irq event (`FUN_10024188`, args timer, `0xff`) after the license
  strings; not reproduced here (package C owns the trace module).
* `ModChip::init`, `core1Entry`, `SubQ`, `edc.c`, FatFs, SD driver, unwind, libgcc: V3 equals upstream (1.00 apart from
  toolchain drift below).
* Debug strings: see section 1; `logging.h` is unchanged on purpose (all flags 0).
* Build, for the integrator: `loaderImage` no longer referenced; `hardware_i2c` still needed by the weak `boot_clock`;
  V3 links `pico_stdio_usb` descriptors ("Raspberry Pi", "Board CDC", "USB stdin / stdout") but never calls
  `stdio_init_all`.

## 6. SDK and toolchain drift (not chased)

These differ only because V3 was built with another pico-sdk / gcc and are not source level changes.

* Clock and PLL: `set_sys_clock_pll` (`0x10000844`/`0x10014d30` region), `check_sys_clock_khz`, `clock_configure*`,
  `xosc_init`, `vreg_set_voltage`.
* Runtime init: `runtime_init*` (`post_clock_resets`, `per_core_irq_priorities`, `usb_power_down`, `mutex`), `crt0` tables,
  `_sbrk`, `__wrap_malloc/calloc/realloc/free`, `sysconf`, `getenv`/`_findenv_r`.
* Time and sync: `time_us_64` (0.89), `busy_wait_us`, `busy_wait_until`, `sleep_ms`, `tick_start`, `critical_section_init`,
  `spin_locks_reset`, `next_striped_spin_lock_num`, `hw_claim_*`, `dma_claim_unused_channel`.
* GPIO / PIO / peripherals: `gpio_set_function`, `gpio_set_outover`, `gpio_set_slew_rate`, `irq_add_shared_handler`,
  `irq_is_enabled`, `pio_sm_init` (0.31), `uart_set_fifo_enabled`.
* stdio and newlib: `stdio_get_until`, `stdio_putchar`, `stdio_out_chars_no_crlf`, `stdio_exit_handler`, `_write`, `_read`,
  `_fflush_r`, `__swsetup_r`, `fputc`, `__sinit`, `_vsnprintf` (0.86), `_out_char`.
* C++ ABI / libgcc: exception unwinder (`__gnu_unwind_*`, `unwind_phase2`, `__cxa_*`), demangler `d_*`, `_ZSt10unexpectedv`,
  `___aeabi_ldiv0_veneer`.
* FatFs build options: `dir_sdi` (0.24), `f_mount` (0.44), `load_xdir` (0.32), `ff_uni2oem`.
* Fuzzy matches below 0.5 against small functions (`File_schedule_close`, `abort`, `strtoul`, `__cxa_throw`, `posix_size`)
  are noise of the matcher (30 byte veneers and stubs), not drift.
