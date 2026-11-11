// Host test of the hook glue (src/mh/hooks_impl.cpp): mounts generated card images through
// mh::hooks::mount_default_disc, reads sectors through card_read_sector and compares the produced bytes with the
// layout documents (docs/re/CARD_LAYOUT_CROSSCHECK.md, handler.md, aux.md). Hardware is faked through mh::hooks::Hw.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "mh/aux_legacy.h"
#include "mh/card.h"
#include "mh/handler.h"
#include "mh/hooks.h"
#include "mh/hooks_hw.h"

using namespace mh;
using Bytes = std::vector<uint8_t>;
using picostation::DiscImage;
using picostation::MechCommand;

namespace {

// ---- fake hardware --------------------------------------------------------------------------------
Bytes g_img;
Bytes g_menu;
int g_sd_init_result = 0;
uint32_t g_sector_max = 0;
uint32_t g_clock_writes = 0;

struct Capture {
    int calls = 0;
    int sector = -1;
    bool has_user = false;
    Bytes user;                 // 2324 bytes handed to buildSector
    const uint16_t* scr = nullptr;
    Bytes scrambled_src;        // 0x930 bytes handed to scramble
    const uint16_t* scramble_table = nullptr;
    int scramble_calls = 0;
} g_cap;

int hw_sd_init() { return g_sd_init_result; }
int hw_sd_read(uint32_t lba, uint32_t blocks, void* dst) {
    if ((static_cast<uint64_t>(lba) + blocks) * 512 > g_img.size()) return 1;
    std::memcpy(dst, g_img.data() + static_cast<size_t>(lba) * 512, static_cast<size_t>(blocks) * 512);
    return 0;
}
void hw_build_sector(DiscImage&, int sector, uint32_t* buffer, const uint16_t* user, const uint16_t* scr) {
    ++g_cap.calls;
    g_cap.sector = sector;
    g_cap.has_user = user != nullptr;
    g_cap.user.assign(2324, 0);
    if (user) std::memcpy(g_cap.user.data(), user, 2324);
    g_cap.scr = scr;
    buffer[0] = 0xB5ECB5EC;  // marker: the output buffer is the caller's
}
void hw_scramble(uint32_t* dst, const uint16_t* src, const uint16_t* scr, uint32_t words) {  // ff.c scramble_data
    ++g_cap.scramble_calls;
    g_cap.scramble_table = scr;
    g_cap.scrambled_src.assign(reinterpret_cast<const uint8_t*>(src), reinterpret_cast<const uint8_t*>(src) + words * 2);
    for (uint32_t i = 0; i < words; ++i) {
        uint32_t t = static_cast<uint32_t>(scr ? (src[i] ^ scr[i]) : src[i]) << 8;
        if (t & 0x100) t |= 0xFF;
        dst[i] = t;
    }
}
void hw_sector_max(uint32_t v) { g_sector_max = v; }
void hw_card_mode(DiscImage&, bool) {}
uint32_t hw_now() { static uint32_t t = 0; return t += 7; }
void hw_i2c_open() {}
bool hw_i2c_write(uint8_t, uint8_t) { ++g_clock_writes; return true; }
bool hw_i2c_read(uint8_t, uint8_t* v) { *v = 0; return true; }  // SYS_INIT and lock bits clear

hooks::Hw g_hw;

Bytes file(const char* path) {
    Bytes v;
    if (FILE* f = std::fopen(path, "rb")) {
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
        std::fclose(f);
    }
    return v;
}

Bytes gen_card(const char* kind) {  // card_image.py <out> <kind>
    const char* out = "integration_test.img";
    char cmd[256];
    std::snprintf(cmd, sizeof cmd, "python3 -I card_image.py %s %s", out, kind);
    if (std::system(cmd) != 0) std::exit(2);
    Bytes v = file(out);
    std::remove(out);
    return v;
}
Bytes gen_legacy_v1() {  // legacy_image.py <kind> <out>
    const char* out = "integration_test.img";
    char cmd[256];
    std::snprintf(cmd, sizeof cmd, "python3 -I legacy_image.py v1 %s", out);
    if (std::system(cmd) != 0) std::exit(2);
    Bytes v = file(out);
    std::remove(out);
    return v;
}

// ---- helpers --------------------------------------------------------------------------------------
card::CardDisc g_cd;
DiscImage& image() { return *reinterpret_cast<DiscImage*>(&g_cd); }
char g_mech_dummy;
MechCommand& mech() { return *reinterpret_cast<MechCommand*>(&g_mech_dummy); }
uint16_t g_table[1176];
uint32_t g_out[1176];

card::CardState& st() { return card::state(); }

Bytes pattern(int tag, int n) {  // tests/card_image.py pattern()
    Bytes b(n);
    for (int i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(tag * 7 + i);
    return b;
}

void bind() {
    g_hw = hooks::Hw{hw_sd_init, hw_sd_read, hw_build_sector, hw_scramble, hw_sector_max, hw_card_mode, hw_now,
                     hw_i2c_open, hw_i2c_write, hw_i2c_read, g_menu.data(), static_cast<uint32_t>(g_menu.size())};
    hooks::set_hw(&g_hw);
    for (int i = 0; i < 1176; ++i) g_table[i] = static_cast<uint16_t>(i * 3 + 1);
}

void mount(const Bytes& img) {
    g_img = img;
    std::memset(&g_cd, 0, sizeof g_cd);
    g_sector_max = 0;
    CHECK(hooks::mount_default_disc(image()));
}

const Capture& read(uint32_t lba) {
    g_cap = Capture{};
    g_out[0] = 0;
    hooks::card_read_sector(image(), g_out, static_cast<int>(lba), g_table);
    return g_cap;
}

void pad(uint32_t sub, uint32_t arg) { hooks::on_mech_command(mech(), 0xF00000 | sub << 16 | (arg & 0xFFFF)); }

std::string tag(const Capture& c) { return std::string(reinterpret_cast<const char*>(c.user.data()), 4); }

// ---- scenarios ------------------------------------------------------------------------------------
void test_v2_menu() {
    Bytes img = gen_card("basic");
    // menu sector 1 (LBA 0x97) gets a data sector sync pattern with mode 2: it must be scrambled
    const size_t menu1 = static_cast<size_t>(card::kMenuLba + 5) * 512;
    img[menu1] = 0;
    for (int i = 1; i <= 10; ++i) img[menu1 + i] = 0xFF;
    img[menu1 + 11] = 0;
    img[menu1 + 15] = 2;
    mount(img);

    // mount results
    CHECK_EQ(st().magic, card::kStateMagic);
    CHECK_EQ(st().phase, 1u);
    CHECK_EQ(st().valid, 1u);
    CHECK_EQ(g_cd.sd_menu, 1);
    CHECK_EQ(g_cd.cue.track_count, 1u);
    CHECK_EQ(g_cd.cue.tracks[1].size, card::kDefaultDiscSectors);
    CHECK_EQ(card::globals().raw_base, card::kMenuLba);
    CHECK_EQ(card::globals().total_sectors, 0x385u + 2 * 0x122A);
    CHECK_EQ(g_sector_max, 0x385u + 2 * 0x122A);

    // menu sector 0 (LBA 0x96): raw read of the card, not scrambled (no sync pattern)
    const Capture& m0 = read(0x96);
    CHECK_EQ(m0.calls, 0);
    CHECK_EQ(m0.scramble_calls, 1);
    CHECK(m0.scramble_table == nullptr);
    CHECK(m0.scrambled_src == pattern(0x80, 0x930));
    {
        const Bytes p = pattern(0x80, 2);
        uint32_t w = static_cast<uint32_t>(p[0] | p[1] << 8) << 8;  // unscrambled: scramble_data without table
        if (w & 0x100) w |= 0xFF;
        CHECK_EQ(g_out[0], w);
    }
    // menu sector 1: sync pattern with mode 2 gets the scrambling table
    const Capture& m1 = read(0x97);
    CHECK(m1.scramble_table == g_table);
    CHECK_EQ(m1.scrambled_src[15], 2);

    // library listing (LBA 0xFA) after a "list all" request: [len][1][name] entries, trailer [0][err][count]
    pad(1, 0);
    const Capture& l = read(0xFA);
    CHECK_EQ(l.calls, 1);
    CHECK_EQ(l.sector, 0xFA);
    CHECK_EQ(g_out[0], 0xB5ECB5ECu);
    const std::string names[] = {"Alundra 2", "Chrono Cross", "CASE PS1", std::string(31, 'A') + "B",
                                 "Caf     ! ~"};
    size_t pos = 0;
    for (const std::string& n : names) {
        CHECK_EQ(l.user[pos], n.size());
        CHECK_EQ(l.user[pos + 1], 1);
        CHECK(std::string(reinterpret_cast<const char*>(&l.user[pos + 2]), n.size()) == n);
        pos += 2 + n.size();
    }
    CHECK_EQ(l.user[pos], 0);
    CHECK_EQ(l.user[pos + 1], 0);
    CHECK_EQ(l.user[pos + 2], 0);
    CHECK_EQ(l.user[pos + 3], 5);

    // config page (LBA 0x12C): "CFG1", length, text at +0x114
    const Capture& c = read(0x12C);
    CHECK(tag(c) == "CFG1");
    const std::string theme = "theme=umbrella\n";
    CHECK_EQ(c.user[4] | c.user[5] << 8, static_cast<int>(theme.size()));
    CHECK(std::string(reinterpret_cast<const char*>(&c.user[0x114]), theme.size()) == theme);

    // music window enabled by the command word 0xF80008: LBA 300 is served by the music server (no music on this card)
    pad(8, 8);  // 0xF80008
    const Capture& mu = read(0x12C);
    CHECK_EQ(mu.calls, 1);
    CHECK(tag(mu) != "CFG1");
    pad(0, 0);  // any other F-class word clears the flag
    CHECK(tag(read(0x12C)) == "CFG1");

    // switch to disc 0: disc info (PGM1), then the session page; afterwards game sectors come from the disc package
    pad(3, 0);  // select Case 0 ("Alundra 2")
    CHECK(tag(read(0xFA)) != "");
    pad(0xC, 0);
    const Capture& info = read(0x226);
    CHECK(tag(info) == "PGM1");
    CHECK_EQ(info.user[4], 1);
    CHECK_EQ(info.user[5], 1);  // valid serial
    const uint32_t raw_lba = st().disc.raw_lba;
    CHECK_EQ(raw_lba, static_cast<uint32_t>(card::kArtSize + 0x100));
    pad(5, 0);
    const Capture& ses = read(0x41A);
    CHECK(tag(ses) == "PLS1");
    CHECK_EQ(ses.user[0x10], 2);  // event 2: disc started
    CHECK_EQ(st().phase, 2u);
    CHECK_EQ(card::globals().raw_base, raw_lba);
    hooks::poll();
    CHECK_EQ(g_sector_max, 3u + 2 * 0x122A);
    CHECK_EQ(g_cd.cue.track_count, 1u);

    // the generator packs packages 0x100 blocks apart, so disc 1's track map overwrites sector 0 of disc 0
    // (LBA 0x96); sectors 1 and 2 are intact
    for (uint32_t s = 1; s < 3; ++s) {
        const Capture& g = read(0x96 + s);
        CHECK_EQ(g.calls, 0);
        CHECK_EQ(g.scramble_calls, 1);
        CHECK(g.scrambled_src == pattern(static_cast<int>(s), 0x930));
    }
    CHECK(std::memcmp(read(0x96).scrambled_src.data(), "PSTTRK1", 7) == 0);
    // outside the menu session nothing but 0x41A is answered by the handler
    CHECK_EQ(read(0xFA).calls, 0);
    CHECK(tag(read(0x41A)) == "PLS1");

    // a PS1 reset puts the raw base back to 2 and the sector cursor to 0
    card::globals().sector_cursor = 9;
    hooks::reset_card_state();
    CHECK_EQ(card::globals().raw_base, 2u);
    CHECK_EQ(card::globals().sector_cursor, 0u);
}

void test_unreadable_sector() {
    mount(gen_card("basic"));
    // game phase, block beyond the image: header only, no data
    card::globals().raw_base = 0x7FFFFF00;
    st().phase = card::kPhaseGame;
    const Capture& c = read(0x96);
    CHECK_EQ(c.calls, 1);
    CHECK(!c.has_user);
    CHECK_EQ(c.scramble_calls, 0);
}

void test_fallback() {
    g_img.assign(0x4000 * 512, 0);
    Bytes zero = g_img;
    mount(zero);
    CHECK_EQ(st().phase, 1u);
    CHECK_EQ(st().menu_sectors, 0u);
    CHECK_EQ(st().valid, 0u);
    CHECK_EQ(g_cd.cue.track_count, 1u);
    CHECK_EQ(g_cd.cue.tracks[1].size, 0x5Du);
    CHECK_EQ(card::globals().raw_base, 0u);
    CHECK_EQ(g_sector_max, 0x24B1u);
    CHECK_EQ(g_cd.sd_menu, 1);

    // sector 0 of the built-in menu: image bytes ^ scrambler, shifted, bit 8 extends to 0xFF
    const Capture& c = read(0x96);
    CHECK_EQ(c.calls, 0);
    CHECK_EQ(c.scramble_calls, 0);
    for (int i = 0; i < 1176; ++i) {
        const uint32_t w = g_menu[2 * i] | g_menu[2 * i + 1] << 8;
        uint32_t e = (w ^ g_table[i]) << 8;
        if (e & 0x100) e |= 0xFF;
        if (g_out[i] != e) {
            CHECK_EQ(g_out[i], e);
            break;
        }
    }
    // a sector past the 29 embedded ones (but inside the 0x5D) is the scrambled zero page
    read(0x96 + 40);
    for (int i = 0; i < 1176; ++i) {
        uint32_t e = static_cast<uint32_t>(g_table[i]) << 8;
        if (e & 0x100) e |= 0xFF;
        if (g_out[i] != e) {
            CHECK_EQ(g_out[i], e);
            break;
        }
    }
    // the listing sector reports the invalid card
    pad(1, 0);
    const Capture& l = read(0xFA);
    CHECK_EQ(l.calls, 1);
    CHECK(std::string(l.user.begin(), l.user.end()).find("ERRO RAW INVALIDO") != std::string::npos);

    // SD init failure: same result
    g_sd_init_result = 1;
    mount(Bytes(0x100 * 512, 0));
    CHECK_EQ(st().phase, 1u);
    CHECK_EQ(g_sector_max, 0x24B1u);
    g_sd_init_result = 0;
}

void test_legacy_single() {
    mount(gen_card("legacy"));
    CHECK_EQ(st().phase, 0u);
    CHECK_EQ(g_sector_max, 4u + 2 * 0x122A);
    CHECK_EQ(card::globals().raw_base, 0x45u);
    const Capture& c = read(0x96 + 2);
    CHECK_EQ(c.calls, 0);
    CHECK(c.scrambled_src == pattern(3, 0x930));
    hooks::on_door();  // single disc: nothing to rotate
    CHECK_EQ(card::globals().raw_base, 0x45u);
}

void test_legacy_multi_rotation() {
    const Bytes img = gen_legacy_v1();
    aux::LegacyV1 v;
    CHECK(aux::legacy_v1_parse(img.data(), v));
    mount(img);
    CHECK_EQ(st().phase, 0u);
    CHECK_EQ(card::globals().raw_base, v.discs[1].raw_lba());  // default disc 1
    CHECK_EQ(g_sector_max, v.discs[1].sectors() + 2 * 0x122A);
    hooks::on_door();
    CHECK_EQ(card::globals().raw_base, v.discs[2].raw_lba());
    hooks::on_door();  // wraps to disc 0
    CHECK_EQ(card::globals().raw_base, v.discs[0].raw_lba());
    hooks::poll();
    CHECK_EQ(g_sector_max, v.discs[0].sectors() + 2 * 0x122A);
}

void test_boot() {
    hooks::boot_trace();
    g_clock_writes = 0;
    hooks::boot_clock();
    CHECK_EQ(g_clock_writes, 49u);  // the fixed Si5351 table
}

}  // namespace

int main() {
    g_menu = file("../binary/picostation-menu.bin");
    CHECK_EQ(g_menu.size(), 68208u);
    bind();
    test_boot();
    test_v2_menu();
    test_unreadable_sector();
    test_fallback();
    test_legacy_single();
    test_legacy_multi_rotation();
    TEST_MAIN_END();
}
