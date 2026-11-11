// V2 card layer: header, Case and Disc validators, track map, mount, state events, library listing and the
// sector stub of PicoStation_MH_V3.uf2 (flash 0x10021274..0x10022bf4). Function names in comments are the
// Ghidra names (FUN_<flash address>). Layout reference: docs/re/CARD_LAYOUT_CROSSCHECK.md.
// The original card code is size optimised; keep the same shape on the device.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("Os")
#endif
#include "mh/card.h"

namespace mh::card {

namespace {
constexpr uint8_t kMagicPsx[6] = {'P', 'S', 'X', 'R', 'A', 'W'};  // 0x10023525
constexpr uint8_t kMagicCase[4] = {'C', 'A', 'S', 'E'};           // 0x10023520
constexpr uint8_t kMagicDisc[4] = {'D', 'I', 'S', 'C'};           // 0x1002352C
constexpr uint8_t kMagicTrk[8] = {'P', 'S', 'T', 'T', 'R', 'K', '1', 0};  // 0x100235D4

inline uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
inline uint32_t u32(const uint8_t* p) { return read_u32(p); }
}  // namespace

// ---------------------------------------------------------------------------------------------
// Header validator
// ---------------------------------------------------------------------------------------------

// V3 FUN_10021aa4 on a slot image
bool header_valid(const uint8_t* b) {
    if (!equal(b, kMagicPsx, 6)) return false;
    if (b[6] != 2 || !(b[7] & 1)) return false;
    const uint32_t cases = u16(b + 8), discs = u16(b + 0xA);
    if (cases > 0x100 || discs > 0x100) return false;
    if ((cases == 0) != (discs == 0)) return false;
    if (u32(b + 0x10) != 0x800) return false;
    if (u32(b + 0x14) != kHdrBLba || u32(b + 0x18) != 0) return false;
    if (u32(b + 0x1C) != kCaseLba || u32(b + 0x20) != 0) return false;
    if (u32(b + 0x24) != kDiscLba || u32(b + 0x28) != 0) return false;
    if (u32(b + 0x2C) != kArtLba || u32(b + 0x30) != 0) return false;
    if (u32(b + 0x34) != kMenuLba || u32(b + 0x38) != 0) return false;
    if (u32(b + 0x3C) - 1 >= 0x5D) return false;
    if (u32(b + 0x40) != kCfgLba || u32(b + 0x44) != 0) return false;
    if (u32(b + 0x48) != kArtSize || u32(b + 0x4C) != 0) return false;
    return crc32(b, kHeaderCrcOffset) == u32(b + kHeaderCrcOffset);
}

bool header_valid() { return header_valid(block_buffer()); }

// ---------------------------------------------------------------------------------------------
// Case and Disc records
// ---------------------------------------------------------------------------------------------

// V3 FUN_10021960
int validate_case(const uint8_t* r, uint32_t index, CaseInfo* out) {
    if (r[4] == 0) return all_zero(r, kCaseRecord) ? 0 : -1;
    if (!equal(r, kMagicCase, 4) || r[6] != index) return -1;
    const uint32_t n = r[5];
    if (n - 1 >= kMaxDiscsPerCase) return -1;
    if (!all_zero(r + 0x3F, 0xD)) return -1;
    if (static_cast<uint16_t>(u16(r + 0x38) + 1) >= 0x101) return -1;
    if (crc32(r, 0x4C) != u32(r + 0x4C)) return -1;
    const uint8_t* ids = r + 0x3A;
    for (uint32_t j = 1; j < n; ++j) {
        for (uint32_t k = 0; k < j; ++k) {
            if (ids[k] == ids[j]) return -1;
        }
    }
    for (uint32_t j = n; j < kMaxDiscsPerCase; ++j) {
        if (ids[j] != 0) return -1;
    }
    if (out) {
        zero_fill(out, sizeof(CaseInfo));
        out->index = static_cast<uint8_t>(index);
        out->disc_count = r[5];
        out->art_id = u16(r + 0x38);
        copy(out->ids, ids, 5);
        copy_printable(out->name, 0x21, r + 8, 0x20);
    }
    return 1;
}

// V3 FUN_10021d84 (record already read)
int validate_disc(const uint8_t* r, uint32_t id, uint32_t first_pkg_lba, DiscInfo* out) {
    if (r[4] == 0) return all_zero(r, kDiscRecord) ? 0 : -1;
    const uint32_t flags = u32(r + 8);
    const uint32_t ext_off = (flags & 2) ? 0x19 : 0xF;   // EXT layout: art at +0x1E, extra at +0x19
    const uint32_t cheat_cap = (flags & 2) ? 0x2800 : 0x1400;
    if (!equal(r, kMagicDisc, 4) || r[5] != id || flags >= 4) return -1;
    const uint32_t tracks = r[0x60];
    if (tracks - 1 >= kMaxTracks) return -1;
    const uint32_t sectors = u32(r + 0x5C);
    if (sectors == 0) return -1;
    if (u32(r + 0x40) != 0 || u32(r + 0x48) != 0 || u32(r + 0x50) != 0 || u32(r + 0x58) != 0) return -1;
    if (u32(r + 0x68) != 0 || u32(r + 0x74) != 0) return -1;
    const uint32_t base = u32(r + 0x3C);
    if (base < first_pkg_lba || (base & 0xFF) != 0) return -1;
    if (u32(r + 0x44) != base || u32(r + 0x4C) != base + 0x100) return -1;
    if (sectors > 0x19999999 || u32(r + 0x54) != sectors * 5) return -1;
    const uint16_t art = u16(r + 0x62);
    if (static_cast<uint16_t>(art + 1) >= 0x101) return -1;
    const uint32_t cheat_size = u32(r + 0x6C);
    if (cheat_size > cheat_cap) return -1;
    const uint32_t cheat_lba = u32(r + 0x64);
    if (cheat_lba == 0 ? cheat_size != 0 : cheat_lba != base + 5) return -1;
    const uint32_t extra_size = u32(r + 0x78);
    if (extra_size > 0xA00) return -1;
    const uint32_t extra_lba = u32(r + 0x70);
    if (extra_lba == 0 ? extra_size != 0 : extra_lba != base + ext_off) return -1;
    if (!all_zero(r + 0x94, 0x68)) return -1;
    const uint32_t rec_crc = crc32(r, 0xFC);
    if (rec_crc != u32(r + 0xFC)) return -1;
    if (out) {
        zero_fill(out, sizeof(DiscInfo));
        out->id = static_cast<uint8_t>(id);
        out->case_index = r[6];
        out->slot = r[7];
        out->track_count = r[0x60];
        out->art_id = art;
        out->flags = flags;
        out->base_lba = base;
        out->track_lba = u32(r + 0x44);
        out->raw_lba = u32(r + 0x4C);
        out->raw_blocks = u32(r + 0x54);
        out->sectors = sectors;
        out->cheat_lba = cheat_lba;
        out->cheat_size = cheat_size;
        out->data_crc32 = u32(r + 0x7C);
        out->record_crc = rec_crc;
        copy_printable(out->serial, 0x10, r + 0xC, 0x10);
        copy_printable(out->title, 0x21, r + 0x1C, 0x20);
    }
    return 1;
}

// V3 FUN_10021c50: the n-th used Case, scanning the table in groups of 32 records (one 5 block read)
bool nth_used_case(uint32_t n, CaseInfo* out) {
    CardState& st = state();
    if (n >= st.case_count) return false;
    uint32_t used = 0;
    for (uint32_t grp = 0; grp != 8; ++grp) {
        if (used >= st.case_count) return false;
        if (!read_blocks(grp * 5 + st.case_lba, 5)) return false;
        uint32_t idx = (grp & 7) << 5;
        const uint8_t* rec = block_buffer();
        for (uint32_t k = 0; k < 32 && used < st.case_count; ++k, rec += kCaseRecord) {
            const int r = validate_case(rec, idx, nullptr);
            if (r == -1) return false;
            if (r != 0) {
                const uint32_t here = used;
                used = (used + 1) & 0xFFFF;
                if (n == here) return validate_case(rec, idx, out) == 1;
            }
            idx = (idx + 1) & 0xFF;
        }
    }
    return false;
}

// V3 FUN_10021d50
bool case_by_index(uint32_t index, CaseInfo* out) {
    uint8_t rec[kCaseRecord];
    if (!read_bytes(state().case_lba, index * kCaseRecord, rec, kCaseRecord)) return false;
    return validate_case(rec, index, out) == 1;
}

// V3 FUN_10021d84
bool disc_by_id(uint32_t id, DiscInfo* out) {
    uint8_t rec[kDiscRecord];
    if (!read_bytes(state().disc_lba, id << 8, rec, kDiscRecord)) return false;
    return validate_disc(rec, id, state().art_size, out) == 1;
}

// V3 FUN_10021fbc
bool disc_of_case(uint32_t slot, DiscInfo* out) {
    CaseInfo ci;
    const uint32_t sel = state().sel_case;
    if (sel >= 0x100 || !case_by_index(sel & 0xFF, &ci) || slot >= ci.disc_count) return false;
    if (!disc_by_id(ci.ids[slot], out) || out->case_index != ci.index) return false;
    return out->slot == slot;
}

// ---------------------------------------------------------------------------------------------
// Track map
// ---------------------------------------------------------------------------------------------

// V3 FUN_10022084
bool load_track_map(const DiscInfo& d, CardDisc* out) {
    uint8_t* b = block_buffer();
    if (!read_slot(d.track_lba, 8) || !equal(b, kMagicTrk, 8)) return false;
    const uint32_t n = d.track_count;
    if (b[8] != n || b[9] != 0x10 || u16(b + 0xA) >= 2) return false;
    const uint32_t stored = u32(b + 0xC);
    if (stored != crc32_window(0x930, 0xC) && stored != crc32_window(0x640, 0xC) && stored != crc32(b + 0x10, 0x630))
        return false;
    if (n - 1 >= kMaxTracks) return false;  // not in the original (the caller checked it); guards the arrays

    struct Entry { uint32_t start, index1, len; uint8_t type; } e[kMaxTracks];
    uint32_t running = 0;
    bool has_data = false;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* p = b + 0x10 + i * 0x10;
        const uint32_t start = u32(p), index1 = u32(p + 4), len = u32(p + 8);
        const uint32_t end = start + len;
        const uint8_t type = p[0xC];
        if (len == 0 || start != running || end < running || index1 < running || end <= index1) return false;
        if (d.sectors < end || type > 2 || p[0xD] > 0xF || p[0xF] != 0) return false;
        if (i == 0) {
            if (type == 0) return false;
            has_data = true;
        } else {
            has_data = has_data || type != 0;
        }
        e[i] = {start, index1, len, type};
        running = end;
    }
    if (running != d.sectors || !has_data) return false;
    if (!all_zero(b + (n + 1) * 0x10, (kMaxTracks - n) * 0x10)) return false;
    if (!out) return true;

