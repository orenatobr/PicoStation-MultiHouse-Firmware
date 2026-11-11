// Integration points between the upstream derived PicoStation code (src/*.cpp) and the Multi House layers.
//
// The original V3 image reaches the card layer through a handful of patched call sites in upstream
// functions. Each patched site is one function in this header; the call sites live in the upstream files
// (cmd.cpp, disc_image.cpp, i2s.cpp, picostation.cpp, main.cpp). docs/re/deltas.md lists the V3 address of
// every site and what the original does there.
//
// Every function has a weak default in src/main.cpp so the firmware links without the Multi House layers
// (the default behaviour is the upstream behaviour). The integration code defines the strong versions.
#pragma once

#include <stdint.h>

namespace picostation {
class DiscImage;
class MechCommand;
}  // namespace picostation

namespace mh::hooks {

// V3 FUN_10030000 (veneer at RAM 0x20000cdc, replaces DiscImage::makeDummyCue). Mounts whatever the SD card
// offers into `disc` (V2 card header A or B, legacy "PSXRAW" multi disc, otherwise the built-in fallback
// menu), installs the RAM stubs and sets the card mode flag of the disc (DiscImage +0x147A) plus
// c_sectorMax. Returns false only when `disc` is null.
bool mount_default_disc(picostation::DiscImage& disc);

// V3 RAM stub 0x20033000 (flash copy of 0x10021640..0x100216d8), entered from DiscImage::readSector when
// the card mode flag is set, after the pre-gap test. Fills `buffer` (1176 words, scrambled) for the request
// sector `sector` (c_preGap already included, so the card LBA is sector, not sector - 150). Everything after
// the call is skipped: readSector returns to its caller.
void card_read_sector(picostation::DiscImage& disc, void* buffer, int sector, const uint16_t* scrambling);

// V3 FUN_1002f050 (veneer at RAM 0x200000c0): wraps MechCommand::processLatchedCommand. `raw` is the latched
// command word read BEFORE the upstream body runs (the body clears it). The hook runs after the body.
// V3 acts when (raw & 0xFF0000) == 0xF90000, i.e. the former COMMAND_GET_COVER (custom command 9).
void on_mech_command(picostation::MechCommand& mech, uint32_t raw);

// V3 FUN_1002773c via RAM stub 0x20033100: called from the I2S loop when the door interrupt fired since the
// previous iteration (the pending flag is test-and-cleared first). Legacy multi disc rotation.
void on_door();

// V3 FUN_1002f190 -> FUN_1002f0b0 via RAM stub: called once per I2S loop iteration, right after on_door
// handling. Completes a pending card command (state word at 0x20033440 == 0x0C).
void poll();

// V3 FUN_20002f9a (reset): the part of the runtime reset that belongs to the card layer (raw base back to 2,
// word at 0x20009a68 back to 0). The upstream-side state (MechCommand, cmd.cpp statics, I2S flags) is reset
// by picostation::resetRuntimeState().
void reset_card_state();

// V3 FUN_10024564 (main): event trace init (FUN_10024044) and the clear of the 24 byte command hook state at
// 0x20033800. Runs before eccedc_init, which the trace init chains to in the original.
void boot_trace();

// V3 FUN_100252dc (main): Si5351 table programming (replaces the upstream si5351_Init section of initHW), the
// original then continues into initHW.
void boot_clock();

}  // namespace mh::hooks
