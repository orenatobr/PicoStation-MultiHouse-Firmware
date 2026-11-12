// Strong implementations of the integration hooks (include/mh/hooks.h): they connect the upstream derived
// firmware to the Multi House layers (card, handler, aux). Host safe: all hardware goes through mh::hooks::Hw
// (include/mh/hooks_hw.h), bound by src/mh/hooks_pico.cpp on the device. Evidence and the V3 addresses of every
// site: docs/re/deltas.md section 4 and docs/re/integration.md.
#include <string.h>

#include "mh/aux_cfg.h"
#include "mh/aux_clock.h"
#include "mh/aux_fallback.h"
#include "mh/aux_legacy.h"
#include "mh/aux_trace.h"
#include "mh/card.h"
#include "mh/handler.h"
#include "mh/hooks.h"
#include "mh/hooks_hw.h"
#include "mh/menu_overlay.h"

namespace mh::hooks {
namespace {

const Hw* g_hw = nullptr;

// V3 legacy state block (RAM 0x20033900, magic "SW62"): which rotation the door handler runs.
struct Legacy {
    uint32_t mode = 0;     // 0 none, 1 PSXRAW v1 (single or multi disc), 2 V2 card
    uint32_t current = 0;  // v1 position
    aux::LegacyV1 v1;
};

Legacy g_legacy;
aux::DiscTable g_table;  // scratch table, copied into the DiscImage once complete
aux::CfgState g_cfg;     // RAM 0x20033800
aux::TraceBlock g_trace;
card::CardDisc* g_disc = nullptr;
bool g_mounted = false;

static_assert(sizeof(card::DiscInfo) == aux::kDiscEntryBytes, "Disc record copies are interchangeable");

// ---------------------------------------------------------------------------------------------
// SD access
// ---------------------------------------------------------------------------------------------
int sd_init_adapter() { return g_hw->sd_init(); }
int __not_in_flash_func(sd_read_adapter)(uint32_t lba, uint32_t blocks, void* dst) {
    return g_hw->sd_read(lba, blocks, dst);
}

bool __not_in_flash_func(block_read)(void*, uint32_t lba, uint32_t blocks, uint8_t* out) {
    return card::sd().read != nullptr && card::sd().read(lba, blocks, out) == 0;
}
const aux::BlockReader kReader = {block_read, nullptr};

// ---------------------------------------------------------------------------------------------
// Mount helpers
// ---------------------------------------------------------------------------------------------

// Installs a finished scratch table into the DiscImage and publishes the sector globals (V3 tail of
// FUN_100270a8 / FUN_10030000: raw base, cursor 0, total sectors, response cache dropped).
void install_table(card::CardDisc* disc) {
    memcpy(disc, g_table.b, sizeof(card::CardDisc));
    card::CardGlobals& g = card::globals();
    g.raw_base = g_table.raw_base;
    g.sector_cursor = 0;
    g.total_sectors = g_table.end_sector;
    card::reset_response_cache();
}

// V3 FUN_10030000 fallback branch: built-in menu, state 1, no card menu.
void mount_fallback(card::CardDisc* disc) {
    aux::fallback_table(g_table);
    install_table(disc);
    card::CardState& st = card::state();
    const aux::FallbackState f;
    st.phase = f.phase;
    st.magic = card::kStateMagic;
    st.disc_image_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(disc));
    st.menu_lba = f.raw_base;
    st.menu_sectors = f.menu_sectors;
    st.cache_key = 0xFFFFFFFFu;
    st.sel_case = 0xFFFFFFFFu;
}

// V3 FUN_100273c4 failure branch: legacy PSXRAW v1 multi disc header in slot 0.
bool mount_legacy_multi(card::CardDisc* disc) {
    if (!card::read_slot(0, 0x10)) return false;
    aux::LegacyV1 v1;
    if (!aux::legacy_v1_mount(card::block_buffer(), kReader, v1, g_table)) return false;
    g_legacy.v1 = v1;
    g_legacy.mode = 1;
    g_legacy.current = v1.default_disc;
    install_table(disc);
    return true;
}

// Sector data of a PS1 sector request, wrapped by the upstream sector builder (the original sends the 0x914 byte
// response buffer through DiscImage::buildSector at RAM 0x20000588).
inline void send_response(picostation::DiscImage& disc, uint32_t* out, int sector, const uint8_t* resp,
                          const uint16_t* scr) {
    g_hw->build_sector(disc, sector, out, reinterpret_cast<const uint16_t*>(resp), scr);
}

// Hooks in front of the card read, in the order of the V3 chain: built-in menu (FUN_100300b8), config/music
// server (FUN_100245e0), handler (DEAD, N=900 refusal, v6 cheats, main). True when the sector was produced.
bool __time_critical_func(serve_chain)(picostation::DiscImage& disc, uint32_t* out, int sector, const uint16_t* scr) {
    const uint32_t lba = static_cast<uint32_t>(sector);
    const card::CardState& st = card::state();
    const bool state_ok = st.magic == card::kStateMagic;
    if (aux::fallback_serves(state_ok, st.phase, st.menu_sectors, 1, lba)) {
        const uint32_t k = lba - aux::kFirstSectorLba;
        const uint8_t* img = nullptr;
        if ((k + 1) * aux::kSectorBytes <= g_hw->menu_image_size) img = aux::fallback_sector(g_hw->menu_image, k);
        aux::fallback_convert(img, reinterpret_cast<const uint8_t*>(scr), out);
        return true;
    }
    uint8_t* resp = card::response();
    aux::CardView view;
    view.mounted = state_ok && st.phase == card::kPhaseMenu;
    view.cfg_lba = st.cfg_lba;
    view.limit_blocks = st.art_size;
    view.generation = st.generation;
    if (aux::cfg_music_serve(g_cfg, view, kReader, lba, 1, resp) == aux::ServeResult::Served) {
        send_response(disc, out, sector, resp, scr);
        return true;
    }
    handler::env().hold = g_cfg.music_enabled != 0;
    const uint8_t* r = nullptr;
    if (handler::read_sector(lba, true, &r)) {
        send_response(disc, out, sector, r, scr);
        return true;
    }
    return false;
}

// I2C callbacks of the Si5351 sequence.
bool clock_write(void*, uint8_t reg, uint8_t v) { return g_hw->i2c_write(reg, v); }
bool clock_read(void*, uint8_t reg, uint8_t* v) { return g_hw->i2c_read(reg, v); }
uint32_t clock_now(void*) { return g_hw->now_us(); }

}  // namespace

