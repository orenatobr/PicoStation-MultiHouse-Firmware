// PS1 command handler of PicoStation_MH_V3: answers reads of the "virtual sectors" the PS1 menu uses to
// talk to the card layer (library listing, art, cheats, disc info, session events), and runs the
// disc switch session. Host safe: card access goes through mh::card (SdIo is injectable).
//
// Wire formats, windows, the session state machine and the original function map: docs/re/handler.md.
//
// CHANGELOG
//   r1  first publication. Uses card.h r1 only; no additions needed there.
#pragma once

#include "mh/card.h"
#include "mh/handler_cheat.h"
#include "mh/platform.h"

namespace mh::handler {

// ---------------------------------------------------------------------------------------------
// Sector windows (absolute LBA; "N" in the docs is lba - 0x96)
// ---------------------------------------------------------------------------------------------
constexpr uint32_t kLbaListing = 0x96 + 100;   // 0xFA   library listing
constexpr uint32_t kLbaConfig = 0x96 + 0x96;   // 0x12C  "CFG1" page (PSTCFG2 slot)
constexpr uint32_t kLbaArt = 0x15E;            // 0x15E..0x167 art chunks (PART / FART)
constexpr uint32_t kArtChunks = 10;
constexpr uint32_t kLbaCheat = 0x1C2;          // 0x1C2..0x3C1 cheat text chunks (PCH2)
constexpr uint32_t kCheatChunks = 0x200;
constexpr uint32_t kCheatChunksV6 = 100;       // chunks served by the v6 handler FUN_1002e100
constexpr uint32_t kLbaInfo = 0x96 + 400;      // 0x226  disc info "PGM1"
constexpr uint32_t kLbaSession = 0x96 + 900;   // 0x41A  session event page "PLS1"
constexpr uint32_t kMenuSectors = 0x5D;        // N < 0x5D is the SD menu image while the menu runs

constexpr uint32_t kArtChunkBytes = 0x842;     // art bytes per chunk
constexpr uint32_t kArtTotal = 0x2800;         // bytes of one art image

// Response magics (little endian u32 of the ASCII tag)
constexpr uint32_t kMagicPgm1 = 0x314D4750;    // "PGM1" disc info
constexpr uint32_t kMagicPch2 = 0x32484350;    // "PCH2" cheat chunk
constexpr uint32_t kMagicPcr2 = 0x32524350;    // "PCR2" request from the PS1
constexpr uint32_t kMagicSel1 = 0x314C4553;    // "SEL1" token seed
constexpr uint32_t kMagicPhc6 = 0x36434850;    // "PHC6" v6 validation cache tag (internal)
constexpr uint32_t kMagicPls1 = 0x31534C50;    // "PLS1" event page (card.h CardEvent)

// PCH2 chunk status byte (+0x0A)
enum ChunkStatus : uint8_t {
    kChunkOk = 0,
    kChunkNoCheats = 1,   // disc has no cheat blob or its size is out of range
    kChunkPastEnd = 2,    // chunk index beyond the text, or the blob could not be read
    kChunkInvalid = 3,    // bad request, bad session, bad flags or invalid blob
};

// Event codes posted to the PS1 (CardEvent::code), see docs/re/handler.md section 6.
enum EventCode : uint8_t {
    kEvSwitchArmed = 1,   // switch request accepted
    kEvSwitched = 2,      // disc started
    kEvAck3 = 3,
    kEvAck4 = 4,
    kEvConfirmed = 5,
    kEvClosing = 6,
    kEvMenuBack = 7,
    kEvReject = 8,        // post_event_ack: arg 2 bad token, 3 bad session, 4 bad phase, 7 load failed, 10 info error
    kEvSessionDone = 9,
};

// PS1 request kinds stored in CardState::cmd by on_pad_command (FUN_100227cc).
enum Cmd : uint32_t {
    kCmdNone = 0,
    kCmdListAll = 1,      // arg unused: list all Cases from list_offset 0
    kCmdListAll2 = 2,
    kCmdSelectCase = 3,   // arg = n-th used Case
    kCmdListOffset = 4,   // arg = first entry of the page
    kCmdArtSelect = 5,    // arg = Case or Disc slot whose art follows
    kCmdInfo = 6,         // arg = Disc slot, answered at kLbaInfo
    kCmdSwitch = 7,
    kCmdConfirm = 8,
    kCmdFinish = 9,
    kCmdCancel = 10,
    kCmdPcr2 = 11,        // a PCR2 request packet is complete
};

// ---------------------------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------------------------
// The disc table the card was mounted with (CardState::disc_image_addr on the device). Used by the
// switch session (load_track_map / default_disc). May be null: switches then fall back to the menu.
void init(card::CardDisc* disc);

// FUN_100227cc: PS1 command word -> pending request. Words (word & 0xF00000) == 0xF00000 only.
void on_pad_command(uint32_t word);

// FUN_1002f050 (after the aux command decoder FUN_10024090 ran): arms the 0xF9 confirmation.
void on_confirm_command(uint32_t word);
// FUN_1002f0b0: finishes a pending 0xF9 confirmation (called once per command poll).
void finish_confirm();
// FUN_1002e0a0: drops a pending cheat request (cmd 11) and the v6 validation cache.
void abort_cheat_request();

// Globals of the original that the handler only reads (see docs/re/handler.md section 8).
struct Env {
    bool service_busy;           // byte 0x20032ef2: skips the DEAD reply
    bool hold;                   // word 0x20033800: the v6 cheat fast path steps aside
};
Env& env();

// FUN_100228c8 front chain: DEAD hook (FUN_1002d02c), N=900 reject (FUN_1002f118), v6 cheat fast path
// (FUN_1002e100), then the main handler. Returns false when the sector is not answered (the caller
// reads it from the disc image), true with *resp pointing at the 0x914 byte response buffer.
// `location_one` is the original `param_4 == 1`.
bool read_sector(uint32_t lba, bool location_one, const uint8_t** resp);

// The main handler alone (FUN_100228c8).
bool read_sector_main(uint32_t lba, bool location_one, const uint8_t** resp);

// ---------------------------------------------------------------------------------------------
// Pieces (exposed for tests)
// ---------------------------------------------------------------------------------------------
// FUN_10022518 + FUN_10029020: builds the PGM1 disc info for Disc `slot` of the selected Case.
void build_disc_info(uint32_t slot);
// Serial format "ABCD-12345" (FUN_10029020 head).
bool serial_valid(const char* serial);
// Token the PS1 must echo: 1 when the SEL1 hash matches, else hash ^ SEL1.
uint32_t disc_token(uint32_t slot, const card::DiscInfo& d, uint32_t generation);
// Boot path "cdrom:\ABCD_123.45;1" built from a valid serial (writes 21 bytes with NUL).
void build_boot_path(const char* serial, char* out);
// The session machine at N=900: runs one pending request (kCmdSwitch..kCmdCancel).
void run_session_request();

}  // namespace mh::handler
