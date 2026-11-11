// PSTCFG2 loader and music server (V3 FUN_10024450 / FUN_100245e0 / FUN_10024588).
#include <cstring>
#include <vector>

#include "check.h"
#include "mh/aux_cfg.h"

using namespace mh::aux;

namespace {

constexpr uint32_t kCfgLba = 0x1685, kLimit = 0x2800, kMusicLba = 0x168A;

struct Card {
    std::vector<uint8_t> blocks = std::vector<uint8_t>(kLimit * 512u, 0);
    std::vector<std::pair<uint32_t, uint32_t>> reads;
    bool fail = false;
    uint8_t* at(uint32_t lba) { return blocks.data() + lba * 512u; }
};

bool rd(void* ctx, uint32_t lba, uint32_t n, uint8_t* out) {
    auto* c = static_cast<Card*>(ctx);
    c->reads.push_back({lba, n});
    if (c->fail || (lba + n) * 512ull > c->blocks.size()) return false;
    std::memcpy(out, c->at(lba), n * 512u);
    return true;
}

void put_cfg(Card& c, const char* text, uint32_t music_lba, uint32_t size, uint32_t mcrc) {
    uint8_t* b = c.at(kCfgLba);
    std::memset(b, 0, kBlockBytes);
    std::memcpy(b, "PSTCFG2\0", 8);
    b[8] = 1;
    put_le32(b + 0x1C, music_lba);
    put_le32(b + 0x24, size);
    put_le32(b + 0x28, mcrc);
    std::strcpy(reinterpret_cast<char*>(b + 0x2C), text);
    put_le32(b + 0x92C, crc32(b, 0x92C));
}

CardView view() { return CardView{true, kCfgLba, kLimit, 7}; }

void fix_crc(Card& c) { put_le32(c.at(kCfgLba) + 0x92C, crc32(c.at(kCfgLba), 0x92C)); }

void test_crc() {
    CHECK_EQ(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9), 0xCBF43926u);
    const uint8_t z[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_EQ(crc32_hole(z, 8, 2, 3), crc32(reinterpret_cast<const uint8_t*>("\1\2\0\0\0\6\7\10"), 8));
}

void test_loader() {
    Card c;
    BlockReader r{rd, &c};
    uint8_t resp[kResponseBytes];
    CfgState st;
    put_cfg(c, "theme=umbrella\nmusic=on\n", kMusicLba, 1000, 1);
    std::memset(resp, 0, sizeof resp);
    CHECK(cfg_load(st, view(), r, resp));
    CHECK_EQ(st.loaded, 1u);
    CHECK_EQ(st.music_lba, kMusicLba);
    CHECK_EQ(st.music_size, 1000u);
    CHECK_EQ(st.generation, 7u);
    CHECK(std::memcmp(resp, "CFG1", 4) == 0);
    CHECK_EQ(le16(resp + 4), 24u);
    CHECK(std::memcmp(resp + 0x114, "theme=umbrella\nmusic=on\n", 24) == 0);
    CHECK_EQ(c.reads.back().second, 5u);

    // window checks on cfg_lba
    CHECK(!cfg_load(st, CardView{true, 0, kLimit, 7}, r, resp));
    CHECK(!cfg_load(st, CardView{true, kLimit - 4, kLimit, 7}, r, resp));
    CHECK(!cfg_load(st, CardView{true, 0xFFFFFFFCu, kLimit, 7}, r, resp));
    CHECK_EQ(st.loaded, 0u);
    c.fail = true;
    CHECK(!cfg_load(st, view(), r, resp));
    c.fail = false;

    struct Mut {
        const char* name;
        void (*f)(uint8_t*);
    };
    const Mut muts[] = {
        {"magic", [](uint8_t* b) { b[3] = 'X'; }},
        {"nul of magic", [](uint8_t* b) { b[7] = 1; }},
        {"version", [](uint8_t* b) { b[8] = 2; }},
        {"version hi", [](uint8_t* b) { b[9] = 1; }},
        {"music high dword", [](uint8_t* b) { b[0x20] = 1; }},
        {"empty text", [](uint8_t* b) { b[0x2C] = 0; }},
        {"unterminated text", [](uint8_t* b) { std::memset(b + 0x2C, 'a', 0x800); }},
    };
    for (const Mut& m : muts) {
        put_cfg(c, "a=b", kMusicLba, 1000, 1);
        m.f(c.at(kCfgLba));
        fix_crc(c);  // only the checked field is wrong
        CHECK(!cfg_load(st, view(), r, resp));
    }
    put_cfg(c, "a=b", kMusicLba, 1000, 1);
    c.at(kCfgLba)[0x300] ^= 1;  // CRC covers the whole block up to 0x92C
    CHECK(!cfg_load(st, view(), r, resp));
    put_cfg(c, "a=b", kMusicLba, 1000, 1);
    c.at(kCfgLba)[0xA] = 9;  // +0xA is not read; the CRC covers it, so refix it and the block must load
    fix_crc(c);
    CHECK(cfg_load(st, view(), r, resp));
    // longest text: 0x7FF characters
    put_cfg(c, "", kMusicLba, 1000, 1);
    std::memset(c.at(kCfgLba) + 0x2C, 'z', 0x7FF);
    fix_crc(c);
    CHECK(cfg_load(st, view(), r, resp));
    CHECK_EQ(le16(resp + 4), 0x7FFu);
}

void test_music_plan() {
    MusicChunk m;
    CHECK(music_chunk_plan(0x830, 0, m));
    CHECK_EQ(m.count, 1u);
    CHECK_EQ(m.length, 0x830u);
    CHECK(!music_chunk_plan(0x830, 1, m));
    CHECK(music_chunk_plan(100, 0, m));
    CHECK_EQ(m.length, 100u);
    CHECK(music_chunk_plan(0x831, 0, m));
    CHECK_EQ(m.count, 2u);
    CHECK_EQ(m.length, 0x830u);
    CHECK(music_chunk_plan(0x831, 1, m));
    CHECK_EQ(m.offset, 0x830u);
    CHECK_EQ(m.length, 1u);
    CHECK(!music_chunk_plan(0x831, 2, m));
    // chunk limit: 0x100 chunks <=> size <= 0x80030
    CHECK(music_chunk_plan(0x80030, 0xFF, m));
    CHECK_EQ(m.count, 0x100u);
    CHECK_EQ(m.offset, 0xFFu * 0x800 + 0x30);
    CHECK_EQ(m.length, 0x800u);
    CHECK(!music_chunk_plan(0x80030, 0x100, m));
    CHECK(!music_chunk_plan(0x80031, 0, m));
    // the embedded Umbrella music: 478880 bytes, 234 chunks
    CHECK(music_chunk_plan(478880, 233, m));
    CHECK_EQ(m.count, 234u);
    CHECK_EQ(m.offset, 233u * 0x800 + 0x30);
    CHECK_EQ(m.length, 478880u - m.offset);
    // every chunk tiles the file exactly once
    uint32_t total = 0;
    for (uint32_t n = 0; n < 234; ++n) {
        CHECK(music_chunk_plan(478880, n, m));
        CHECK_EQ(m.offset, n ? total : 0u);
        total = m.offset + m.length;
    }
    CHECK_EQ(total, 478880u);
}

void test_serve() {
    Card c;
    BlockReader r{rd, &c};
    CfgState st;
    uint8_t resp[kResponseBytes];
    const uint32_t size = 5000;
    for (uint32_t i = 0; i < size + 16; ++i) c.at(kMusicLba)[i] = static_cast<uint8_t>(i * 7 + 3);
    put_cfg(c, "k=v", kMusicLba, size, 0xDEADBEEF);

    // pass through cases
    CardView off = view();
    off.mounted = false;
    CHECK(cfg_music_serve(st, off, r, 300, 1, resp) == ServeResult::PassThrough);
    CHECK(cfg_music_serve(st, view(), r, 300, 2, resp) == ServeResult::PassThrough);
    CHECK(cfg_music_serve(st, view(), r, 0x95, 1, resp) == ServeResult::PassThrough);
    CHECK(cfg_music_serve(st, view(), r, 301, 1, resp) == ServeResult::PassThrough);  // music off: only 300
    CHECK(cfg_music_serve(st, view(), r, 299, 1, resp) == ServeResult::PassThrough);

    // config window
    std::memset(resp, 0x55, sizeof resp);
    CHECK(cfg_music_serve(st, view(), r, 300, 1, resp) == ServeResult::Served);
    CHECK(std::memcmp(resp, "CFG1", 4) == 0);
    CHECK_EQ(le16(resp + 4), 3u);
    CHECK_EQ(resp[kResponseBytes - 1], 0u);
    CHECK_EQ(resp[0x113], 0u);
    c.at(kCfgLba)[0x2C] = 0;  // broken config
    fix_crc(c);
    CHECK(cfg_music_serve(st, view(), r, 300, 1, resp) == ServeResult::Served);
    CHECK(std::memcmp(resp, "DEAD", 4) == 0);
    CHECK_EQ(resp[4], 0u);
    put_cfg(c, "k=v", kMusicLba, size, 0xDEADBEEF);

    // enabling command word
    cfg_command_word(st, true, 0x00F80007);
    CHECK_EQ(st.music_enabled, 0u);
    cfg_command_word(st, false, kCommandMusicOn);
    CHECK_EQ(st.music_enabled, 0u);
    cfg_command_word(st, true, 0x00080008);  // not an F-class command: no change
    CHECK_EQ(st.music_enabled, 0u);
    cfg_command_word(st, true, kCommandMusicOn);
    CHECK_EQ(st.music_enabled, 1u);
    cfg_command_word(st, true, 0x00080008);
    CHECK_EQ(st.music_enabled, 1u);
    CHECK(cfg_music_serve(st, view(), r, 300 + 256, 1, resp) == ServeResult::PassThrough);
    CHECK(cfg_music_serve(st, view(), r, 299, 1, resp) == ServeResult::PassThrough);

    // chunk 0
    c.reads.clear();
    CHECK(cfg_music_serve(st, view(), r, 300, 1, resp) == ServeResult::Served);
    CHECK(std::memcmp(resp, "MUS1", 4) == 0);
    CHECK_EQ(le16(resp + 4), 0x18u);
    CHECK_EQ(resp[6], 0u);
    CHECK_EQ(resp[7], 0u);
    CHECK_EQ(le16(resp + 8), 3u);  // (5000 - 0x31) >> 11 + 1
    CHECK_EQ(le16(resp + 10), 0x830u);
    CHECK_EQ(le32(resp + 12), size);
    CHECK(std::memcmp(resp + 0x18, c.at(kMusicLba), 0x830) == 0);
    CHECK_EQ(le32(resp + 16), crc32(resp + 0x18, 0x830));
    CHECK_EQ(le32(resp + 20), crc32(resp, 0x14));
    CHECK_EQ(resp[0x18 + 0x830], 0u);
    CHECK_EQ(c.reads.back().first, kMusicLba);
    CHECK_EQ(c.reads.back().second, 5u);
    // chunk 1: offset 0x830, one block skew 0x30
    c.reads.clear();
    CHECK(cfg_music_serve(st, view(), r, 301, 1, resp) == ServeResult::Served);
    CHECK_EQ(resp[6], 1u);
    CHECK_EQ(le16(resp + 10), 0x800u);
    CHECK(std::memcmp(resp + 0x18, c.at(kMusicLba) + 0x830, 0x800) == 0);
    CHECK_EQ(c.reads.back().first, kMusicLba + 4);  // 0x830 >> 9
    CHECK_EQ(c.reads.back().second, 5u);
    // last chunk is short
    CHECK(cfg_music_serve(st, view(), r, 302, 1, resp) == ServeResult::Served);
    CHECK_EQ(le16(resp + 10), size - (2 * 0x800 + 0x30));
    CHECK(std::memcmp(resp + 0x18, c.at(kMusicLba) + 2 * 0x800 + 0x30, le16(resp + 10)) == 0);
    // beyond the chunk count: all zero response
    CHECK(cfg_music_serve(st, view(), r, 303, 1, resp) == ServeResult::Served);
    CHECK(all_zero(resp, kResponseBytes));
    // read error: all zero
    c.fail = true;
    CHECK(cfg_music_serve(st, view(), r, 300, 1, resp) == ServeResult::Served);
    CHECK(all_zero(resp, kResponseBytes));
    c.fail = false;

    // cached config is reused while the generation matches, reloaded (and refused) otherwise
    c.reads.clear();
    CHECK(cfg_music_serve(st, view(), r, 300, 1, resp) == ServeResult::Served);
    CHECK_EQ(c.reads.size(), 1u);  // only the music read
    CardView newer = view();
    newer.generation = 8;
    c.reads.clear();
    CHECK(cfg_music_serve(st, newer, r, 300, 1, resp) == ServeResult::Served);
    CHECK_EQ(c.reads.size(), 2u);  // cfg reload + music read
    CHECK_EQ(st.generation, 8u);

    // music window validation: lba/size/crc zero, lba >= limit, size beyond the window
    struct Case { uint32_t lba, size, crc; };
    const Case bad[] = {{0, size, 1}, {kMusicLba, 0, 1}, {kMusicLba, size, 0}, {kLimit, size, 1},
                        {kLimit - 1, 513, 1}, {kMusicLba, 0x80031, 1}};
    for (const Case& k : bad) {
        put_cfg(c, "k=v", k.lba, k.size, k.crc);
        st = CfgState{};
        st.music_enabled = 1;
        CHECK(cfg_music_serve(st, view(), r, 300, 1, resp) == ServeResult::Served);
        CHECK(all_zero(resp, kResponseBytes));
    }
    put_cfg(c, "k=v", kLimit - 1, 512, 1);  // exactly the window
    st = CfgState{};
    st.music_enabled = 1;
    CHECK(cfg_music_serve(st, view(), r, 300, 1, resp) == ServeResult::Served);
    CHECK(std::memcmp(resp, "MUS1", 4) == 0);
    CHECK_EQ(le16(resp + 10), 512u);

    // reset
    cfg_reset(st);
    CHECK_EQ(st.music_enabled, 0u);
    CHECK_EQ(st.loaded, 0u);
}

}  // namespace

int main() {
    test_crc();
    test_loader();
    test_music_plan();
    test_serve();
    TEST_MAIN_END();
}