void set_hw(const Hw* hw) { g_hw = hw; }

// V3 FUN_10030000 -> FUN_100273c4 -> FUN_10021280.
bool mount_default_disc(picostation::DiscImage& image) {
    card::CardDisc* disc = reinterpret_cast<card::CardDisc*>(&image);
    card::SdIo io;
    io.init = sd_init_adapter;
    io.read = sd_read_adapter;
    card::set_sd(io);
    g_legacy = Legacy{};
    g_disc = disc;

    if (card::mount(disc)) {
        // V2 card (phase 1) or PSXRAW v1 single disc (phase 0): the door rotation mode of the V3 legacy block.
        g_legacy.mode = aux::legacy_mode_after_v2(card::state().phase == card::kPhaseNone ? 0 : 1);
    } else if (!mount_legacy_multi(disc)) {
        mount_fallback(disc);
        g_legacy.mode = 0;
    }
    g_hw->set_card_mode(image, true);
    g_hw->set_sector_max(card::globals().total_sectors);
    handler::init(disc);
    g_mounted = true;
    return true;
}

// V3 stub 0x20033000 (flash 0x10021640..0x100216d8) and the card branch of DiscImage::readSector.
void __time_critical_func(card_read_sector)(picostation::DiscImage& disc, void* buffer, int sector,
                                            const uint16_t* scrambling) {
    uint32_t* out = static_cast<uint32_t*>(buffer);
    const uint32_t lba = static_cast<uint32_t>(sector);
    const card::SectorRoute route = card::sector_route(lba);
    if (route != card::SectorRoute::kCard) {
        if (serve_chain(disc, out, sector, scrambling)) return;
        if (route == card::SectorRoute::kBuiltin) {  // built-in menu only: anything it does not serve is zero
            aux::fallback_convert(nullptr, reinterpret_cast<const uint8_t*>(scrambling), out);
            return;
        }
    }

    // Raw sector: 5 blocks (one 0x930 byte sector plus padding) at raw_base + (lba - 0x96) * 5.
    uint8_t* blk = card::block_buffer();
    const uint32_t block = card::sector_block(card::globals().raw_base, lba);
    if (card::sd().read == nullptr || card::sd().read(block, 5, blk) != 0) {
        g_hw->build_sector(disc, sector, out, nullptr, scrambling);  // unreadable: header only, no data
        return;
    }
    // Menu program sectors: swap the ones the Manager 0.5.5 patch changes when the card still has the originals.
    const card::CardState& cs = card::state();
    if (cs.magic == card::kStateMagic && cs.phase == card::kPhaseMenu && card::globals().raw_base == cs.menu_lba &&
        lba >= card::kFirstDataLba && lba - card::kFirstDataLba < cs.menu_sectors) {
        menu_overlay::apply(lba - card::kFirstDataLba, blk);
    }
    // Scramble only data sectors: sync 00 FF*10 00 and a mode byte of 1 or 2.
    bool data = blk[0] == 0 && blk[11] == 0 && (blk[15] == 1 || blk[15] == 2);
    for (uint32_t i = 1; data && i <= 10; ++i) data = blk[i] == 0xFF;
    g_hw->scramble(out, reinterpret_cast<const uint16_t*>(blk), data ? scrambling : nullptr, 1176);
}

