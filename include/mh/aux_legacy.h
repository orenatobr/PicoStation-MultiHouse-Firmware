// Legacy card loaders of the V3 firmware (not used by V2 cards, but still accepted):
//  * "PSXRAW" version 1 multi disc layout   V3 FUN_100273c4 (header), FUN_100270a8 (track map -> disc table)
//  * disc rotation tick                      V3 FUN_1002773c
//  * older raw card header (sector 0)        V3 FUN_10018ff2 (RAM copy FUN_20000ce6)
// Layouts and decisions are documented in docs/re/aux.md.
#pragma once

#include "mh/aux_common.h"

namespace mh::aux {

constexpr uint32_t kTocOffset = 0x122A;     // 4650: added to every track position in the disc table
constexpr uint32_t kMaxLegacyDiscs = 6;
constexpr uint32_t kDiscEntryBytes = 100;

// Disc table: the firmware's in-RAM DiscImage object (CueDisc: track count at +0, then 0x34 byte track
// records starting at +4, then the DiscImage flags at +0x1470). Only the bytes the V3 code writes are named.
struct DiscTable {
    static constexpr size_t kBytes = 0x1480;
    static constexpr size_t kTrackStride = 0x34;
    uint8_t b[kBytes];
    uint32_t raw_base = 0;    // RAM 0x200087f0: raw data base block of the current disc
    uint32_t end_sector = 0;  // RAM 0x200088c0: total sectors + 0x2454

    DiscTable() { clear(); }
    void clear() {
        for (auto& v : b) v = 0;
        raw_base = end_sector = 0;
    }
    uint8_t* track(unsigned i) { return b + 4 + kTrackStride * i; }  // i = 0 lead-in, 1..n tracks, n + 1 lead-out
    const uint8_t* track(unsigned i) const { return b + 4 + kTrackStride * i; }
    void set_track_count(uint32_t n) { put_le32(b, n); }
    // Track record fields: +4 size, +8 file offset, +0xC index count, +0x10 index 0, +0x14 index 1, +0x1C type.
    void set_size(unsigned i, uint32_t v) { put_le32(track(i) + 4, v); }
    void set_file_offset(unsigned i, uint32_t v) { put_le32(track(i) + 8, v); }
    void set_index_count(unsigned i, uint32_t v) { put_le32(track(i) + 0xC, v); }
    void set_index0(unsigned i, uint32_t v) { put_le32(track(i) + 0x10, v); }
    void set_index1(unsigned i, uint32_t v) { put_le32(track(i) + 0x14, v); }
    void set_type(unsigned i, uint8_t v) { track(i)[0x1C] = v; }
    uint32_t track_count() const { return le32(b); }
    uint32_t size(unsigned i) const { return le32(track(i) + 4); }
    uint32_t file_offset(unsigned i) const { return le32(track(i) + 8); }
    uint32_t index_count(unsigned i) const { return le32(track(i) + 0xC); }
    uint32_t index0(unsigned i) const { return le32(track(i) + 0x10); }
    uint32_t index1(unsigned i) const { return le32(track(i) + 0x14); }
    uint8_t type(unsigned i) const { return track(i)[0x1C]; }  // 1 audio, 2 data
    // DiscImage tail: m_hasData @0x1470, m_currentLogicalTrack @0x1474, skip flags @0x1478/9, sd_menu @0x147A
    // (same layout as mh::card::CardDisc).
    void set_has_data(bool v) { b[0x1470] = v; }
    bool has_data() const { return b[0x1470] != 0; }
    void set_tail_flag(bool v) { b[0x147A] = v; }
    bool tail_flag() const { return b[0x147A] != 0; }
};

// 100 byte disc entry (state block layout at RAM 0x2003391c + 100 * i, also the current disc at 0x20033524).
struct DiscEntry {
    uint8_t b[kDiscEntryBytes] = {};
    uint8_t index() const { return b[0]; }      // disc id (V1: position)
    uint8_t case_index() const { return b[1]; } // 0xFF in V1 layouts
    uint8_t slot() const { return b[2]; }
    uint8_t track_count() const { return b[3]; }
    uint32_t base_lba() const { return le32(b + 0xC); }
    uint32_t track_lba() const { return le32(b + 0x10); }
    uint32_t raw_lba() const { return le32(b + 0x14); }
    uint32_t raw_blocks() const { return le32(b + 0x18); }
    uint32_t sectors() const { return le32(b + 0x1C); }
    const uint8_t* serial() const { return b + 0x30; }  // 16 bytes
    const uint8_t* title() const { return b + 0x40; }   // 32 bytes
};

struct LegacyV1 {
    uint32_t disc_count = 0;
    uint32_t default_disc = 0;
    DiscEntry discs[kMaxLegacyDiscs];
};

// V3 FUN_100273c4 (v1 branch): validate the 0xA00 byte header block and fill the disc entries.
bool legacy_v1_parse(const uint8_t* blk, LegacyV1& out);

// V3 FUN_100270a8: read the "PSTTRK1" map of `d` through `rd`, validate it and build the disc table.
bool legacy_track_map(const DiscEntry& d, const BlockReader& rd, DiscTable& out);

// Builds the table for `n` tracks (shared tail of FUN_100270a8, FUN_10030000 and FUN_100217e0).
struct TocTrack {
    uint32_t size, start, index1;
    bool data;
};
void toc_build(DiscTable& t, const TocTrack* tracks, unsigned n, uint32_t total_sectors, uint32_t raw_base);

// Whole legacy mount after the V2 mount failed (FUN_100273c4): header + track map of the default disc.
bool legacy_v1_mount(const uint8_t* hdr_blk, const BlockReader& rd, LegacyV1& out, DiscTable& table);

// V3 FUN_100273c4 bookkeeping after FUN_10021280 succeeded: 1 = single disc v1, 2 = V2 card.
inline uint32_t legacy_mode_after_v2(uint32_t v2_state0) { return v2_state0 == 0 ? 1 : 2; }

// V3 FUN_1002773c: disc rotation.
enum class SwapStatus : int { Ok = 0, Skipped = -1, CaseLookup = 2, DiscLookup = 3, TrackMap = 4 };
struct CaseView {
    uint8_t index = 0, disc_count = 0;
    uint8_t disc_ids[5] = {};
};
struct SwapEnv {
    void* ctx;
    bool (*case_by_index)(void* ctx, uint32_t idx, CaseView& out);   // FUN_10021d50
    bool (*disc_by_id)(void* ctx, uint32_t id, DiscEntry& out);      // FUN_10021d84
};
struct SwapResult {
    SwapStatus status = SwapStatus::Skipped;
    uint32_t next = 0;        // new position / slot
    DiscEntry entry;          // entry whose table was built
};
// Mode 1 (v1 multi disc): next disc in the list, wrapping; needs at least two discs.
SwapResult legacy_swap_v1(const LegacyV1& v1, uint32_t current, const BlockReader& rd, DiscTable& table);
// Mode 2 (V2 Case with 2..5 discs, state 3): next slot of the Case of `cur`.
SwapResult legacy_swap_v2(const DiscEntry& cur, const SwapEnv& env, const BlockReader& rd, DiscTable& table);

// Older raw card header (FUN_10018ff2): sector 0 with "PSXRAW", track count at +0xC, 10 byte entries at +0xD.
constexpr uint32_t kRawMaxTracks = 49;  // (512 - 0xD) / 10, bound added over the original
bool raw_header_parse(const uint8_t* sector512, DiscTable& out);

}  // namespace mh::aux