    zero_fill(&out->cue, sizeof(CardCue));
    out->cue.track_count = n;
    out->cue.tracks[0].indices[0] = 0;
    out->cue.tracks[0].indices[1] = kLeadIn;
    for (uint32_t i = 0; i < n; ++i) {
        CardTrack& t = out->cue.tracks[i + 1];
        t.size = e[i].len;
        t.file_offset = e[i].start;
        t.index_count = 1;
        t.indices[0] = e[i].start + kLeadIn;
        t.indices[1] = e[i].index1 + kLeadIn;
        t.track_type = 2 - (e[i].type == 0);
    }
    CardTrack& last = out->cue.tracks[n + 1];
    last.file_offset = running;
    last.indices[0] = running + kLeadIn;
    last.indices[1] = running + kLeadIn;
    out->has_data = 1;
    out->current_track = 0;
    out->sd_menu = 1;
    return true;
}

// ---------------------------------------------------------------------------------------------
// State and events
// ---------------------------------------------------------------------------------------------

// V3 FUN_100217b0
void reset_response_cache() {
    state().cache_key = 0xFFFFFFFFu;
    ResponseCache& c = response_cache();
    for (uint32_t& s : c.slot) s = 0xFFFFFFFEu;
    c.last = 0xFFFFFFFFu;
}

