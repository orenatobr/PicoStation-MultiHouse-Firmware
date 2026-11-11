// V3 FUN_100273c4 / FUN_100270a8 / FUN_1002773c / FUN_10018ff2: legacy "PSXRAW" loaders.
#include "mh/aux_legacy.h"

#include <cstring>

namespace mh::aux {

namespace {
const uint8_t kRaw[6] = {'P', 'S', 'X', 'R', 'A', 'W'};
const uint8_t kDisc[4] = {'D', 'I', 'S', 'C'};
const uint8_t kTrk[8] = {'P', 'S', 'T', 'T', 'R', 'K', '1', 0};
const uint8_t kSbi[4] = {'S', 'B', 'I', 0};
}  // namespace

// V3 FUN_100273c4, version 1 branch (the V2 mount FUN_10021280 has already refused the card).
bool legacy_v1_parse(const uint8_t* blk, LegacyV1& out) {
    if (!bytes_equal(blk, kRaw, 6) || blk[6] != 1) return false;
    const uint32_t n = blk[7];
    if (n < 2 || n > kMaxLegacyDiscs) return false;
    if (blk[8] >= n || blk[9] != 0 || le16(blk + 10) != 0x930) return false;
    if (le32(blk + 0x10) != crc32_hole(blk, 0x930, 0x10, 4)) return false;
    if (!all_zero(blk + 0x14, 0x11C) || !all_zero(blk + 0x930, 0xD0)) return false;

    out = LegacyV1{};
    out.disc_count = n;
    out.default_disc = blk[8];
    uint32_t next = 5;  // first package starts right after the 5 block header
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* r = blk + 0x130 + i * 0x100;
        if (!bytes_equal(r, kDisc, 4) || r[4] != 1 || r[5] != i || r[6] != 0xFF || r[7] != i) return false;
        if (le32(r + 8) != 0) return false;
        if (le32(r + 0xFC) != crc32_hole(r, 0xFC, 0xFC, 0)) return false;
        if (le32(r + 0x40) || le32(r + 0x48) || le32(r + 0x50) || le32(r + 0x58)) return false;
        if (le16(r + 0x62) != 0xFFFF) return false;
        if (!all_zero(r + 0x64, 0x18) || !all_zero(r + 0x94, 0x68)) return false;

        const uint32_t base = le32(r + 0x3C), track = le32(r + 0x44), raw = le32(r + 0x4C);
        const uint32_t blocks = le32(r + 0x54), sectors = le32(r + 0x5C);
        const uint32_t tracks = r[0x60];
        if (base != next || track != next || raw != next + 5) return false;
        if (sectors - 1 > 0x6DDCF || blocks != sectors * 5 || tracks - 1 > 0x61) return false;
        const uint32_t end = raw + blocks;
        if (end < raw) return false;
        next = end;

        DiscEntry& e = out.discs[i];
        e.b[0] = static_cast<uint8_t>(i);
        e.b[1] = 0xFF;
        e.b[2] = static_cast<uint8_t>(i);
        e.b[3] = r[0x60];
        put_le16(e.b + 4, 0xFFFF);
        put_le32(e.b + 0xC, base);
        put_le32(e.b + 0x10, track);
        put_le32(e.b + 0x14, raw);
        put_le32(e.b + 0x18, blocks);
        put_le32(e.b + 0x1C, sectors);
        std::memcpy(e.b + 0x30, r + 0xC, 0x10);
        std::memcpy(e.b + 0x40, r + 0x1C, 0x20);
    }
    return all_zero(blk + 0x130 + n * 0x100, (8 - n) * 0x100);
}

// Tail shared by FUN_100270a8, FUN_10030000 and FUN_100217e0.
void toc_build(DiscTable& t, const TocTrack* tracks, unsigned n, uint32_t total_sectors, uint32_t raw_base) {
    t.clear();
    t.set_track_count(n);
    put_le32(t.track(0) + 0x14, kTocOffset);  // lead-in INDEX 01
    for (unsigned i = 0; i < n; ++i) {
        const unsigned k = i + 1;
        t.set_size(k, tracks[i].size);
        t.set_file_offset(k, tracks[i].start);
        t.set_index_count(k, 1);
        t.set_index0(k, tracks[i].start + kTocOffset);
        t.set_index1(k, tracks[i].index1 + kTocOffset);
        t.set_type(k, tracks[i].data ? 2 : 1);
    }
    const unsigned out = n + 1;  // lead-out marker
    t.set_file_offset(out, total_sectors);
    t.set_index0(out, total_sectors + kTocOffset);
    t.set_index1(out, total_sectors + kTocOffset);
    t.set_has_data(true);
    t.set_tail_flag(true);
    t.raw_base = raw_base;
    t.end_sector = total_sectors + 0x2454;
}

