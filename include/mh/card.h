// V2 SD card layer of PicoStation_MH_V3 (rebuild of flash 0x10021274..0x10022bf4 and the RAM sector stub).
// Host safe: no pico headers. Card access goes through an injectable SdIo, so host tests serve a file
// backed image. Layout and validators: docs/re/CARD_LAYOUT_CROSSCHECK.md, evidence: docs/re/card-core.md.
//
// CHANGELOG (keep names stable; announce signature changes here)
//   r1  first publication: CardState, CardDisc, DiscInfo, CaseInfo, API declarations.
#pragma once

#include <cstddef>
#include <cstdint>

#include "mh/platform.h"

namespace mh::card {

// ---------------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------------
constexpr uint32_t kStateMagic = 0x39313334;  // "4319" little endian, state.magic when initialised
constexpr uint32_t kBlock = 0x200;            // SD block size
constexpr uint32_t kSlot = 0xA00;             // 5 block slot (one 2352 byte sector + 208 zero bytes)
constexpr uint32_t kSlotUsed = 0x930;         // bytes of a slot that carry data; 0x930..0xA00 must be zero
constexpr uint32_t kHeaderCrcOffset = 0x92C;  // header CRC position
constexpr uint32_t kCaseRecord = 0x50;
constexpr uint32_t kDiscRecord = 0x100;
constexpr uint32_t kMaxCases = 0x100;
constexpr uint32_t kMaxDiscs = 0x100;
constexpr uint32_t kMaxDiscsPerCase = 5;
constexpr uint32_t kMaxTracks = 99;
constexpr uint32_t kLeadIn = 0x122A;          // 4650: lead-in added to every track index of the disc table
constexpr uint32_t kDefaultDiscSectors = 0x385;  // the menu disc as seen by the PS1 (901 sectors)
constexpr uint32_t kRespSize = 0x914;         // response sector buffer bytes used by the library listing
constexpr uint32_t kTextSize = 0x3000;        // response text buffer
constexpr uint32_t kListNameCap = 0x47;       // longest "title - DISCO n" entry
constexpr uint32_t kFirstDataLba = 0x96;      // sector read stub: block = (lba - 0x96) * 5 from the raw base

// Layout facts shared with the writer (fixed zone, 512 byte blocks)
constexpr uint32_t kHdrBLba = 5, kCaseLba = 10, kDiscLba = 0x32, kArtLba = 0xB4;
constexpr uint32_t kMenuLba = 0x14B4, kCfgLba = 0x1685, kArtSize = 0x2800;

// ---------------------------------------------------------------------------------------------
// SD access (inject for host tests)
// ---------------------------------------------------------------------------------------------
// FUN_20003188 (init, 0 = ok) and FUN_20003414 (read, 0 = ok). `dst` has room for blocks * 512 bytes.
using SdInitFn = int (*)();
using SdReadFn = int (*)(uint32_t lba, uint32_t blocks, void* dst);
struct SdIo {
    SdInitFn init;
    SdReadFn read;
};
void set_sd(const SdIo& io);     // device: called once with the SD driver; tests: file backed image
const SdIo& sd();

// ---------------------------------------------------------------------------------------------
// Records returned by the validators
// ---------------------------------------------------------------------------------------------
// Output of the Case validator FUN_10021960 (0x2a bytes, `ids` are Disc table positions).
struct CaseInfo {
    uint8_t index;                      // +0x00 Case position
    uint8_t disc_count;                 // +0x01 1..5
    uint16_t art_id;                    // +0x02 0xFFFF none, else art slot (<= 0xFF)
    uint8_t ids[5];                     // +0x04 Disc ids, first disc_count distinct, rest 0
    char name[0x21];                    // +0x09 printable, at most 32 characters, NUL terminated
};
static_assert(sizeof(CaseInfo) == 0x2A, "CaseInfo is 0x2a bytes");

// Output of the Disc validator FUN_10021d84 (0x64 bytes). The same layout lives at CardState::disc.
struct DiscInfo {
    uint8_t id;                         // +0x00 Disc table position
    uint8_t case_index;                 // +0x01 owning Case
    uint8_t slot;                       // +0x02 position inside the Case
    uint8_t track_count;                // +0x03 1..99
    uint16_t art_id;                    // +0x04 0xFFFF none
    uint16_t pad06;                     // +0x06
    uint32_t flags;                     // +0x08 bit0 tracks fixed, bit1 EXT layout (< 4)
    uint32_t base_lba;                  // +0x0C package base
    uint32_t track_lba;                 // +0x10 track map slot (== base)
    uint32_t raw_lba;                   // +0x14 first raw data block (base + 0x100)
    uint32_t raw_blocks;                // +0x18 sectors * 5
    uint32_t sectors;                   // +0x1C 1..0x19999999
    uint32_t cheat_lba;                 // +0x20 0 or base + 5
    uint32_t cheat_size;                // +0x24 bytes
    uint32_t data_crc32;                // +0x28 Disc record +0x7C (CRC of the raw data, sent to the PS1 as a token)
    uint32_t record_crc;                // +0x2C CRC-32 of the Disc record 0..0xFB
    char serial[0x10];                  // +0x30 printable, at most 15 characters
    char title[0x21];                   // +0x40 printable, at most 32 characters
    uint8_t pad61[3];                   // +0x61
};
static_assert(sizeof(DiscInfo) == 0x64, "DiscInfo is 0x64 bytes");

// ---------------------------------------------------------------------------------------------
// The state block (fixed RAM block of 0x1b0 bytes on the original at 0x20033400)
// ---------------------------------------------------------------------------------------------
// Phase values (state.phase). 0 = not mounted / legacy raw disc, 1 = menu, 2 = game running,
// 3 = fault after a bad request, 4 = switch acknowledged by the PS1 (inferred from FUN_100228c8).
enum Phase : uint32_t { kPhaseNone = 0, kPhaseMenu = 1, kPhaseGame = 2, kPhaseFault = 3, kPhaseSwitch = 4 };

// PLS1 status event record (state.event, 0x28 bytes) posted by FUN_10021858.
struct CardEvent {
    uint32_t magic;                     // +0x00 "PLS1" (0x31534C50)
    uint16_t version;                   // +0x04 1
    uint16_t length;                    // +0x06 0x28
    uint32_t seq;                       // +0x08 incremented on every post, wraps 0xFFFFFFFF -> 1
    uint16_t session;                   // +0x0C read by FUN_1002f000 (cheat session), not written by the card core
    uint16_t token;                     // +0x0E compared with sel_disc by the PS1 command handler (command 400)
    uint8_t code;                       // +0x10 event code (0 mounted, 1 switch armed, 3, 5, 6, 7, 8, 9, 10 ...)
    uint8_t arg;                        // +0x11
    uint16_t flags;                     // +0x12 mount sets 8, every post ORs 4
    uint32_t reserved14;                // +0x14 compared with the PCR2 request +8 (cheat package)
    uint32_t reserved18;                // +0x18
    uint32_t reserved1c;                // +0x1C
    uint32_t extra;                     // +0x20 third argument of post_event
    uint32_t crc;                       // +0x24 CRC-32 of 0..0x23
};
static_assert(sizeof(CardEvent) == 0x28, "CardEvent is 0x28 bytes");

struct CardState {
    uint32_t phase;                     // +0x00 Phase; written last by mount (1 = ready), cleared first
    uint32_t magic;                     // +0x04 kStateMagic once mount ran
    uint32_t disc_image_addr;           // +0x08 address of the CardDisc table handed to mount (0 = none)
    uint32_t generation;                // +0x0C header generation of the chosen header (A or B)
    uint32_t case_count;                // +0x10 header +0x08 (u16)
    uint32_t disc_count;                // +0x14 header +0x0A (u16)
    uint32_t case_lba;                  // +0x18 header +0x1C (10)
    uint32_t disc_lba;                  // +0x1C header +0x24 (0x32)
    uint32_t art_lba;                   // +0x20 header +0x2C (0xB4)
    uint32_t menu_lba;                  // +0x24 header +0x34 (0x14B4)
    uint32_t menu_sectors;              // +0x28 header +0x3C (1..93). 0 selects the built-in fallback menu
    uint32_t cfg_lba;                   // +0x2C header +0x40 (0x1685), PSTCFG2 slot
    uint32_t art_size;                  // +0x30 header +0x48 (0x2800): first package LBA, bound of config/music windows
    uint32_t crc_cases;                 // +0x34 header +0x50 (stored, never compared)
    uint32_t crc_discs;                 // +0x38 header +0x54 (stored, never compared)
    uint32_t crc_menu;                  // +0x3C header +0x5C (stored, never compared)
    uint32_t cmd;                       // +0x40 pending PS1 request kind (1..12), 0 = none (FUN_100227cc)
    uint32_t cmd_arg;                   // +0x44 16 bit argument of that request
    uint32_t list_offset;               // +0x48 first Case/Disc of the library listing page
    uint32_t cache_key;                 // +0x4C response cached for this key (100/400/900 ...), 0xFFFFFFFF = none
    uint32_t sel_case;                  // +0x50 selected Case index, > 0xFF = whole library listing
    uint32_t art_block;                 // +0x54 art LBA of the item the PS1 asked for, 0 = none
    uint32_t sel_slot;                  // +0x58 slot of the selected Disc inside its Case
    uint32_t sel_disc_id;               // +0x5C Disc table id of the selected Disc
    uint32_t valid;                     // +0x60 1 after a V2 mount (library listing guard)
    uint32_t switch_count;              // +0x64 counts PS1 acknowledgements of a disc switch
    uint32_t pcr2_mode;                 // +0x68 incoming PCR2 packet: 2 receiving, 0 idle
    uint32_t pcr2_fill;                 // +0x6C bytes of the PCR2 packet received so far (0..0x24)
    uint32_t pcr2_ready;                // +0x70 PCR2 packet complete
    uint32_t cheat_pending;             // +0x74 cheat blob delivery in progress
    uint32_t cheat_bytes;               // +0x78 size of the loaded cheat blob
    uint32_t cheat_status;              // +0x7C cheat status code (1..3)
    uint8_t pcr2[0x24];                 // +0x80 PCR2 request: "PCR2", len 0x24, token, ... CRC at +0xA0
    uint8_t game_id[0x80];              // +0xA4 text built when a Disc is selected (FUN_10022518 body)
    DiscInfo disc;                      // +0x124 the selected Disc (0x64 bytes)
    CardEvent event;                    // +0x188 PLS1 event record
};
static_assert(offsetof(CardState, case_count) == 0x10, "layout");
static_assert(offsetof(CardState, cmd) == 0x40, "layout");
static_assert(offsetof(CardState, sel_case) == 0x50, "layout");
static_assert(offsetof(CardState, valid) == 0x60, "layout");
static_assert(offsetof(CardState, cheat_status) == 0x7C, "layout");
static_assert(offsetof(CardState, pcr2) == 0x80, "layout");
static_assert(offsetof(CardState, game_id) == 0xA4, "layout");
static_assert(offsetof(CardState, disc) == 0x124, "layout");
static_assert(offsetof(CardState, event) == 0x188, "layout");
static_assert(offsetof(CardState, event.flags) == 0x19A, "layout");
static_assert(sizeof(CardState) == 0x1B0, "the state block is 0x1b0 bytes");

CardState& state();                     // the one state block (0x20033400 on the original)

// ---------------------------------------------------------------------------------------------
// Disc table handed to mount / load_track_map: mirror of picostation::DiscImage in V3
// (CueDisc with MAXINDEX 2, plus hasData, current track, skip flags and the SD flag at +0x147A).
// ---------------------------------------------------------------------------------------------
struct CardTrack {                      // 0x34 bytes, CueTrack of the V3 build
    uint32_t file;                      // +0x00 unused by the card layer
    uint32_t size;                      // +0x04 sectors
    uint32_t file_offset;               // +0x08 first sector of the track
    uint32_t index_count;               // +0x0C 1
    uint32_t indices[2];                // +0x10 INDEX 00, INDEX 01 (absolute, lead-in added)
    uint32_t postgap;                   // +0x18
    uint32_t track_type;                // +0x1C 1 audio, 2 data
    uint32_t flags[5];                  // +0x20 compressed, copy permitted, four channel, pre-emphasis, SCMS
};
static_assert(sizeof(CardTrack) == 0x34, "CardTrack is 0x34 bytes");

struct CardCue {                        // CueDisc of the V3 build, 0x1470 bytes
    uint32_t track_count;               // +0x0000
    CardTrack tracks[100];              // +0x0004 track 0 is the lead-in, track n+1 the lead-out marker
    char catalog[14];                   // +0x1454
    char isrc[13];                      // +0x1462
};
static_assert(sizeof(CardCue) == 0x1470, "CardCue is 0x1470 bytes");

struct CardDisc {
    CardCue cue;                        // +0x0000
    uint8_t has_data;                   // +0x1470
    uint8_t pad1471[3];
    int32_t current_track;              // +0x1474
    uint8_t skip_bootsector;            // +0x1478
    uint8_t skip_edc;                   // +0x1479
    uint8_t sd_menu;                    // +0x147A set to 1 by the card layer: sectors come from the card
    uint8_t pad147b;
};
static_assert(offsetof(CardDisc, cue.tracks) == 4, "layout");
static_assert(offsetof(CardDisc, has_data) == 0x1470, "layout");
static_assert(offsetof(CardDisc, current_track) == 0x1474, "layout");
static_assert(offsetof(CardDisc, sd_menu) == 0x147A, "layout");
static_assert(sizeof(CardDisc) == 0x147C, "layout");

// Globals the card layer publishes to the sector pipeline (0x200087f0, 0x20009a68, 0x200088c0).
struct CardGlobals {
    uint32_t raw_base;                  // first raw block: sector lba -> block raw_base + (lba - 0x96) * 5
    uint32_t sector_cursor;             // cleared by mount / default disc
    uint32_t total_sectors;             // disc sectors + 2 * kLeadIn
};
CardGlobals& globals();

// ---------------------------------------------------------------------------------------------
// Buffers owned by the card layer
// ---------------------------------------------------------------------------------------------
uint8_t* block_buffer();                // 0xA00 byte buffer filled by read_blocks (0x2000a39c)
uint8_t* response();                    // response sector buffer (0x20034000), at least kRespSize bytes
struct TextBuf {
    char data[kTextSize + 1];           // 0x20038000 on the original
    uint32_t len;
};
TextBuf& text();

// Response cache invalidated by reset_response_cache (33 words, 0x20009368..0x200093ec).
struct ResponseCache {
    uint32_t slot[32];
    uint32_t last;
};
ResponseCache& response_cache();

// ---------------------------------------------------------------------------------------------
// Leaf helpers (FUN_100216d8 .. FUN_10021788, FUN_10021924, FUN_100218ac..FUN_100218f4)
// ---------------------------------------------------------------------------------------------
void zero_fill(void* dst, uint32_t n);                          // 100216d8 (n >= 1)
void copy(void* dst, const void* src, uint32_t n);              // 100216e6
bool equal(const void* a, const void* b, uint32_t n);           // 100216f8 (n >= 1)
bool all_zero(const void* p, uint32_t n);                       // 10021714
uint32_t read_u32(const void* p);                               // 1002172c (unaligned safe)
void store_u32(void* p, uint32_t v);                            // 10021742
uint32_t crc32_update(uint32_t crc, const void* p, uint32_t n); // 10021748 (no init, no final xor)
uint32_t crc32(const void* p, uint32_t n);                      // 10021774 (init ~0, final xor ~0)
uint32_t crc32_window(uint32_t len, uint32_t zero_from);        // 10022040 over block_buffer(), bytes [zero_from, +4) read as 0
void retry_delay(uint32_t attempt);                             // 10021788 busy wait
// 10021924: copies src (NUL terminated, at most src_limit bytes) to dst, bytes outside 0x20..0x7E become blanks.
// kind == 0x10 limits the result to 15 characters (serials), anything else to 32 (names); NUL terminated.
void copy_printable(char* dst, uint32_t kind, const uint8_t* src, uint32_t src_limit);

bool text_append_char(TextBuf& t, char c);                      // 100218ac (false when full: len > 0x2ffe)
bool text_append_str(TextBuf& t, const char* s);                // 100218d4
bool text_append_hex(TextBuf& t, uint32_t v, uint32_t nibbles); // 100218f4 (upper case, high nibble first)

// ---------------------------------------------------------------------------------------------
// Block IO
// ---------------------------------------------------------------------------------------------
bool read_blocks(uint32_t lba, uint32_t blocks);                // 10021c14 into block_buffer(), 3 tries
bool read_bytes(uint32_t lba, uint32_t byte_off, void* dst, uint32_t len); // 10021ce4 (<= 5 blocks per read)
bool read_slot(uint32_t lba, uint32_t tries);                   // 1002200c 5 blocks, bytes 0x930..0xA00 zero

// ---------------------------------------------------------------------------------------------
// Validators and record access
// ---------------------------------------------------------------------------------------------
// Header validator FUN_10021aa4 on the slot held in block_buffer().
bool header_valid();
// Pure form on a 0xA00 byte slot (used by tests and by header_valid).
bool header_valid(const uint8_t* slot);
// FUN_10021960: 1 used and valid (info filled when non null), 0 empty (all zero), -1 invalid.
int validate_case(const uint8_t* rec, uint32_t index, CaseInfo* out);
// FUN_10021d84 body on a 0x100 byte record: same convention. first_pkg_lba is state.art_size.
int validate_disc(const uint8_t* rec, uint32_t id, uint32_t first_pkg_lba, DiscInfo* out);
bool nth_used_case(uint32_t n, CaseInfo* out);                  // 10021c50
bool case_by_index(uint32_t index, CaseInfo* out);              // 10021d50 (false for empty or invalid)
bool disc_by_id(uint32_t id, DiscInfo* out);                    // 10021d84
bool disc_of_case(uint32_t slot, DiscInfo* out);                // 10021fbc, Case = state.sel_case
// FUN_10022084: validates the PSTTRK1 slot of `d` and, when `out` is not null, fills the disc table
// (deviation: out is only touched after the whole map was accepted).
bool load_track_map(const DiscInfo& d, CardDisc* out);

// ---------------------------------------------------------------------------------------------
// Mount and state
// ---------------------------------------------------------------------------------------------
// FUN_10021280: initialises the SD, chooses header A or B (or the legacy PSXRAW v1 single disc),
// fills state() and the default disc. Returns false when no layout is usable. `disc` must not be null.
bool mount(CardDisc* disc);
void default_disc(CardDisc* disc);                              // 100217e0: the menu
void reset_response_cache();                                    // 100217b0
void post_event(uint8_t code, uint8_t arg, uint32_t extra);     // 10021858
void post_event_ack(uint8_t arg, uint32_t extra);               // 1002189c (code 8)

// ---------------------------------------------------------------------------------------------
// Library listing for the PS1 menu (FUN_100222e0 and FUN_10022284 / FUN_10021a70)
// ---------------------------------------------------------------------------------------------
// Appends [len][flag][bytes] to response() at *pos; false when it does not fit (pos + 6 + len > 0x914) or len is 0.
bool listing_append(uint32_t* pos, const char* name, uint8_t flag);
// Fills response(): zeroes 0x914 bytes, then entries, then the 4 byte trailer [0][err][count hi][count lo].
void library_listing();
// FUN_10022518 failure path ("PGM1" error response, event 10); true when the Disc of `slot` is valid
// (the success response is built by the caller).
bool disc_info_request(uint32_t slot, DiscInfo* out);

// ---------------------------------------------------------------------------------------------
// Sector read stub (flash 0x10021640..0x100216d8 copied to RAM by the original; here a RAM function)
// ---------------------------------------------------------------------------------------------
enum class SectorRoute {
    kCard,            // read raw_base + (lba - 0x96) * 5 from the card
    kBuiltinThenCard, // offer the sector to the built-in decoder FUN_100300b8 first, then the card
    kBuiltin,         // no card menu (phase 1, menu_sectors == 0): built-in menu only
};
// Mirrors the branch tree of the stub: phase 1 -> builtin for lba >= 0xF3, phase 2 or 4 -> builtin for lba == 0x41a.
// Side effect: phase 4 and lba != 0x41a moves the state to 3 and resets the response cache.
SectorRoute sector_route(uint32_t lba);
// First block of the raw sector for `lba` (>= 0x96): raw_base + (lba - 0x96) * 5.
uint32_t sector_block(uint32_t raw_base, uint32_t lba);

}  // namespace mh::card
