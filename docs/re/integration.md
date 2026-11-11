# Integration (work package E)

Sources: `src/mh/hooks_impl.cpp` (host safe glue), `src/mh/hooks_pico.cpp` (device bindings), `include/mh/hooks_hw.h`
(hardware operation table), test `tests/test_integration.cpp`, ledger `re/ledger/integration.tsv`. Nothing here was run on a
console or on any RP2040: only the host tests and the cross build were run.

## 1. What is wired

Every function of `include/mh/hooks.h` has a strong definition, so the weak defaults in `src/main.cpp` were removed (main.cpp now
only calls the hooks). The firmware links the card, handler and aux layers (checked with `arm-none-eabi-nm`, see section 5).

| Hook | V3 | Implementation |
|---|---|---|
| `mount_default_disc` | `FUN_10030000` (veneer `0x20000cdc`) | `card::set_sd` with the SD driver, then `card::mount` (V2 header A/B or PSXRAW v1 single disc), else `aux::legacy_v1_mount` (PSXRAW v1 multi disc, table built in a scratch `DiscTable`, copied into the `DiscImage`), else the fallback menu (`aux::fallback_table`, state phase 1, `menu_sectors` 0). The disc table is the `DiscImage` itself (`CardDisc` has the same layout, `static_assert`s in `hooks_pico.cpp` and `disc_image.h`). Then card mode flag (`DiscImage +0x147A`, already set by the table builders and set again through `Hw::set_card_mode`), `c_sectorMax = card::globals().total_sectors`, `handler::init(disc)`. Always returns true. |
| `card_read_sector` | stub `0x20033000` | `card::sector_route(lba)`. Route `kCard` (menu sectors below `0xF3`, game data, everything in phase 2 except `0x41A`): 5 blocks at `card::sector_block(raw_base, lba)` into the block buffer, `scramble_data` with the scrambling table only when the sector starts with `00 FF*10 00` and byte 15 is 1 or 2, else without table. Routes `kBuiltinThenCard` / `kBuiltin`: chain in V3 order: built-in menu (`aux::fallback_serves` / `fallback_convert` from `loaderImage`), config and music server (`aux::cfg_music_serve`), handler (`handler::read_sector`: DEAD, N=900 refusal, v6 cheats, main); a response is wrapped with `DiscImage::buildSector(sector, buf, response, table)` exactly as the original sends the 0x914 byte response buffer. Unanswered `kBuiltinThenCard` falls to the raw read, unanswered `kBuiltin` gives the scrambled zero page. A failed SD read produces a header only sector (`buildSector` with null data). |
| `on_mech_command` | `FUN_1002f050` + `FUN_10024090/10024588` | trace begin/end (`aux_trace` ring), `aux::cfg_command_word` (music flag), `handler::on_pad_command`, `handler::on_confirm_command`, on the 24 bit latched word. |
| `on_door` | `FUN_1002773c` | `aux::legacy_swap_v1` (mode 1: PSXRAW v1 multi disc) or `aux::legacy_swap_v2` (mode 2: V2 card, only in phase 3, the game running). A successful swap installs the new table, raw base, cursor 0, total sectors and drops the response cache. |
| `poll` | `FUN_1002f190 -> FUN_1002f0b0` | `handler::finish_confirm()`, then `c_sectorMax = card::globals().total_sectors` (the original shares one word between the card layer and the sector limit; a disc switch moves it). |
| `reset_card_state` | part of `FUN_20002f9a` | `raw_base = 2`, `sector_cursor = 0` (literal, see risks). |
| `boot_trace` | `FUN_10024564` | `aux::trace_init`, `aux::cfg_reset` (the 24 bytes at `0x20033800`). |
| `boot_clock` | `FUN_100252dc` | I2C0 bring-up (`hooks_pico.cpp`: `i2c_init` 400 kHz, GPIO12/13 pull-ups), then `aux::clock_program` with a bus on `i2c_write_timeout_us` to address `0x60`. The upstream computed Si5351 code is no longer used by the firmware. |