// V3 FUN_100270a8
bool legacy_track_map(const DiscEntry& d, const BlockReader& rd, DiscTable& out) {
    const uint32_t n = d.track_count();
    if (n - 1 >= 0x62 || d.sectors() == 0) return false;
    uint8_t blk[kBlockBytes];
    if (!rd(d.track_lba(), 5, blk)) return false;
    if (!bytes_equal(blk, kTrk, 8) || blk[8] != n || blk[9] != 0x10) return false;
    const uint16_t sbi = le16(blk + 10);
    if (sbi >= 2) return false;
    if (le32(blk + 0xC) != crc32_hole(blk, 0x930, 0xC, 4) || !all_zero(blk + 0x930, 0xD0)) return false;

    TocTrack tr[0x62];
    uint32_t running = 0;
    bool any_data = false;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* e = blk + 0x10 + i * 0x10;
        const uint32_t start = le32(e), index1 = le32(e + 4), len = le32(e + 8);
        const uint32_t end = start + len;
        if (len == 0 || start != running || end < running || end > d.sectors()) return false;
        if (index1 < running || end <= index1) return false;
        if (e[0xC] > 2 || e[0xD] > 0xF || e[0xE] > 7 || e[0xF] != 0) return false;
        if (i == 0 && e[0xC] == 0) return false;  // the first track must be data
        any_data = any_data || e[0xC] != 0;
        tr[i] = TocTrack{len, start, index1, e[0xC] != 0};
        running = end;
    }
    if (!any_data || d.sectors() != running) return false;
    if (!all_zero(blk + 0x10 + n * 0x10, (99 - n) * 0x10)) return false;
    if (sbi != 0) {
        const uint32_t len = le16(blk + 0x640);
        if (len - 4 >= 0x2E5 || !bytes_equal(blk + 0x648, kSbi, 4)) return false;
        if (le32(blk + 0x644) != crc32(blk + 0x648, len)) return false;
    }
    toc_build(out, tr, n, d.sectors(), d.raw_lba());
    return true;
}

bool legacy_v1_mount(const uint8_t* hdr_blk, const BlockReader& rd, LegacyV1& out, DiscTable& table) {
    return legacy_v1_parse(hdr_blk, out) && legacy_track_map(out.discs[out.default_disc], rd, table);
}

// V3 FUN_1002773c, mode 1
SwapResult legacy_swap_v1(const LegacyV1& v1, uint32_t current, const BlockReader& rd, DiscTable& table) {
    SwapResult r;
    if (v1.disc_count < 2) return r;
    uint32_t next = current + 1;
    if (next == v1.disc_count) next = 0;
    r.next = next;
    r.entry = v1.discs[next];
    r.status = legacy_track_map(r.entry, rd, table) ? SwapStatus::Ok : SwapStatus::TrackMap;
    return r;
}

// V3 FUN_1002773c, mode 2
SwapResult legacy_swap_v2(const DiscEntry& cur, const SwapEnv& env, const BlockReader& rd, DiscTable& table) {
    SwapResult r;
    CaseView c;
    if (cur.case_index() == 0xFF || !env.case_by_index(env.ctx, cur.case_index(), c) || c.disc_count - 2u > 3u ||
        cur.slot() >= c.disc_count || c.disc_ids[cur.slot()] != cur.index()) {
        r.status = SwapStatus::CaseLookup;
        return r;
    }
    uint32_t next = cur.slot() + 1;
    if (next == c.disc_count) next = 0;
    r.next = next;
    if (!env.disc_by_id(env.ctx, c.disc_ids[next], r.entry) || r.entry.case_index() != c.index || r.entry.slot() != next) {
        r.status = SwapStatus::DiscLookup;
        return r;
    }
    r.status = legacy_track_map(r.entry, rd, table) ? SwapStatus::Ok : SwapStatus::TrackMap;
    return r;
}

// V3 FUN_10018ff2 (raw card branch of the disc loader)
bool raw_header_parse(const uint8_t* s, DiscTable& out) {
    if (!bytes_equal(s, kRaw, 6)) return false;
    const uint32_t n = s[0xC];
    if (n > kRawMaxTracks) return false;
    out.clear();
    out.set_track_count(n);
    put_le32(out.track(0) + 0x14, kTocOffset);  // lead-in INDEX 01
    uint32_t pos = kTocOffset;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* e = s + 0xD + i * 10;
        const uint32_t start = le32(e + 1), len = le32(e + 5), pregap = e[9];
        const unsigned k = i + 1;
        out.set_size(k, len);
        out.set_file_offset(k, start);
        out.set_index0(k, pos - pregap);
        out.set_index1(k, pos);
        out.set_type(k, e[0] == 1 ? 1 : e[0] == 2 ? 2 : 0);
        if (e[0] == 2) out.set_has_data(true);
        pos += len;
    }
    const unsigned lo = n + 1;
    out.set_file_offset(lo, pos - kTocOffset);
    out.set_index0(lo, pos);
    out.set_index1(lo, pos);
    out.set_tail_flag(true);
    out.end_sector = pos + kTocOffset;
    return true;
}

}  // namespace mh::aux
