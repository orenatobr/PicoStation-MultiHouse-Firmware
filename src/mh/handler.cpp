// PS1 command handler of PicoStation_MH_V3. Original functions:
//   FUN_100228c8 main handler, FUN_100229f8 send tail, FUN_10022bf4 copy+send tail,
//   FUN_10022518 + FUN_10029000/FUN_10029020 disc info (PGM1),
//   FUN_100227cc PS1 command decoder,
//   FUN_1002d02c DEAD hook, FUN_1002f118 N=900 reject, FUN_1002e100 v6 cheat fast path,
//   FUN_1002f018/f050/f0b0/f000 and FUN_1002e0a0 confirmation (0xF9) glue.
// Evidence and wire formats: docs/re/handler.md.
#include "mh/handler.h"

namespace mh::handler {

namespace {

using card::CardState;
using card::state;

constexpr uint32_t kRespBytes = card::kRespSize;  // 0x914
constexpr uint32_t kNoneKey = 0xFFFFFFFFu;

// Validation cache of the v6 fast path (words 0x20037fe0..0x20037ff4 on the original).
struct V6Cache {
    uint32_t magic;
    uint32_t cheat_lba;
    uint32_t cheat_size;
    uint32_t token;
    uint32_t seq;
    uint32_t slot;
};

struct Globals {
    card::CardDisc* disc;
    Env env;
    V6Cache cache;
    alignas(4) uint8_t blob[0x2800];  // 0x20035000: cheat blob read from the card
};
Globals g;

inline uint8_t* resp() { return card::response(); }
inline void put16(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put32(uint8_t* p, uint32_t v) { card::store_u32(p, v); }

// FUN_100229f8: the response is final; remember which sector it holds. After a session page that reports
// event code 9 the PS1 acknowledged the switch: count it and move to phase 4.
bool finish(uint32_t n, const uint8_t** out) {
    CardState& st = state();
    st.cache_key = n;
    if (n == 900 && st.event.code == kEvSessionDone) {
        st.switch_count = st.switch_count + 1;
        st.phase = card::kPhaseSwitch;
    }
    *out = resp();
    return true;
}

// FUN_1002f018: a game runs and the last event is "disc started" (2) or "confirm armed" (4).
bool confirm_ready() {
    const CardState& st = state();
    if (st.magic != card::kStateMagic || st.phase != card::kPhaseGame) return false;
    return st.event.code == kEvSwitched || st.event.code == kEvAck4;
}

// ---------------------------------------------------------------------------------------------
// Request side: library list / art selection / session requests
// ---------------------------------------------------------------------------------------------
// LAB_1002293e: the request is consumed and every cached sector is dropped.
void finish_request() {
    state().cmd = kCmdNone;
    card::reset_response_cache();
}

// N=100 with a pending kCmdListAll .. kCmdListOffset.
void apply_list_request(uint32_t cmd, uint32_t arg) {
    CardState& st = state();
    if (cmd <= kCmdListAll2) {
        st.sel_case = kNoneKey;
        st.list_offset = 0;
    } else if (cmd == kCmdSelectCase) {
        card::CaseInfo c;
        st.sel_case = card::nth_used_case(arg & 0xFFFF, &c) ? c.index : kNoneKey;
        st.list_offset = 0;
    } else {
        st.list_offset = arg;
    }
}

// Art window with kCmdArtSelect: art block of the selected Case (n-th used) or of Disc `arg` of the
// selected Case; 0 when there is none.
void apply_art_request(uint32_t arg) {
    CardState& st = state();
    uint32_t src = 0;
    if (st.sel_case < 0x100) {
        card::DiscInfo d;
        if (card::disc_of_case(arg & 0xFFFF, &d) && d.art_id != 0xFFFF) src = d.base_lba + ((d.flags & 2) ? 0x1E : 0x14);
    } else {
        card::CaseInfo c;
        if (card::nth_used_case(arg & 0xFFFF, &c) && c.art_id != 0xFFFF) src = st.art_lba + static_cast<uint32_t>(c.art_id) * 0x14;
    }
    st.art_block = src;
}

// ---------------------------------------------------------------------------------------------
// Response builders
// ---------------------------------------------------------------------------------------------
// N = 0x96: "CFG1" page, the first 0x800 bytes of the PSTCFG2 slot, or "DEAD" when it cannot be read.
void build_config_page() {
    uint8_t* r = resp();
    card::zero_fill(r, kRespBytes);
    if (card::read_bytes(state().cfg_lba, 0, r + 0x114, 0x800)) {
        put32(r, 0x31474643);  // "CFG1"
        put16(r + 4, 0x800);
    } else {
        put32(r, 0x44414544);  // "DEAD"
    }
}

// Art chunk k = lba - 0x15E: "PART" [k][5] data, or "FART" when there is nothing to send.
void build_art_chunk(uint32_t lba) {
    CardState& st = state();
    uint8_t* r = resp();
    const uint32_t k = (lba - kLbaArt) & 0xFFFF;
    const uint32_t off = k * kArtChunkBytes;
    uint32_t remaining = off < kArtTotal ? kArtTotal - off : 0;
    card::zero_fill(r, kRespBytes);
    if (st.art_block != 0 && remaining != 0) {
        if (kArtChunkBytes < remaining) remaining = kArtChunkBytes;
        if (card::read_bytes(st.art_block, off, r + 6, remaining)) {
            put32(r, 0x54524150);  // "PART"
            r[4] = static_cast<uint8_t>(k);
            r[5] = 5;
            return;
        }
    }
    put32(r, 0x54524146);  // "FART"
}

// N = 900: the PLS1 event page, with the event CRC refreshed.
void build_session_page() {
    CardState& st = state();
    uint8_t* r = resp();
    card::zero_fill(r, kRespBytes);
    st.event.crc = card::crc32(&st.event, 0x24);
    card::copy(r, &st.event, sizeof(st.event));
}

// PCH2 header + data already placed at +0x30 (FUN_100228c8 tail at LAB_10022db4, FUN_1002e100 at LAB_1002e28e).
void write_chunk_header(uint32_t chunk, uint32_t len, uint8_t status, bool last) {
    CardState& st = state();
    uint8_t* r = resp();
    card::zero_fill(r, 0x30);
    put32(r, kMagicPch2);
    put16(r + 4, 0x30);
    put16(r + 6, chunk);
    put16(r + 8, len);
    r[0xA] = status;
    r[0xB] = last;
    put16(r + 0xC, st.sel_slot);
    put32(r + 0x10, st.event.reserved14);  // token
    put32(r + 0x14, st.event.seq);
    card::copy(r + 0x18, st.disc.serial, 16);
    put32(r + 0x28, card::crc32(r + 0x30, len));
    put32(r + 0x2C, card::crc32(r, 0x2C));
}

void chunk_error(uint32_t chunk, uint8_t status) {
    card::zero_fill(resp(), kRespBytes);
    write_chunk_header(chunk, 0, status, true);
}

// PCR2 request validation shared by both cheat paths (the v6 path additionally allows no cache).
// Request: "PCR2", u16 0x24, u16 selected slot, u32 token, u32 event seq, 16 byte serial, u32 CRC of 0..0x1f.
bool request_valid() {
    const CardState& st = state();
    if (st.pcr2_ready == 0) return false;
    const uint8_t* q = st.pcr2;
    if (card::read_u32(q) != kMagicPcr2 || (q[4] | q[5] << 8) != 0x24) return false;
    if (static_cast<uint32_t>(q[6] | q[7] << 8) != st.sel_slot) return false;
    if (card::read_u32(q + 8) != st.event.reserved14) return false;
    if (card::read_u32(q + 0xC) != st.event.seq) return false;
    if (!card::equal(q + 0x10, st.disc.serial, 16)) return false;
    return card::read_u32(q + 0x20) == card::crc32(q, 0x20);
}

// Main handler chunk path for version 4 blobs (FUN_100228c8 LAB_10022b9x .. LAB_10022db4).
void build_cheat_chunk_main(uint32_t chunk) {
    CardState& st = state();
    card::zero_fill(resp(), kRespBytes);
    if (!request_valid() || st.event.code != 0) return chunk_error(chunk, kChunkInvalid);
    if ((st.event.flags & 1) == 0) return chunk_error(chunk, kChunkInvalid);
    if (st.cheat_pending == 0) {  // first request of this session: validate and render the whole blob
        st.cheat_pending = 1;
        st.cheat_status = kChunkInvalid;
        st.cheat_bytes = 0;
        card::zero_fill(g.blob, sizeof(g.blob));
        card::zero_fill(card::text().data, card::kTextSize);
        const card::DiscInfo& d = st.disc;
        const uint32_t cap = (d.flags & 2) ? kBlobMaxExt : kBlobMaxV4Plain;
        if (d.cheat_lba == 0 || d.cheat_size < kBlobHeader || d.cheat_size > cap) {
            st.cheat_status = kChunkNoCheats;
        } else if (!card::read_bytes(d.cheat_lba, 0, g.blob, d.cheat_size)) {
            st.cheat_status = kChunkPastEnd;
        } else if (v4_validate(g.blob, d.cheat_size) && v4_render(g.blob, card::text())) {
            st.cheat_bytes = card::text().len;
            st.cheat_status = kChunkOk;
        } else {
            st.cheat_status = kChunkInvalid;
            st.cheat_bytes = 0;
        }
    }
    if ((st.cheat_status & 0xFF) != 0) return chunk_error(chunk, static_cast<uint8_t>(st.cheat_status));
    const uint32_t start = chunk * kChunkBytes;
    if (start >= st.cheat_bytes) return chunk_error(chunk, kChunkPastEnd);
    uint32_t len = st.cheat_bytes - start;
    const bool last = len <= kChunkBytes;
    if (len > kChunkBytes) len = kChunkBytes;
    card::copy(resp() + 0x30, card::text().data + start, len);
    write_chunk_header(chunk, len, kChunkOk, last);
}

// ---------------------------------------------------------------------------------------------
// Front chain hooks
// ---------------------------------------------------------------------------------------------
// FUN_1002d02c: while the menu waits for a fresh listing (cmd 1 or 2), probe the SD; "DEAD" when it fails.
bool hook_dead(uint32_t lba, bool loc1, const uint8_t** out) {
    CardState& st = state();
    if (!(loc1 && lba == kLbaListing && st.phase == card::kPhaseMenu && st.magic == card::kStateMagic)) return false;
    if (st.cmd != kCmdListAll && st.cmd != kCmdListAll2) return false;
    g.cache.magic = 0;
    if (g.env.service_busy) return false;
    const auto init = card::sd().init;
    const int dead = init ? init() : 0;
    st.cache_key = kNoneKey;
    if (dead == 0) return false;
    st.cmd = 0;
    card::zero_fill(resp(), kRespBytes);
    put32(resp(), 0x44414544);  // "DEAD"
    *out = resp();
    return true;
}

// FUN_1002f118: after the PS1 confirmed a switch with 0xF9 the session page is left to the disc image.
bool hook_reject_session(uint32_t lba, bool loc1) {
    const CardState& st = state();
    if (!loc1 || lba != kLbaSession || !confirm_ready()) return false;
    if (st.cmd != 0 || static_cast<int32_t>(st.cmd_arg) >= 0) return false;
    return (st.cmd_arg & 0x7FFF) == st.event.session;
}

// FUN_1002e100 (the unreferenced twin FUN_1002c0a0 additionally requires cmd == 11 and ignores `hold`):
// cheat chunks 0..99 of version 6 blobs, rendered on the fly from a validated blob. Version 4 blobs are
// left to the main handler.
bool hook_cheat_v6(uint32_t lba, bool loc1, const uint8_t** out) {
    CardState& st = state();
    if (!loc1 || st.phase != card::kPhaseMenu || st.magic != card::kStateMagic) return false;
    const uint32_t chunk = lba - kLbaCheat;
    if (chunk >= kCheatChunksV6 || g.env.hold) return false;
    const uint32_t cheat_lba = st.disc.cheat_lba, size = st.disc.cheat_size;
    uint8_t status = kChunkInvalid;
    uint32_t len = 0;
    bool last = true;
    const bool request_ok = request_valid() && st.event.code == 0 && (st.event.flags & 1) != 0;
    if (request_ok && (cheat_lba == 0 || size == 0)) {
        status = kChunkNoCheats;
    } else if (request_ok && size - kBlobHeader <= 0x27C0) {  // unsigned: also rejects size < 0x40
        bool ok = lba != kLbaCheat && g.cache.magic == kMagicPhc6 && g.cache.cheat_lba == cheat_lba &&
                  g.cache.cheat_size == size && g.cache.token == st.event.reserved14 &&
                  g.cache.seq == st.event.seq && g.cache.slot == st.sel_slot;
        if (!ok) {
            g.cache.magic = 0;
            const auto rd = card::sd().read;
            if (rd(cheat_lba, 1, g.blob) != 0) {
                status = kChunkPastEnd;
            } else {
                const uint32_t version = g.blob[8] | g.blob[9] << 8;
                if (version == 4) return false;
                if (version == 6) {
                    if (size + 0x1FF > 0x3FF && rd(cheat_lba + 1, ((size + 0x1FF) >> 9) - 1, g.blob + 0x200) != 0) {
                        status = kChunkPastEnd;
                    } else if (v6_validate(g.blob, size)) {
                        g.cache = V6Cache{kMagicPhc6, cheat_lba, size, st.event.reserved14, st.event.seq, st.sel_slot};
                        ok = true;
                    }
                }
            }
        }
        if (ok) {
            card::zero_fill(resp(), kRespBytes);
            const ChunkOut c = v6_render_chunk(g.blob, size, chunk, resp() + 0x30);
            len = c.len;
            last = c.last;
            status = len != 0 ? kChunkOk : kChunkPastEnd;
        }
    }
    if (status != kChunkOk) {
        card::zero_fill(resp(), kRespBytes);
        len = 0;
    }
    write_chunk_header(chunk, len, status, last);
    *out = resp();
    return true;
}

}  // namespace

Env& env() { return g.env; }

void init(card::CardDisc* disc) { g.disc = disc; }

// ---------------------------------------------------------------------------------------------
// PS1 command decoder (FUN_100227cc)
// ---------------------------------------------------------------------------------------------
void on_pad_command(uint32_t word) {
    CardState& st = state();
    if (st.magic != card::kStateMagic || st.phase == 0) return;
    if ((word & 0xF00000) != 0xF00000) return;
    const uint32_t arg = word & 0xFFFF;
    st.cmd_arg = arg;
    bool set_cmd = true;
    uint32_t cmd = 0;
    switch ((word & 0xFFFFF) >> 16) {
        case 0x0: cmd = kCmdNone; break;
        case 0x1: st.list_offset = 0; cmd = kCmdListAll; break;
        case 0x2: st.list_offset = 0; cmd = kCmdListAll2; break;
        case 0x3: cmd = kCmdSelectCase; break;
        case 0x4: st.list_offset = arg; cmd = kCmdListOffset; break;
        case 0x5: cmd = kCmdSwitch; break;
        case 0x6:  // PCR2 receive control: 2 starts a packet, 0 aborts
            st.pcr2_mode = arg;
            if (arg == 2) {
                card::zero_fill(st.pcr2, sizeof(st.pcr2));
                st.pcr2_fill = 0;
                st.cheat_pending = 0;
                st.pcr2_ready = 0;
            } else if (arg == 0) {
                st.pcr2_ready = 0;
                st.cmd = 0;
            }
            set_cmd = false;
            break;
        case 0x7:  // two bytes of the PCR2 packet, big endian
            if (st.pcr2_mode == 2 && st.pcr2_fill < sizeof(st.pcr2)) {
                uint32_t fill = st.pcr2_fill;
                st.pcr2[fill++] = static_cast<uint8_t>(arg >> 8);
                if (fill < sizeof(st.pcr2)) st.pcr2[fill++] = static_cast<uint8_t>(arg);
                st.pcr2_fill = fill;
                if (fill == sizeof(st.pcr2)) {
                    st.pcr2_ready = 1;
                    st.cmd = kCmdPcr2;
                    st.cheat_pending = 0;
                    st.pcr2_mode = 0;
                }
            }
            set_cmd = false;
            break;
        case 0xB: cmd = kCmdArtSelect; break;
        case 0xC: cmd = kCmdInfo; break;
        case 0xD: cmd = kCmdConfirm; break;
        case 0xE:
            st.cmd_arg = word & 0x7FFF;
            cmd = (word & 0x8000) ? kCmdCancel : kCmdFinish;
            break;
        default: set_cmd = false; break;
    }
    if (set_cmd) st.cmd = cmd;
    card::reset_response_cache();
}

// FUN_1002f050: command 0xF9xxxx whose low 15 bits equal the session id arms the confirmation (cmd 12).
void on_confirm_command(uint32_t word) {
    CardState& st = state();
    if ((word & 0xFF0000) != 0xF90000 || !confirm_ready() || st.cmd != 0) return;
    if (st.event.session == 0 || (word & 0x7FFF) != st.event.session) return;
    st.cmd_arg = word & 0xFFFF;
    st.cmd = 12;
}

// FUN_1002f0b0: the armed confirmation is judged on the next poll. Bit 15 of the word set and the same
// session id: cmd_arg gets bit 31 ("confirmed"); anything else leaves only the 16 bit word. cmd is freed.
void finish_confirm() {
    CardState& st = state();
    if (st.magic != card::kStateMagic || st.cmd != 12) return;
    card::reset_response_cache();
    const uint32_t a = st.cmd_arg;
    if (confirm_ready() && (a & 0x7FFF) == st.event.session && (a & 0x8000)) {
        st.cmd_arg = a | 0x80000000u;
    } else {
        st.cmd_arg = a & 0xFFFF;
    }
    st.cmd = 0;
}

// FUN_1002e0a0.
void abort_cheat_request() {
    CardState& st = state();
    if (st.phase == card::kPhaseMenu && st.magic == card::kStateMagic && st.cmd == kCmdPcr2) {
        st.cmd = 0;
        g.cache.magic = 0;
        card::reset_response_cache();
    }
}

// ---------------------------------------------------------------------------------------------
// Disc info (FUN_10022518 / FUN_10029020)
// ---------------------------------------------------------------------------------------------
bool serial_valid(const char* s) {
    for (int i = 0; i < 4; ++i) {
        if (static_cast<uint32_t>(static_cast<uint8_t>(s[i])) - 'A' > 25u) return false;
    }
    if (s[4] != '-') return false;
    for (int i = 5; i < 10; ++i) {
        if (static_cast<uint32_t>(static_cast<uint8_t>(s[i])) - '0' > 9u) return false;
    }
    return s[10] == 0;
}

uint32_t disc_token(uint32_t slot, const card::DiscInfo& d, uint32_t generation) {
    const uint32_t x = (slot << 8) ^ generation ^ d.record_crc ^ d.data_crc32 ^ (static_cast<uint32_t>(d.id) << 24);
    return x == kMagicSel1 ? 1u : x ^ kMagicSel1;
}

void build_boot_path(const char* s, char* out) {
    static const char kCdrom[] = "cdrom:\\";
    int n = 0;
    for (; n < 7; ++n) out[n] = kCdrom[n];
    for (int i = 0; i < 4; ++i) out[n++] = s[i];
    out[n++] = '_';
    for (int i = 5; i < 8; ++i) out[n++] = s[i];
    out[n++] = '.';
    out[n++] = s[8];
    out[n++] = s[9];
    out[n++] = ';';
    out[n++] = '1';
    out[n] = 0;
}

void build_disc_info(uint32_t slot) {
    CardState& st = state();
    uint8_t* r = resp();
    card::zero_fill(r, kRespBytes);
    card::DiscInfo d;
    if (!card::disc_info_request(slot, &d)) return;  // error page + event 10 built by the card layer

    const uint32_t token = disc_token(slot, d, st.generation);
    const bool serial_ok = serial_valid(d.serial);
    card::copy(&st.disc, &d, sizeof(d));
    st.sel_disc_id = d.id;
    st.sel_slot = slot;
    card::zero_fill(st.game_id, sizeof(st.game_id));
    if (serial_ok) build_boot_path(d.serial, reinterpret_cast<char*>(st.game_id));

    put32(r, kMagicPgm1);
    r[4] = 1;
    r[5] = serial_ok ? 1 : 2;  // status: 1 ok, 2 no usable serial (no boot path), 3 invalid (card layer)
    put16(r + 6, slot);
    put32(r + 8, token);
    r[0xC] = serial_ok ? static_cast<uint8_t>((d.cheat_size != 0) + 2) : 2;
    if (serial_ok) card::copy(r + 0x10, d.serial, 16);
    uint32_t n = 0;
    while (n < 32 && d.title[n] != 0) ++n;
    card::copy(r + 0x20, d.title, n);
    if (serial_ok) {
        n = 0;
        while (n < 0x7F && st.game_id[n] != 0) ++n;
        card::copy(r + 0x80, st.game_id, n);
    }
    put32(r + 0x100, card::crc32(r, 0x100));

    // Event bookkeeping: session id, selected slot, token, generation and data CRC, flags 9, then a post.
    card::CardEvent& ev = st.event;
    ev.reserved14 = token;
    ev.session = static_cast<uint16_t>(((token >> 16) ^ token) & 0x7FFF);
    if (ev.session == 0) ev.session = 1;
    ev.token = static_cast<uint16_t>(slot);
    ev.reserved18 = st.generation;
    ev.reserved1c = d.data_crc32 != 0 ? d.data_crc32 : d.record_crc;
    ev.flags = 9;
    card::post_event(0, 0, 0);
}

// ---------------------------------------------------------------------------------------------
// Session machine (N = 900 with a pending kCmdSwitch .. kCmdCancel)
// ---------------------------------------------------------------------------------------------
void run_session_request() {
    CardState& st = state();
    card::CardEvent& ev = st.event;
    const uint32_t arg = st.cmd_arg;
    auto bad_session = [&] { return ev.session != static_cast<uint16_t>(arg); };
    auto back_to_menu = [&] {
        if (g.disc) card::default_disc(g.disc);
        st.phase = card::kPhaseMenu;
    };
    switch (st.cmd) {
        case kCmdSwitch:
            if ((ev.flags & 1) == 0 || arg != st.sel_slot) {
                card::post_event_ack(2, arg);
                break;
            }
            card::post_event(kEvSwitchArmed, 0, 0);
            if (g.disc && card::load_track_map(st.disc, g.disc)) {
                card::CardGlobals& cg = card::globals();
                cg.raw_base = st.disc.raw_lba;
                cg.sector_cursor = 0;
                cg.total_sectors = st.disc.sectors + 2 * card::kLeadIn;
                st.phase = card::kPhaseGame;
                ev.flags |= 2;
                card::post_event(kEvSwitched, 0, 0);
            } else {
                back_to_menu();
                card::post_event_ack(7, arg);
            }
            break;
        case kCmdConfirm:
            if (bad_session()) {
                card::post_event_ack(3, arg);
            } else if (((ev.code - 2u) & 0xFDu) != 0) {  // only code 2 or 6 may be confirmed
                card::post_event_ack(4, arg);
            } else {
                card::post_event(kEvAck3, 0, 0);
                card::post_event(kEvAck4, 0, 0);
            }
            break;
        case kCmdFinish:
            if (bad_session()) {
                card::post_event_ack(3, arg);
            } else if (ev.code != kEvAck4) {
                card::post_event_ack(4, arg);
            } else {
                ev.flags |= 0x10;
                card::post_event(kEvConfirmed, 0, 0);
                ev.flags |= 0x30;
                card::post_event(kEvSessionDone, 0, 0);
                st.switch_count = 0;
            }
            break;
        case kCmdCancel:
            if (bad_session()) {
                card::post_event_ack(3, arg);
            } else {
                card::post_event(kEvClosing, 0, 0);
                back_to_menu();
                ev.flags &= 0xFFCD;
                card::post_event(kEvMenuBack, 0, 0);
            }
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------------------------
bool read_sector_main(uint32_t lba, bool loc1, const uint8_t** out) {
    CardState& st = state();
    if (!loc1 || st.magic != card::kStateMagic || lba < 0x96) return false;
    const uint32_t n = lba - 0x96;
    const uint32_t cmd = st.cmd, arg = st.cmd_arg;

    if (cmd != 0) {
        if (n == 100) {
            if (cmd - 1 < 4) {
                apply_list_request(cmd, arg);
                finish_request();
            }
        } else if (lba - kLbaArt < kArtChunks) {
            if (cmd == kCmdArtSelect) {
                apply_art_request(arg);
                finish_request();
            }
        } else if (n == 400) {
            if (cmd == kCmdInfo) {
                build_disc_info(arg & 0xFFFF);
                st.cmd = 0;
                st.cache_key = 400;
                return finish(n, out);
            }
            if (cmd == kCmdPcr2) finish_request();
        } else if (lba - kLbaCheat < kCheatChunks) {
            if (cmd == kCmdPcr2) finish_request();
        } else if (n == 900) {
            run_session_request();
            finish_request();
        }
    }

    if ((st.phase == card::kPhaseGame && n != 900) || st.phase == card::kPhaseFault ||
        (st.phase == card::kPhaseMenu && n < kMenuSectors))
        return false;
    if (st.cache_key == n) return finish(n, out);

    if (n == 100) {
        card::library_listing();
    } else if (n == 0x96) {
        build_config_page();
    } else if (lba - kLbaArt < kArtChunks) {
        build_art_chunk(lba);
    } else if (lba - kLbaCheat < kCheatChunks) {
        // Note: the info sector (N=400, 0x226) lies inside this window as chunk 100. It is only reachable
        // through the cache or kCmdInfo above; an uncached read of it is answered as cheat chunk 100.
        build_cheat_chunk_main(lba - kLbaCheat);
    } else if (n == 900) {
        build_session_page();
    } else {
        card::zero_fill(resp(), kRespBytes);
    }
    return finish(n, out);
}

bool read_sector(uint32_t lba, bool loc1, const uint8_t** out) {
    if (hook_dead(lba, loc1, out)) return true;
    if (hook_reject_session(lba, loc1)) return false;
    if (hook_cheat_v6(lba, loc1, out)) return true;
    return read_sector_main(lba, loc1, out);
}

}  // namespace mh::handler