The glue is `hooks_impl.cpp` for everything that does not need a pico header. `hooks_pico.cpp` binds `mh::hooks::Hw` from a
static constructor (runs before `main()`): `sd_init`/`sd_read_blocks` (SD driver), `DiscImage::buildSector`, `scramble_data`,
`c_sectorMax`, `DiscImage::set_card_mode`, `time_us_32`, the I2C operations and the embedded menu (`loaderImage`,
`loaderImageSize`; the `addBinaryFileWithSize` line in `CMakeLists.txt` is kept).

State shared between hooks (scratch `DiscTable`, `CfgState`, `TraceBlock`, `LegacyV1`) is static (BSS, 0x1488 + about 3.7 KB +
small); the response, block and text buffers are the ones of `card_util.cpp`. No heap, no printf in the hot path.

Placement: `card_read_sector`, `on_mech_command`, `poll` and the helpers they use directly (`serve_chain`, the SD adapters,
`block_read`) are `__time_critical_func` / `__not_in_flash_func` (RAM, addresses `0x2000xxxx` in the ELF). The handler, aux and
card validators called from there stay in flash as in the original (`FUN_100228c8`, `FUN_100245e0` are flash functions in V3);
a flash miss stalls core 1 only, never the SD driver.

Order of the card read (differences to the V3 chain are in section 4): the V3 chain order is built-in menu, DEAD, N=900
refusal, v6 cheats, config/music, main handler. Here: built-in menu, config/music, then the handler's own chain
(DEAD, refusal, v6, main). The windows differ (LBA 300..555 for the music server versus `0xFA`, `0x41A`, `0x1C2..0x3C1`), the only overlap
is the music window with the cheat window while the music flag is set, in which V3 would try the v6 path first (it steps aside
when the music flag is set, `Env::hold`, which `serve_chain` sets from `CfgState::music_enabled`).

## 2. Build

`tools/build.sh` with `BUILD_DIR=$HOME/.cache/picostation-fw/build-E`: no warnings from the files of this package (recompiled
with a clean object), no warnings from the others.

| | V3 original | this build |
|---|---|---|
| UF2 | 1036 blocks (265216 bytes of image) | 745 blocks, image 190720 bytes (`0x10000000..0x1002e900`) |
| UF2 family id | `0xe48bff56` | `0xe48bff56` (all 745 blocks) |
| Base address | `0x10000000` | `0x10000000` |
| text / data / bss (ELF) | n/a | 190576 / 0 (the `.data` image is part of the flash copy) / 205208 |

The 74.5 KB difference is mostly TinyUSB: the original links `pico_stdio_usb` (descriptors "Raspberry Pi", "Board CDC", "USB stdin / stdout",
deltas.md section 5) although it never calls `stdio_init_all`; `CMakeLists.txt` has `pico_enable_stdio_usb(... 0)`, so this build does not
carry them. The RAM stubs of V3 (flash `0x1002a000`, `0x10021640..`) and the card layer's patched copies do not exist as such here.
`.time_critical` functions are copied to RAM at start-up by the SDK crt0 (`.data` section at `0x200000c0`, load address `0x10028a68`).

## 3. Tests

`make -C tests`: all green, 4515 checks (aux_cfg 588, aux_legacy 1371, aux_misc 60, card 1617, handler_cheat 155, handler 620,
integration 104). `tests/test_integration.cpp` runs the real `hooks_impl.cpp` against fake hardware (`Hw`): images from
`card_image.py` and `legacy_image.py`.

* V2 card: `mount_default_disc` fills the state (phase 1, valid, `sd_menu`, one data track of 0x385 sectors, raw base `menu_lba`,
  total/`c_sectorMax` `0x385 + 2 * 0x122A`). Menu sector reads (`0x96`, `0x97`) return the card bytes at `menu_lba + 5 * n`, scrambled only
  for the sector that carries the sync pattern and mode 2. The library listing (`0xFA`) is compared byte by byte with
  the layout (names, flag 1, blanks for non ASCII, 32 character name, trailer `[0][0][0][5]`); `0x12C` returns `CFG1` and the text
  of the card; the music flag (`0xF80008` through `on_mech_command`) moves `0x12C` to the music server and any other F-class word clears it; disc
  info (`PGM1`) and the switch request (`PLS1` event 2, phase 2, raw base `raw_lba`, `poll` moves the sector limit to `3 + 2 * 0x122A`), then game sectors
  from the disc package (the generator packs packages 0x100 blocks apart, so sector 0 of disc 0 is overwritten by the next track map and is
  checked as such); `0xFA` is no longer answered in the game phase; `reset_card_state` sets raw base 2 and cursor 0.