// V3 FUN_1002f050 (after the upstream body) chained with FUN_10024090 / FUN_10024588 / FUN_100227cc: trace the
// call, run the music flag hook and the PS1 command decoder, then arm the 0xF9 confirmation.
void __time_critical_func(on_mech_command)(picostation::MechCommand&, uint32_t raw) {
    const uint32_t word = raw & 0xFFFFFF;
    aux::TraceEvent& ev = aux::trace_begin(g_trace, g_hw->now_us(), 0);
    const card::CardState& st = card::state();
    aux::cfg_command_word(g_cfg, st.magic == card::kStateMagic && st.phase == card::kPhaseMenu, word);
    handler::on_pad_command(word);
    handler::on_confirm_command(word);
    aux::trace_end(g_trace, ev, 0, 0);
}

// V3 FUN_1002773c: legacy disc rotation when the door was opened.
void on_door() {
    if (!g_mounted || g_disc == nullptr) return;
    card::CardState& st = card::state();
    if (g_legacy.mode == 1) {
        const aux::SwapResult r = aux::legacy_swap_v1(g_legacy.v1, g_legacy.current, kReader, g_table);
        if (r.status != aux::SwapStatus::Ok) return;
        g_legacy.current = r.next;
        install_table(g_disc);
    } else if (g_legacy.mode == 2 && st.phase == card::kPhaseFault) {
        aux::DiscEntry cur;
        memcpy(cur.b, &st.disc, sizeof cur.b);
        aux::SwapEnv env;
        env.ctx = nullptr;
        env.case_by_index = [](void*, uint32_t idx, aux::CaseView& out) {
            card::CaseInfo c;
            if (!card::case_by_index(idx, &c)) return false;
            out.index = c.index;
            out.disc_count = c.disc_count;
            memcpy(out.disc_ids, c.ids, sizeof out.disc_ids);
            return true;
        };
        env.disc_by_id = [](void*, uint32_t id, aux::DiscEntry& out) {
            card::DiscInfo d;
            if (!card::disc_by_id(id, &d)) return false;
            memcpy(out.b, &d, sizeof out.b);
            return true;
        };
        const aux::SwapResult r = aux::legacy_swap_v2(cur, env, kReader, g_table);
        if (r.status != aux::SwapStatus::Ok) return;
        memcpy(&st.disc, r.entry.b, sizeof r.entry.b);
        st.sel_slot = r.next;
        st.sel_disc_id = r.entry.index();
        st.phase = card::kPhaseFault;
        install_table(g_disc);
    }
}

// V3 FUN_1002f190 -> FUN_1002f0b0, once per I2S loop iteration.
void __time_critical_func(poll)() {
    handler::finish_confirm();
    // The original shares one word between the card layer and the sector limit: a disc switch moves it.
    if (g_mounted) g_hw->set_sector_max(card::globals().total_sectors);
}

// V3 FUN_20002f9a, card part.
void reset_card_state() {
    card::CardGlobals& g = card::globals();
    g.raw_base = 2;
    g.sector_cursor = 0;
}

// V3 FUN_10024564.
void boot_trace() {
    aux::trace_init(g_trace, g_hw->now_us());
    aux::cfg_reset(g_cfg);
}

// V3 FUN_100252dc.
void boot_clock() {
    g_hw->i2c_open();
    aux::ClockBus bus;
    bus.write_reg = clock_write;
    bus.read_reg = clock_read;
    bus.now_us = clock_now;
    bus.ctx = nullptr;
    aux::clock_program(bus);
}

}  // namespace mh::hooks