// V3 FUN_10021858
void post_event(uint8_t code, uint8_t arg, uint32_t extra) {
    CardEvent& e = state().event;
    e.magic = 0x31534C50;  // "PLS1"
    e.version = 1;
    e.length = 0x28;
    e.seq = e.seq == 0xFFFFFFFFu ? 1 : e.seq + 1;
    e.extra = extra;
    e.code = code;
    e.arg = arg;
    e.flags |= 4;
    e.crc = crc32(&e, 0x24);
    reset_response_cache();
}

// V3 FUN_1002189c
void post_event_ack(uint8_t arg, uint32_t extra) { post_event(8, arg, extra); }

// V3 FUN_100217e0: the menu is disc 0 of the table, one data track of 901 sectors, raw blocks from menu_lba
void default_disc(CardDisc* disc) {
    zero_fill(&disc->cue, sizeof(CardCue));
    CardCue& c = disc->cue;
    c.track_count = 1;
    c.tracks[0].indices[1] = kLeadIn;
    c.tracks[1].size = kDefaultDiscSectors;
    c.tracks[1].index_count = 1;
    c.tracks[1].indices[0] = kLeadIn;
    c.tracks[1].indices[1] = kLeadIn;
    c.tracks[1].track_type = 2;
    c.tracks[2].file_offset = kDefaultDiscSectors;
    c.tracks[2].indices[0] = kLeadIn + kDefaultDiscSectors;
    c.tracks[2].indices[1] = kLeadIn + kDefaultDiscSectors;
    disc->has_data = 1;
    disc->current_track = 0;
    disc->sd_menu = 1;
    CardGlobals& g = globals();
    g.raw_base = state().menu_lba;
    g.sector_cursor = 0;
    g.total_sectors = kDefaultDiscSectors + 2 * kLeadIn;
}

