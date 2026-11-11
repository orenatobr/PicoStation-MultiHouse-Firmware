// V3 FUN_10024450 / FUN_100245e0 / FUN_10024588: PSTCFG2 loader and music server.
#include "mh/aux_cfg.h"

#include <cstring>

namespace mh::aux {

namespace {
const uint8_t kMagic[8] = {'P', 'S', 'T', 'C', 'F', 'G', '2', 0};
}

// V3 FUN_10024450
bool cfg_load(CfgState& st, const CardView& card, const BlockReader& rd, uint8_t* resp) {
    st.loaded = 0;
    // The original adds in 32 bits; 64 bits here so that a huge cfg_lba cannot wrap into range.
    if (card.cfg_lba == 0 || static_cast<uint64_t>(card.cfg_lba) + kCfgBlocks > card.limit_blocks) return false;

    uint8_t blk[kBlockBytes];
    if (!rd(card.cfg_lba, kCfgBlocks, blk)) return false;
    if (!bytes_equal(blk, kMagic, 8)) return false;
    if (blk[8] != 1 || blk[9] != 0) return false;           // version u16 1; +0xA is not read
    if (le32(blk + 0x20) != 0) return false;                // music lba high dword
    if (crc32(blk, kCfgCrcOffset) != le32(blk + kCfgCrcOffset)) return false;

    // Text: NUL terminated within 0x800 bytes and not empty.
    uint32_t len = 0;
    while (len < kCfgTextMax && blk[kCfgTextOffset + len] != 0) ++len;
    if (len == 0 || len == kCfgTextMax) return false;

    st.music_lba = le32(blk + 0x1C);
    st.music_size = le32(blk + 0x24);
    st.music_crc = le32(blk + 0x28);
    st.generation = card.generation;
    std::memcpy(resp, "CFG1", 4);
    put_le16(resp + 4, static_cast<uint16_t>(len));
    std::memcpy(resp + kCfgTextResponseOffset, blk + kCfgTextOffset, len);
    st.loaded = 1;
    return true;
}

// V3 FUN_100245e0 (chunk arithmetic)
bool music_chunk_plan(uint32_t size, uint32_t n, MusicChunk& out) {
    uint32_t len = kMusicFirstChunk;
    uint32_t remaining = size;
    uint32_t offset = 0;
    uint32_t count;
    if (size < kMusicFirstChunk + 1) {
        if (n != 0) return false;
        count = 1;
    } else {
        count = ((size - 0x31) >> 11) + 1;
        if (count > kMusicMaxChunks || count <= n) return false;
        if (n != 0) {
            offset = n * kMusicChunk + 0x30;
            len = kMusicChunk;
            remaining = size - offset;
        }
    }
    if (remaining < len) len = remaining;
    out = MusicChunk{n, count, offset, len};
    return true;
}

// V3 FUN_100245e0
ServeResult cfg_music_serve(CfgState& st, const CardView& card, const BlockReader& rd, uint32_t lba,
                            uint32_t count, uint8_t* resp) {
    if (!card.mounted || count != 1 || lba < 0x96) return ServeResult::PassThrough;
    const bool enabled = st.music_enabled != 0;
    if (enabled ? (lba - kCfgRequestLba) > 0xFF : lba != kCfgRequestLba) return ServeResult::PassThrough;

    std::memset(resp, 0, kResponseBytes);
    if (!enabled) {
        // Config window: reload the block on every request; "DEAD" tells the PS1 there is no config.
        if (!cfg_load(st, card, rd, resp)) std::memcpy(resp, "DEAD", 4);
        return ServeResult::Served;
    }
    if (!((st.loaded != 0 && st.generation == card.generation) || cfg_load(st, card, rd, resp))) return ServeResult::Served;
    std::memset(resp, 0, kResponseBytes);

    const uint32_t size = st.music_size;
    if (st.music_size == 0 || st.music_lba == 0 || st.music_crc == 0) return ServeResult::Served;
    if (st.music_lba >= card.limit_blocks) return ServeResult::Served;
    if (static_cast<uint64_t>(size) > static_cast<uint64_t>(card.limit_blocks - st.music_lba) * 0x200) return ServeResult::Served;

    MusicChunk c;
    if (!music_chunk_plan(size, lba - kCfgRequestLba, c)) return ServeResult::Served;

    uint8_t blk[kBlockBytes];
    const uint32_t skew = c.offset & 0x1FF;
    const uint32_t blocks = (c.length + skew + 0x1FF) >> 9;
    if (!rd(st.music_lba + (c.offset >> 9), blocks, blk)) return ServeResult::Served;  // zeroed answer

    std::memcpy(resp + kMusicHeader, blk + skew, c.length);
    std::memcpy(resp, "MUS1", 4);
    put_le16(resp + 4, kMusicHeader);
    resp[6] = static_cast<uint8_t>(c.index);
    resp[7] = 0;
    put_le16(resp + 8, static_cast<uint16_t>(c.count));
    put_le16(resp + 10, static_cast<uint16_t>(c.length));
    put_le32(resp + 12, size);
    put_le32(resp + 16, crc32(resp + kMusicHeader, c.length));
    put_le32(resp + 20, crc32(resp, 0x14));
    return ServeResult::Served;
}

// V3 FUN_10024588
void cfg_command_word(CfgState& st, bool card_mounted, uint32_t word) {
    if ((word & 0xF00000) == 0xF00000) st.music_enabled = (card_mounted && word == kCommandMusicOn) ? 1 : 0;
}

}  // namespace mh::aux