* SD read failure: header only sector. No layout / SD init failure: fallback menu (phase 1, `menu_sectors` 0, total `0x24B1`), sector 0
  equals `image ^ scrambler`, a sector past the 29 embedded ones is the scrambled zero page, the listing says `ERRO RAW INVALIDO`.
* PSXRAW v1 single disc (phase 0, raw base `0x45`, total `4 + 0x2454`, data at `raw_base + 5 * n`) and multi disc (default disc 1, door
  rotation 1 -> 2 -> 0 with the right raw base and sector limit).
* `boot_clock` writes the 49 entry Si5351 table through the bus.

Not run on the host: the pico bindings (`hooks_pico.cpp`), the interrupt context of `on_mech_command`, core 1 timing.

## 4. Not wired, not known, risks

* **`reset_card_state`**: the documented write `raw_base = 2` is reproduced literally (`FUN_20002f9a` stores 2 into `0x200087f0`, the
  initial `.data` value of that word is also 2, i.e. the base of the older raw card header). After a PS1 reset the card path then reads blocks at
  `2 + (lba - 0x96) * 5` until something remounts (`resetToMenu`, whose caller in V3 is unknown, see deltas.md 5). If V3 remounts after every reset
  in a path that is not known, a V2 card shows garbage after a plain console reset here. This is the single most likely functional
  difference on a console.
* **`resetToMenu` trigger** (`I2S::s_resetPending`): nothing sets it, as in the documentation. No hook is defined for it.
* **Door in V2 mode** only acts in phase 3 (a switched game is running), as in `FUN_1002773c`. Phase 3 is also what `sector_route` sets for
  any non `0x41A` read in phase 4.
* **`on_mech_command` order**: V3 runs the music flag and the PS1 command decoder BEFORE the upstream command body (the thunk at `0x10021624`) and the F9
  arm after it; the hook runs after the body (the body does not touch the card). The word is `raw & 0xFFFFFF`, the value read before the body clears it.
* **Trace**: `aux::trace_begin/end` records one event per command with flags 0 (the status word `FUN_10024000` needs globals that are not identified,
  `trace_flags` is not called). `trace_irq_begin/end` (wraps `setBootSectorPattern`, RAM `0x20000484`) is not wired: that needs an edit of `cmd.cpp`.
* **Linker drops**: of the 89 ledger functions, 5 are not in the ELF: `handler::abort_cheat_request` (`FUN_1002e0a0`, no caller anywhere in V3), `aux::raw_header_parse`
  (`FUN_10018ff2`, belongs to the old `DiscImage::load` that nothing calls in V3), `aux::trace_flags`, `aux::trace_irq_begin` (see above) and
  `aux::clock_wait_status` (inlined into `clock_program`, so it is present in effect).
* **`Env::service_busy`** (byte `0x20032ef2`) stays false: no writer found in V3. DEAD hook then runs the SD init as in the normal case.
* **Aliasing**: the card layer writes the `DiscImage` through a `CardDisc*` (`reinterpret_cast`), as the binary patched original does. Sizes and field offsets
  are checked at compile time (`disc_image.h`, `hooks_pico.cpp`); no LTO is used. If LTO is ever enabled, give `CardDisc` `may_alias`.
* **`c_sectorMax`** equals the total sectors (`sectors + 2 * 0x122A`) for card discs, the value the original shares with the card layer; the upstream `load()` used
  `indices[0] + 4652` (2 more).