// ---------------------------------------------------------------------------------------------
// Mount
// ---------------------------------------------------------------------------------------------
namespace {

// Legacy "PSXRAW" version 1, single disc: header 0x930 bytes in the block buffer, one Disc record at +0x130.
// Part of V3 FUN_10021280.
bool mount_legacy(CardDisc* disc) {
    const uint8_t* b = block_buffer();
    if (!equal(b, kMagicPsx, 6) || b[7] != 1 || b[8] != 0 || b[9] != 0 || u16(b + 0xA) != 0x930) return false;
    const uint32_t hdr_crc = u32(b + 0x10);
    if (hdr_crc != crc32_window(0x930, 0x10) && hdr_crc != crc32_window(0x130, 0x10)) return false;
    const uint8_t* r = b + 0x130;
    if (!equal(r, kMagicDisc, 4) || r[4] != 1 || r[5] != 0 || r[6] != 0xFF || r[7] != 0) return false;
    if (u32(r + 0x40) != 0 || u32(r + 0x48) != 0 || u32(r + 0x50) != 0 || u32(r + 0x58) != 0) return false;
    const uint32_t track_lba = u32(r + 0x44);
    if (track_lba != u32(r + 0x3C)) return false;
    const uint32_t raw_lba = u32(r + 0x4C);
    if (raw_lba != track_lba + 5) return false;
    const uint32_t tracks = r[0x60];
    if (tracks - 1 >= kMaxTracks) return false;
    const uint32_t sectors = u32(r + 0x5C);
    if (sectors - 1 > 0x19999998) return false;
    const uint32_t raw_blocks = u32(r + 0x54);
    if (raw_blocks != sectors * 5) return false;
    if (u32(r + 0xFC) != crc32(r, 0xFC)) return false;

    DiscInfo d;
    zero_fill(&d, sizeof d);
    d.case_index = 0xFF;
    d.track_count = static_cast<uint8_t>(tracks);
    d.art_id = 0xFFFF;
    d.track_lba = track_lba;
    d.raw_lba = raw_lba;
    d.raw_blocks = raw_blocks;
    d.sectors = sectors;
    if (!load_track_map(d, disc)) return false;
    CardGlobals& g = globals();
    g.raw_base = raw_lba;
    g.sector_cursor = 0;
    g.total_sectors = sectors + 2 * kLeadIn;
    state().phase = kPhaseNone;
    return true;
}

}  // namespace

