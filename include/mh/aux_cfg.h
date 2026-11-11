// PSTCFG2 configuration loader and music server of the V3 firmware.
// V3 FUN_10024450 (loader), FUN_100245e0 (request server), FUN_10024588 (command word hook),
// FUN_10024564 (state reset). Behaviour and wire format are documented in docs/re/aux.md.
#pragma once

#include "mh/aux_common.h"

namespace mh::aux {

constexpr uint32_t kResponseBytes = 0x914;     // response sector buffer cleared before each answer
constexpr uint32_t kCfgBlocks = 5;
constexpr uint32_t kCfgTextOffset = 0x2C;
constexpr uint32_t kCfgTextMax = 0x800;
constexpr uint32_t kCfgCrcOffset = 0x92C;
constexpr uint32_t kCfgRequestLba = 300;       // request LBA of the config sector and of music chunk 0
constexpr uint32_t kMusicFirstChunk = 0x830;   // bytes in chunk 0 (0x30 header bytes + 0x800)
constexpr uint32_t kMusicChunk = 0x800;
constexpr uint32_t kMusicMaxChunks = 0x100;
constexpr uint32_t kMusicHeader = 0x18;
constexpr uint32_t kCfgTextResponseOffset = 0x114;
constexpr uint32_t kCommandMusicOn = 0x00F80008;  // command word that enables the music window

// Volatile state at RAM 0x20033800 (6 words), cleared by FUN_10024564.
struct CfgState {
    uint32_t music_enabled = 0;  // set by command word 0xF80008 while a card is mounted
    uint32_t loaded = 0;         // a valid PSTCFG2 block was read
    uint32_t music_lba = 0;
    uint32_t music_size = 0;
    uint32_t music_crc = 0;      // stored but never compared
    uint32_t generation = 0;     // header generation seen at load time
};

// What the loader needs from the mounted card (fields of the 0x20033400 state block).
struct CardView {
    bool mounted = false;        // state[0] == 1 and the state magic is valid
    uint32_t cfg_lba = 0;        // header cfg_lba
    uint32_t limit_blocks = 0;   // header art_size, upper bound of the cfg and music windows
    uint32_t generation = 0;     // header generation
};

// FUN_10024450. Fills `st`; on success writes the "CFG1" response into `resp` (kResponseBytes, not cleared here).
bool cfg_load(CfgState& st, const CardView& card, const BlockReader& rd, uint8_t* resp);

struct MusicChunk {
    uint32_t index = 0, count = 0, offset = 0, length = 0;
};
// Chunk arithmetic of FUN_100245e0. Returns false when the chunk is not served (response stays zero).
bool music_chunk_plan(uint32_t size, uint32_t n, MusicChunk& out);

enum class ServeResult { PassThrough, Served };
// FUN_100245e0: handles sector requests `lba` (count must be 1, lba >= 0x96) in the config/music window.
// PassThrough: the request belongs to the next handler. Served: `resp` (kResponseBytes) holds the answer.
ServeResult cfg_music_serve(CfgState& st, const CardView& card, const BlockReader& rd, uint32_t lba,
                            uint32_t count, uint8_t* resp);

// FUN_10024588: command word hook (bits 20..23 all set selects the F-class commands).
void cfg_command_word(CfgState& st, bool card_mounted, uint32_t word);

// FUN_10024564 (state part).
inline void cfg_reset(CfgState& st) { st = CfgState{}; }

}  // namespace mh::aux