* **I2C**: the Si5351 is programmed through the SDK driver with a 20 ms timeout per transfer, not the register-level code of `FUN_10025108`; timing of the
  controller set-up (fast mode counts `0x110/0x196`) is whatever `i2c_init(400 kHz)` computes at 271.2 MHz. Result of the sequence is not checked (as in V3, the boot continues).
* **Concurrency**: `on_mech_command` writes the card state from the mechacon interrupt while core 1 reads it, as in V3.
* **Card read cost**: `fallback_convert` uses byte loads; per sector it is a few thousand cycles, only in the fallback menu.
* The card handler chain of V3 is only partly known (several globals unidentified, `0x20031798`, `0x20008f74`); nothing was run on a console.

## 5. Linker check

`arm-none-eabi-nm -C` on the ELF shows all hooks (`mount_default_disc`, `card_read_sector`, `on_mech_command`, `on_door`, `poll`,
`reset_card_state`, `boot_trace`, `boot_clock`, `set_hw`), `card::mount`, `card::sector_route`, `handler::read_sector`,
`handler::read_sector_main`, `handler::on_pad_command`, `aux::cfg_music_serve`, `aux::cfg_load`, `aux::clock_program`,
`aux::legacy_swap_v1/v2`, `aux::legacy_v1_mount`, `aux::legacy_track_map`, `aux::fallback_convert`, `aux::trace_begin`, `loaderImage`.
122 `mh::` symbols. `objdump -d` shows `main` calling `boot_trace` and `boot_clock`, `readSector` calling `card_read_sector` (`0x20000dc0`),
the I2S loop calling `poll`, `processLatchedCommand` calling `on_mech_command`, `makeDummyCue` reaching `mount_default_disc` through its veneer.

## 6. Ledger (`re/ledger.py <elf> re/mh_v3.bin re/ledger`)