// V3 FUN_10021280
bool mount(CardDisc* disc) {
    if (!disc) return false;
    CardState& st = state();
    zero_fill(&st, sizeof st);
    st.magic = kStateMagic;
    st.disc_image_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(disc));
    st.cache_key = 0xFFFFFFFFu;
    st.sel_case = 0xFFFFFFFFu;
    if (!sd().init || sd().init() != 0) return false;

    if (read_slot(0, 0x10) && block_buffer()[6] == 1) return mount_legacy(disc);

    uint8_t head[kSlotUsed];  // the chosen header (the original keeps it in the response buffer)
    bool have = false;
    if (read_slot(0, 0x10) && header_valid()) {
        copy(head, block_buffer(), kSlotUsed);
        have = true;
    }
    if (!read_slot(kHdrBLba, 0x10) || !header_valid()) {
        if (!have) return false;
    } else {
        bool take_b = true;
        if (have) {
            // B wins only when its generation is newer in 32 bit wrap around order: (int32)(B - A) > 0
            const uint32_t a = u32(head + 0xC), bgen = u32(block_buffer() + 0xC);
            if (a == bgen) take_b = false;
            else if (a < bgen) take_b = !(static_cast<int32_t>(bgen - a) < 0);
            else take_b = !(a - bgen < 0x80000001u);
        }
        if (take_b) copy(head, block_buffer(), kSlotUsed);
    }

    st.generation = u32(head + 0xC);
    st.case_count = u16(head + 8);
    st.disc_count = u16(head + 0xA);
    st.case_lba = u32(head + 0x1C);
    st.disc_lba = u32(head + 0x24);
    st.art_lba = u32(head + 0x2C);
    st.menu_lba = u32(head + 0x34);
    st.menu_sectors = u32(head + 0x3C);
    st.cfg_lba = u32(head + 0x40);
    st.art_size = u32(head + 0x48);
    st.crc_cases = u32(head + 0x50);
    st.crc_discs = u32(head + 0x54);
    st.crc_menu = u32(head + 0x5C);
    st.valid = 1;
    zero_fill(&st.event, sizeof st.event);
    st.event.flags = 8;
    post_event(0, 0, 0);
    default_disc(disc);
    st.phase = kPhaseMenu;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Library listing
// ---------------------------------------------------------------------------------------------

// V3 FUN_10022284
bool listing_append(uint32_t* pos, const char* name, uint8_t flag) {
    uint32_t n = 0;
    while (n != 0x60) {
        if (name[n] == 0) {
            if (n == 0) return false;
            break;
        }
        ++n;
    }
    if (*pos + 6 + n > kRespSize) return false;
    uint8_t* r = response();
    r[*pos] = static_cast<uint8_t>(n);
    r[*pos + 1] = flag;
    copy(r + *pos + 2, name, n);
    *pos += 2 + n;
    return true;
}

namespace {
// V3 FUN_10021a70
void listing_trailer(uint32_t pos, uint8_t err, uint32_t count) {
    if (pos + 4 > kRespSize) return;
    uint8_t* r = response();
    r[pos] = 0;
    r[pos + 1] = err;
    r[pos + 2] = static_cast<uint8_t>(count >> 8);
    r[pos + 3] = static_cast<uint8_t>(count);
}

// "<title> - DISCO <n>" cut at 0x47 characters, as built inline by FUN_100222e0
uint32_t disc_entry_name(char* buf, const char* title, bool numbered, uint32_t number) {
    uint32_t len = 0;
    while (title[len] != 0 && len != kListNameCap) {
        buf[len] = title[len];
        ++len;
    }
    if (numbered) {
        static const char kSuffix[] = " - DISCO ";
        for (uint32_t i = 0; i < 9 && len != kListNameCap; ++i) buf[len++] = kSuffix[i];
        char digits[12];
        uint32_t nd = 0;
        uint32_t v = number;
        do {
            digits[nd++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v != 0 && nd != 10);
        while (nd != 0 && len < kListNameCap) buf[len++] = digits[--nd];
    }
    buf[len] = 0;
    return len;
}
}  // namespace

// V3 FUN_100222e0
void library_listing() {
    CardState& st = state();
    zero_fill(response(), kRespSize);
    uint32_t pos = 0, count = 1;
    uint8_t err = 0;

    if (st.valid != 1) {
        listing_append(&pos, "ERRO RAW INVALIDO", 1);
    } else if (st.sel_case > 0xFF) {
        // every used Case, from the list_offset-th one on
        uint32_t used = 0, listed = 0;
        for (uint32_t grp = 0; used < st.case_count && read_blocks(grp * 5 + st.case_lba, 5);) {
            uint32_t idx = (grp & 7) << 5;
            const uint8_t* rec = block_buffer();
            err = 0;
            for (uint32_t k = 0; k < 32 && used < st.case_count; ++k, rec += kCaseRecord) {
                CaseInfo ci;
                if (validate_case(rec, idx, &ci) == 1) {
                    const uint32_t here = used;
                    used = (used + 1) & 0xFFFF;
                    if (here >= st.list_offset) {
                        if (!listing_append(&pos, ci.name[0] ? ci.name : "CASE PS1", 1)) {
                            err = 1;
                            break;
                        }
                        listed = (listed + 1) & 0xFFFF;
                    }
                }
                idx = (idx + 1) & 0xFF;
            }
            if (grp == 7 || (++grp, err != 0)) break;
        }
        count = st.list_offset == 0 ? (st.case_count & 0xFFFF) : listed;
    } else {
        CaseInfo ci;
        if (case_by_index(st.sel_case & 0xFF, &ci)) {
            uint32_t listed = 0;
            for (uint32_t slot = st.list_offset & 0xFFFF; slot < ci.disc_count; slot = (slot + 1) & 0xFFFF) {
                DiscInfo di;
                if (!disc_by_id(ci.ids[slot], &di) || di.case_index != ci.index || di.slot != slot) continue;
                char name[kListNameCap + 1];
                disc_entry_name(name, di.title[0] ? di.title : "JOGO PS1", ci.disc_count != 1, slot + 1);
                if (!listing_append(&pos, name, 1)) {
                    err = 1;
                    break;
                }
                listed = (listed + 1) & 0xFFFF;
            }
            count = st.list_offset != 0 ? listed : ci.disc_count;
        } else {
            listing_append(&pos, "CASE INVALIDA", 1);
        }
    }
    listing_trailer(pos, err, count);
}

// V3 FUN_10022518 (failure branch)
bool disc_info_request(uint32_t slot, DiscInfo* out) {
    zero_fill(response(), kRespSize);
    if (state().valid == 1 && disc_of_case(slot, out)) return true;
    uint8_t* r = response();
    store_u32(r, 0x314D4750);  // "PGM1"
    r[4] = 1;
    r[5] = 3;
    r[6] = static_cast<uint8_t>(slot);
    r[7] = static_cast<uint8_t>(slot >> 8);
    store_u32(r + 0x100, crc32(r, 0x100));
    post_event_ack(10, slot);
    return false;
}

// ---------------------------------------------------------------------------------------------
// Sector stub (flash 0x10021640..0x100216d8, RAM copy 0x20033000 on the original)
// ---------------------------------------------------------------------------------------------

SectorRoute __not_in_flash_func(sector_route)(uint32_t lba) {
    CardState& st = state();
    bool builtin_first = false;
    switch (st.phase) {
    case kPhaseMenu:
        builtin_first = lba >= 0xF3;
        break;
    case kPhaseGame:
        builtin_first = lba == 0x41A;
        break;
    case kPhaseSwitch:
        if (lba == 0x41A) {
            builtin_first = true;
        } else {
            st.phase = kPhaseFault;
            ResponseCache& c = response_cache();  // only the table, not state.cache_key
            for (uint32_t& s : c.slot) s = 0xFFFFFFFEu;
            c.last = 0xFFFFFFFFu;
        }
        break;
    default:
        break;
    }
    if (st.phase == kPhaseMenu && st.magic == kStateMagic && st.menu_sectors == 0) return SectorRoute::kBuiltin;
    return builtin_first ? SectorRoute::kBuiltinThenCard : SectorRoute::kCard;
}

uint32_t __not_in_flash_func(sector_block)(uint32_t raw_base, uint32_t lba) { return raw_base + (lba - kFirstDataLba) * 5; }

}  // namespace mh::card