```
_ZN2mh3aux8cfg_loadERNS0_8CfgStateERKNS0_8CardViewERKNS0_11BlockReaderEPh 0x10024450 mine=  432 v3=  196  0.31  PSTCFG2 loader
_ZN2mh3aux15cfg_music_serveERNS0_8CfgStateERKNS0_8CardViewERKNS0_11BlockReaderEmmPh 0x100245e0 mine=  828 v3=  496  0.20  config and music server (chunk plan inlined)
_ZN2mh3aux16cfg_command_wordERNS0_8CfgStateEbm   0x10024588 mine=   40 v3=   62  0.50  command word hook, the tail call into FUN_100227cc is glue
_ZN2mh3aux15legacy_v1_parseEPKhRNS0_8LegacyV1E   0x100273c4 mine= 1280 v3=  724  0.18  header branch only; V2 mount call and RAM stub copy are glue
_ZN2mh3aux16legacy_track_mapERKNS0_9DiscEntryERKNS0_11BlockReaderERNS0_9DiscTableE 0x100270a8 mine=  920 v3=  670  0.26  track map and table builder (toc_build inlined)
_ZN2mh3aux14legacy_swap_v1ERKNS0_8LegacyV1EmRKNS0_11BlockReaderERNS0_9DiscTableE 0x1002773c mine=   88 v3=  228  0.22  mode 1 branch only (mode 2 is legacy_swap_v2)
_ZN2mh3aux16raw_header_parseEPKhRNS0_9DiscTableE 0x10018ff2 mine=    0 v3=    0 MISSING  raw card branch of DiscImage::load; SD read and cue fallback are upstream
_ZN2mh3aux14fallback_tableERNS0_9DiscTableE      0x10030000 mine=   40 v3=  118  0.26  table part; the mount chain (call FUN_100273c4) and state stores are glue
_ZN2mh3aux16fallback_convertEPKhS2_Pm            0x100300b8 mine=  134 v3=  138  0.37  sector conversion loop
_ZN2mh3aux17clock_wait_statusERKNS0_8ClockBusEh  0x10025094 mine=    0 v3=    0 MISSING  status wait
_ZN2mh3aux13clock_programERKNS0_8ClockBusE       0x10025108 mine=  272 v3=  364  0.23  register sequence only; I2C0 bring-up is pico glue
_ZN2mh3aux11trace_flagsERKNS0_12TraceFlagsInE    0x10024000 mine=    0 v3=    0 MISSING
_ZN2mh3aux10trace_initERNS0_10TraceBlockEm       0x10024044 mine=   48 v3=   50  0.47  chained init call is glue
_ZN2mh3aux11trace_beginERNS0_10TraceBlockEmm     0x10024090 mine=   68 v3=   96  0.51  first half only
_ZN2mh3aux15trace_irq_beginERNS0_10TraceBlockEmmmmm 0x10024188 mine=    0 v3=    0 MISSING  first half only
_ZN2mh4card9zero_fillEPvm                        0x100216d8 mine=   14 v3=   14  1.00  zero_fill
_ZN2mh4card4copyEPvPKvm                          0x100216e6 mine=   18 v3=   18  0.78  copy
_ZN2mh4card5equalEPKvS2_m                        0x100216f8 mine=   26 v3=   26  0.77  equal
_ZN2mh4card8all_zeroEPKvm                        0x10021714 mine=   24 v3=   24  0.75  all_zero
_ZN2mh4card8read_u32EPKv                         0x1002172c mine=   16 v3=   16  0.00  read_u32
_ZN2mh4card9store_u32EPvm                        0x10021742 mine=   14 v3=   14  0.31  store_u32
_ZN2mh4card12crc32_updateEmPKvm                  0x10021748 mine=   48 v3=   48  0.78  crc32_update
_ZN2mh4card5crc32EPKvm                           0x10021774 mine=   18 v3=   18  1.00  crc32
_ZN2mh4card11retry_delayEm                       0x10021788 mine=   36 v3=   36  0.78  retry_delay
_ZN2mh4card14copy_printableEPcmPKhm              0x10021924 mine=   58 v3=   58  0.62  copy_printable
_ZN2mh4card16text_append_charERNS0_7TextBufEc    0x100218ac mine=   40 v3=   40  0.75  text_append_char
_ZN2mh4card15text_append_strERNS0_7TextBufEPKc   0x100218d4 mine=   30 v3=   30  0.79  text_append_str
_ZN2mh4card15text_append_hexERNS0_7TextBufEmm    0x100218f4 mine=   68 v3=   68  0.45  text_append_hex
_ZN2mh4card11read_blocksEmm                      0x10021c14 mine=   70 v3=   70  0.37  read_blocks
_ZN2mh4card10read_bytesEmmPvm                    0x10021ce4 mine=  118 v3=  118  0.50  read_bytes
_ZN2mh4card9read_slotEmm                         0x1002200c mine=   64 v3=   64  0.56  read_slot
_ZN2mh4card12crc32_windowEmm                     0x10022040 mine=   96 v3=   96  0.37  crc32_window
_ZN2mh4card12header_validEPKh                    0x10021aa4 mine=  324 v3=  324  0.57  header_valid(slot)
_ZN2mh4card13validate_caseEPKhmPNS0_8CaseInfoE   0x10021960 mine=  280 v3=  280  0.64  validate_case
_ZN2mh4card13nth_used_caseEmPNS0_8CaseInfoE      0x10021c50 mine=  172 v3=  172  0.49  nth_used_case
_ZN2mh4card13case_by_indexEmPNS0_8CaseInfoE      0x10021d50 mine=   52 v3=   52  0.77  case_by_index
_ZN2mh4card13validate_discEPKhmmPNS0_8DiscInfoE  0x10021d84 mine=  616 v3=  616  0.35  validate_disc
_ZN2mh4card12disc_of_caseEmPNS0_8DiscInfoE       0x10021fbc mine=   76 v3=   76  0.76  disc_of_case
_ZN2mh4card14load_track_mapERKNS0_8DiscInfoEPNS0_8CardDiscE 0x10022084 mine=  528 v3=  528  0.24  load_track_map
_ZN2mh4card14listing_appendEPmPKch               0x10022284 mine=   88 v3=   88  0.61  listing_append
_ZN2mh4card15library_listingEv                   0x100222e0 mine=  628 v3=  628  0.41  library_listing
_ZN2mh4card17disc_info_requestEmPNS0_8DiscInfoE  0x10022518 mine=  108 v3=  108  0.45  disc_info_request
_ZN2mh4card20reset_response_cacheEv              0x100217b0 mine=   40 v3=   40  0.22  reset_response_cache
_ZN2mh4card12default_discEPNS0_8CardDiscE        0x100217e0 mine=  108 v3=  108  0.83  default_disc
_ZN2mh4card10post_eventEhhm                      0x10021858 mine=   84 v3=   84  0.15  post_event
_ZN2mh4card14post_event_ackEhm                   0x1002189c mine=   14 v3=   14  1.00  post_event_ack
_ZN2mh4card5mountEPNS0_8CardDiscE                0x10021280 mine=  840 v3=  840  0.51  mount
_ZN2mh4card12sector_routeEm                      0x10021640 mine=  136 v3=  136  0.33  sector_route
_ZN11picostation11MechCommand21processLatchedCommandEv 0x1002f050 mine=   20 v3=   82  0.27  wrapper: V3 FUN_1002f050 (hook chain), the logging layer FUN_10024090 is aux_trace
_ZN11picostation11MechCommand14processLatchedEv  0x200000c8 mine=  964 v3=  948  0.98  upstream body; first 8 bytes of the original are the veneer to FUN_1002f050
_ZN11picostation11MechCommand20setBootSectorPatternEh 0x20000484 mine=   84 v3=   70  0.91
_ZN11picostation11MechCommand7setSensEjb         0x200004e0 mine=   36 v3=   36  1.00
_ZN11picostation11MechCommand10updateMechEv      0x20000504 mine=   88 v3=   84  0.98
_ZN11picostation9DiscImage11buildSectorEiPmPtPKtb 0x20000588 mine=  568 v3=  542  0.98
_ZN11picostation9DiscImage12generateSubQEi       0x200007c0 mine= 1264 v3= 1202  1.00
_ZN11picostation9DiscImage6unloadEv              0x20000f4c mine=   82 v3=   82  1.00
_ZN11picostation9DiscImage10readSectorEPviNS0_12DataLocationEPKt 0x20000fa0 mine=  272 v3=  268  0.58  V3 continues with the inline card read (0x20001018..0x2000105a) owned by the card layer
_ZN11picostation14DriveMechanics16moveToNextSectorEv 0x2000112c mine=   48 v3=   38  0.88
_ZN11picostation14DriveMechanics9setSectorEmb    0x2000115c mine=  520 v3=  510  0.97
_ZN11picostation14DriveMechanics8moveSledERNS_11MechCommandE 0x20001374 mine=   82 v3=   82  1.00
_ZN11picostation14DriveMechanics9startSledEv     0x200013c8 mine=   22 v3=   22  1.00
ecc_generate                                     0x200013e0 mine=  396 v3=  360  0.96
eccedc_init                                      0x2000156c mine=  228 v3=  208  0.95
eccedc_generate                                  0x20001650 mine=  464 v3=  414  0.94
_ZN11picostation3I2S5startERNS_11MechCommandE    0x20001820 mine= 1612 v3= 1168  1.00  capstone stops early (reports 1.00); objdump compare 0.43: first 250 instructions aligned, rest differs by the patch stubs
_ZL12mech_irq_hndv                               0x20001f98 mine=   24 v3=   16  0.78
_ZL16interruptHandlerjm                          0x20001fb0 mine=  292 v3=  258  0.98
_ZL7initPWMPN11picostation11PWMSettingsE         0x200020d4 mine=  116 v3=  112  0.98
_ZN11picostation6initHWEv                        0x20002148 mine=  852 v3=  778  0.98
_ZN11picostation5resetEv                         0x200024c0 mine=  660 v3=  578  0.90
_ZN11picostation11resetToMenuEv                  0x20002760 mine=  652 v3=  538  0.93
_ZN11picostation10core0EntryEv                   0x200029dc mine=  752 v3=  770  1.00  capstone over reports; objdump compare 0.82 (register allocation of the inlined updatePlaybackSpeed)
_ZN11picostation4SubQ10start_subqEi              0x20002ce0 mine=  236 v3=  214  0.95
_ZN11picostation7ModChip17sendLicenseStringEiRNS_11MechCommandE 0x10000440 mine=  460 v3=  402  0.93  V3 adds an aux_trace irq event at the end of the license strings
_ZN2mh7handler16read_sector_mainEmbPPKh          0x100228c8 mine= 1460 v3= 2174  0.22  main handler incl. run_session_request, apply_*_request, build_* (inlined); FUN_100229f8 finish tail
_ZN2mh7handler11read_sectorEmbPPKh               0x1002e100 mine=  732 v3= 1642  0.23  v6 cheat fast path; also contains hook_dead (FUN_1002d02c, 178) and hook_reject_session (FUN_1002f118, 102) inlined
_ZN2mh7handler14on_pad_commandEm                 0x100227cc mine=  208 v3=  218  0.42  PS1 command decoder
_ZN2mh7handler15build_disc_infoEm                0x10022518 mine=  592 v3=   96  0.15  PGM1 disc info; original success path lives in cave FUN_10029020 (384) for invalid serials
_ZN2mh7handler18on_confirm_commandEm             0x1002f050 mine=  100 v3=   82  0.48  0xF9 arm
_ZN2mh7handler14finish_confirmEv                 0x1002f0b0 mine=  104 v3=   84  0.48  0xF9 judge
_ZN2mh7handler19abort_cheat_requestEv            0x1002e0a0 mine=    0 v3=    0 MISSING  drop PCR2 request
_ZN2mh5hooks18mount_default_discERN11picostation9DiscImageE 0x10030000 mine=  228 v3=  228  0.27  mount chain FUN_10030000 + FUN_100273c4 glue
_ZN2mh5hooks16card_read_sectorERN11picostation9DiscImageEPviPKt 0x10021640 mine=  536 v3=  152  0.21  sector stub (flash 10021640), chain and raw read inlined; serve_chain inlined
_ZN2mh5hooks15on_mech_commandERN11picostation11MechCommandEm 0x1002f050 mine=  104 v3=   82  0.34  F9 hook; also the 10024090/10024588 chain
_ZN2mh5hooks7on_doorEv                           0x1002773c mine=  280 v3=  228  0.21  legacy rotation (lambdas are the FUN_10021d50/FUN_10021d84 adapters)
_ZN2mh5hooks4pollEv                              0x1002f190 mine=   40 v3=   40  0.28  poll shim, finish_confirm is FUN_1002f0b0 (handler)
_ZN2mh5hooks16reset_card_stateEv                 0x20002f9a mine=   14 v3=   88  0.08  card part of the runtime reset
_ZN2mh5hooks10boot_traceEv                       0x10024564 mine=   44 v3=   44  0.24  trace init and cfg state clear
_ZN2mh5hooks10boot_clockEv                       0x100252dc mine=   56 v3=   56  0.23  Si5351 entry, I2C bring-up is hooks_pico.cpp

84/89 functions built, size-weighted similarity 0.593
```

The hook functions score low because each one merges several V3 functions or stubs (the ledger compares a single V3 range); the
real V3 code behind a hook is spread over functions of the other packages, whose scores are above.

## 7. Fuzzy match and coverage

`re/fuzzy.py <elf> re/mh_v3.bin re/ghidra/out-mhv3/functions.tsv /tmp/fz.tsv`: 792 funcs; 323 with ratio>=0.85; 66398 bytes of 131970

```
rest of image: 89204 bytes
  >= 0.95        43424 bytes   48.7%
  0.85 - 0.95     8962 bytes   10.0%
  0.60 - 0.85     2792 bytes    3.1%
  < 0.60         34026 bytes   38.1%
multi house region: 16408 bytes
  >= 0.95           46 bytes    0.3%
  0.85 - 0.95        0 bytes    0.0%
  0.60 - 0.85     1554 bytes    9.5%
  < 0.60         14808 bytes   90.2%
```
