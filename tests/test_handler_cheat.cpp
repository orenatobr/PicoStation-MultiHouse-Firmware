// Host tests of the cheat blob validators and renderers (src/mh/handler_cheat.cpp).
// Blobs are built here from the field list in docs/re/handler.md section 4, not by the code under test.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "mh/handler_cheat.h"

using namespace mh::handler;
using Bytes = std::vector<uint8_t>;

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;

void w16(Bytes& b, size_t o, uint32_t v) { b[o] = v; b[o + 1] = v >> 8; }
void w32(Bytes& b, size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) b[o + i] = v >> (8 * i); }
uint32_t r32(const Bytes& b, size_t o) { return b[o] | b[o + 1] << 8 | b[o + 2] << 16 | static_cast<uint32_t>(b[o + 3]) << 24; }

uint32_t crc(const uint8_t* p, size_t n) { return mh::card::crc32(p, static_cast<uint32_t>(n)); }

// Recompute the blob CRC (+0x3c) over `total` bytes with the field read as zero.
void seal(Bytes& b, size_t total) {
    w32(b, 0x3C, 0);
    w32(b, 0x3C, crc(b.data(), total));
}

struct Line { uint32_t addr; uint16_t val; };

// ---- version 4 -------------------------------------------------------------------------------
struct E4 {
    std::string name;
    uint8_t flags;
    uint8_t game;
    uint32_t a, b;  // +0x40 variant, +0x44 id
    std::vector<Line> lines;
};

Bytes build_v4(uint8_t game, uint32_t title, uint32_t variant, const std::vector<E4>& es, size_t pad = 0) {
    const uint32_t n = static_cast<uint32_t>(es.size());
    const uint32_t table_end = 0x40 + n * 0x60;
    uint32_t lines = 0, flag0 = 0;
    for (auto& e : es) { lines += e.lines.size(); flag0 += e.flags & 1; }
    const uint32_t total = table_end + lines * 6;
    Bytes b(total + pad, 0);
    std::memcpy(b.data(), "PSTCHT1\0", 8);
    w16(b, 8, 4); w16(b, 0xA, 0x40); w32(b, 0x10, n); w32(b, 0x14, flag0); w32(b, 0x18, lines);
    w32(b, 0x1C, 0x60); w32(b, 0x20, 0x40); w32(b, 0x24, table_end); w32(b, 0x28, total);
    w32(b, 0x2C, title); w32(b, 0x30, variant); w32(b, 0x34, game ? kCheatDbCrc : 0);
    b[0x38] = 2; b[0x39] = game;
    uint32_t cur = table_end;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t* e = b.data() + 0x40 + i * 0x60;
        std::memcpy(e, es[i].name.data(), es[i].name.size());
        w32(b, 0x40 + i * 0x60 + 0x40, es[i].a);
        w32(b, 0x40 + i * 0x60 + 0x44, es[i].b);
        w32(b, 0x40 + i * 0x60 + 0x48, cur);
        w16(b, 0x40 + i * 0x60 + 0x4C, es[i].lines.size());
        b[0x40 + i * 0x60 + 0x4E] = es[i].game;
        b[0x40 + i * 0x60 + 0x4F] = es[i].flags;
        const uint32_t start = cur;
        for (auto& l : es[i].lines) {
            b[cur] = l.addr >> 24; b[cur + 1] = l.addr >> 16; b[cur + 2] = l.addr >> 8; b[cur + 3] = l.addr;
            b[cur + 4] = l.val >> 8; b[cur + 5] = l.val;
            cur += 6;
        }
        w32(b, 0x40 + i * 0x60 + 0x50, crc(b.data() + start, cur - start));
    }
    seal(b, total);
    return b;
}

std::vector<E4> sample_v4() {
    return {
        {"ONE", 0x09, 1, 7, 5, {{0x80123456, 0x00FF}, {0x30000001, 0x0002}}},
        {"TWO", 0x0B, 1, 7, 6, {{0x80ABCDEF, 0x1234}}},
        {"CUSTOM", 0x04, 0, kNone, kNone, {{0x800000A0, 0xBEEF}}},
    };
}

const char kSampleText[] =
    "MHDialect=V5ORIG_BANK_V01\n"
    "[ONE]\nType=V5Native1\nMHFlags=01\n80123456 00FF\n30000001 0002\n\n"
    "[TWO (M)]\nType=V5Native1\nMHFlags=03\n80ABCDEF 1234\n\n"
    "[CUSTOM]\nType=V5Native1\nMHFlags=00\n800000A0 BEEF\n\n";

void test_v4_valid_and_text() {
    Bytes b = build_v4(1, 12, 7, sample_v4());
    CHECK(v4_validate(b.data(), b.size()));
    CHECK_EQ(blob_version(b.data(), b.size()), 4u);
    mh::card::TextBuf& t = mh::card::text();
    CHECK(v4_render(b.data(), t));
    CHECK_EQ(std::string(t.data), std::string(kSampleText));
    CHECK_EQ(t.len, std::strlen(kSampleText));

    // zero padding after `total` (the card slot tail) is accepted, anything else is not
    Bytes padded = build_v4(1, 12, 7, sample_v4(), 0x40);
    CHECK(v4_validate(padded.data(), padded.size()));
    padded.back() = 1;
    CHECK(!v4_validate(padded.data(), padded.size()));

    // game 0 blob: only custom entries, no title, no variant, no database CRC
    Bytes g0 = build_v4(0, kNone, kNone, {{"MINE", 0x04, 0, kNone, kNone, {{0x80000000, 1}}}});
    CHECK(v4_validate(g0.data(), g0.size()));
    Bytes g0bad = g0;
    w32(g0bad, 0x34, kCheatDbCrc);
    seal(g0bad, g0bad.size());
    CHECK(!v4_validate(g0bad.data(), g0bad.size()));
}

void test_v4_rejections() {
    const Bytes good = build_v4(1, 12, 7, sample_v4());
    struct Mut { const char* what; size_t off; uint32_t val; int width; };
    const Mut muts[] = {
        {"magic", 0, 'X', 1}, {"version", 8, 5, 2}, {"version 6", 8, 6, 2}, {"header size", 0xA, 0x41, 2}, {"+0xc", 0xC, 1, 4},
        {"count 0", 0x10, 0, 4}, {"count 2", 0x10, 2, 4}, {"count 0x101", 0x10, 0x101, 4}, {"flag0 count", 0x14, 3, 4},
        {"line total", 0x18, 5, 4}, {"entry size", 0x1C, 0x61, 4}, {"table offset", 0x20, 0x41, 4}, {"lines offset", 0x24, 0x100, 4},
        {"total short", 0x28, 0x100, 4}, {"title -1 with game", 0x2C, kNone, 4}, {"variant -1 with game", 0x30, kNone, 4},
        {"db crc", 0x34, kCheatDbCrc ^ 1, 4}, {"scope 1", 0x38, 1, 1}, {"scope 0", 0x38, 0, 1}, {"game 2", 0x39, 2, 1}, {"+0x3a", 0x3A, 1, 1},
        // entry 0 at 0x40: name at +0, ids at +0x40/+0x44, offset +0x48, lines +0x4c, game +0x4e, flags +0x4f, crc +0x50, pad +0x54
        {"name bracket", 0x40, '[', 1}, {"name bracket close", 0x41, ']', 1}, {"name control", 0x42, 0x1F, 1}, {"name 0x7f", 0x42, 0x7F, 1},
        {"name empty", 0x40, 0, 1}, {"name tail", 0x40 + 5, 'x', 1}, {"variant mismatch", 0x40 + 0x40, 8, 4}, {"id -1 audited", 0x40 + 0x44, kNone, 4},
        {"offset", 0x40 + 0x48, 0x1A0, 4}, {"lines 0", 0x40 + 0x4C, 0, 2}, {"lines too many", 0x40 + 0x4C, 3, 2}, {"entry game", 0x40 + 0x4E, 0, 1},
        {"flags 0", 0x40 + 0x4F, 0x00, 1}, {"flags both", 0x40 + 0x4F, 0x0D, 1}, {"flags 0x40", 0x40 + 0x4F, 0x49, 1},
        {"flags bit1 w/o bit0", 0x40 + 0x4F, 0x0A, 1}, {"flags bit4 w/o bit3", 0x40 + 0xC0 + 0x4F, 0x14, 1},
        {"custom with game", 0x40 + 0xC0 + 0x4E, 1, 1}, {"custom with variant", 0x40 + 0xC0 + 0x40, 7, 4}, {"custom with id", 0x40 + 0xC0 + 0x44, 3, 4},
        {"entry crc", 0x40 + 0x50, 0x12345678, 4}, {"entry pad", 0x40 + 0x54, 1, 1}, {"entry pad end", 0x40 + 0x5F, 1, 1},
        {"line byte", 0x40 + 3 * 0x60 + 2, 0x77, 1},  // damages the code lines, entry CRC must catch it
    };
    for (const Mut& m : muts) {
        Bytes b = good;
        if (m.width == 1) b[m.off] = static_cast<uint8_t>(m.val);
        else if (m.width == 2) w16(b, m.off, m.val);
        else w32(b, m.off, m.val);
        seal(b, r32(good, 0x28));  // only the mutated field may be the reason
        if (v4_validate(b.data(), b.size())) {
            std::fprintf(stderr, "v4 mutation accepted: %s\n", m.what);
            ++g_failures;
        }
        ++g_checks;
    }
    // truncated input and a header CRC that was not resealed
    CHECK(!v4_validate(good.data(), 0x3F));
    CHECK(!v4_validate(good.data(), good.size() - 1));
    Bytes unsealed = good;
    unsealed[0x3C] ^= 1;
    CHECK(!v4_validate(unsealed.data(), unsealed.size()));
    // game 1 needs at least one audited entry
    Bytes noaud = build_v4(1, 12, 7, {{"ONLY", 0x04, 0, kNone, kNone, {{0x80000000, 1}}}});
    CHECK(!v4_validate(noaud.data(), noaud.size()));
    // 0x101 lines: refused even though the sums agree (the profile allows 510)
    std::vector<Line> many(0x101, Line{0x80000000, 1});
    Bytes big = build_v4(0, kNone, kNone, {{"BIG", 0x04, 0, kNone, kNone, many}});
    CHECK(!v4_validate(big.data(), big.size()));
    many.pop_back();
    Bytes ok256 = build_v4(0, kNone, kNone, {{"BIG", 0x04, 0, kNone, kNone, many}});
    CHECK(v4_validate(ok256.data(), ok256.size()));
    // text longer than the buffer: validation passes, rendering refuses (256 entries of 50 characters)
    mh::card::TextBuf& t = mh::card::text();
    std::vector<E4> huge;
    for (int i = 0; i < 256; ++i) huge.push_back({"ENTRY" + std::to_string(i % 10), 0x04, 0, kNone, kNone, {{0x80000000, 2}}});
    Bytes hb = build_v4(0, kNone, kNone, huge);
    CHECK(v4_validate(hb.data(), hb.size()));
    CHECK(!v4_render(hb.data(), t));
    huge.resize(200);
    Bytes hb2 = build_v4(0, kNone, kNone, huge);
    CHECK(v4_render(hb2.data(), t));
    CHECK(t.len < kTextMax);
}

// ---- version 6 -------------------------------------------------------------------------------
struct E6 {
    std::string name;
    uint8_t flags;
    uint16_t id;
    std::vector<std::pair<uint8_t, uint32_t>> lines;  // opcode byte, low 24 bits of the address
    uint16_t value = 0x00AB;
};

Bytes build_v6(uint32_t title, const std::vector<E6>& es) {
    const uint32_t n = static_cast<uint32_t>(es.size());
    Bytes b(0x40 + n * 8, 0);
    uint32_t lines = 0, flag0 = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t name_off = b.size();
        b.insert(b.end(), es[i].name.begin(), es[i].name.end());
        b.push_back(0);
        const uint32_t lines_off = b.size();
        for (auto& l : es[i].lines) {
            b.push_back(l.first); b.push_back(l.second >> 16); b.push_back(l.second >> 8); b.push_back(l.second);
            b.push_back(es[i].value >> 8); b.push_back(es[i].value);
        }
        w16(b, 0x40 + i * 8, name_off); w16(b, 0x40 + i * 8 + 2, lines_off); w16(b, 0x40 + i * 8 + 4, es[i].id);
        b[0x40 + i * 8 + 6] = es[i].lines.size(); b[0x40 + i * 8 + 7] = es[i].flags;
        lines += es[i].lines.size();
        flag0 += es[i].flags & 1;
    }
    std::memcpy(b.data(), "PSTCHT1\0", 8);
    w16(b, 8, 6); w16(b, 0xA, 0x40); w32(b, 0x10, n); w32(b, 0x14, flag0); w32(b, 0x18, lines);
    w32(b, 0x1C, 8); w32(b, 0x20, 0x40); w32(b, 0x24, (n + 8) * 8); w32(b, 0x28, b.size());
    w32(b, 0x2C, title); w32(b, 0x30, title); w32(b, 0x34, kCheatDbCrc);
    b[0x38] = 2; b[0x39] = 1;
    seal(b, b.size());
    return b;
}

std::vector<E6> sample_v6() {
    return {
        {"INF LIVES", 0x09, 1, {{0x80, 0x123456}, {0x30, 0x000001}}},
        {"MAX-MONEY 2", 0x08, 2, {{0x80, 0xABCDEF}}, 0x1234},
        {"JOGO 'X'.", 0x18, 3, {{0xC1, 0x000000}, {0x00, 0x000000}, {0x21, 0x000002}, {0x70, 0x000003}, {0x90, 0x000004}, {0x31, 0x000005}, {0x20, 6}}},
    };
}

std::string reference_text(const Bytes& b) {
    std::string s = kTextHeader;
    char tmp[32];
    const uint32_t n = r32(b, 0x10);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* e = b.data() + 0x40 + i * 8;
        const uint32_t name_off = e[0] | e[1] << 8, lines_off = e[2] | e[3] << 8;
        s += "[" + std::string(reinterpret_cast<const char*>(b.data() + name_off)) + "]\nType=V5Native1\nMHFlags=";
        std::snprintf(tmp, sizeof tmp, "%02X\n", e[7] & 3);
        s += tmp;
        for (uint32_t k = 0; k < e[6]; ++k) {
            const uint8_t* l = b.data() + lines_off + k * 6;
            std::snprintf(tmp, sizeof tmp, "%02X%02X%02X%02X %02X%02X\n", l[0], l[1], l[2], l[3], l[4], l[5]);
            s += tmp;
        }
        s += "\n";
    }
    return s;
}

void test_v6_valid_and_chunks() {
    Bytes b = build_v6(0x100, sample_v6());
    CHECK(v6_validate(b.data(), b.size()));
    CHECK_EQ(blob_version(b.data(), b.size()), 6u);
    uint8_t out[kChunkBytes + 8];
    ChunkOut c = v6_render_chunk(b.data(), b.size(), 0, out);
    const std::string ref = reference_text(b);
    CHECK_EQ(c.len, ref.size());
    CHECK(c.last);
    CHECK(std::memcmp(out, ref.data(), c.len) == 0);
    CHECK(ref.find("[INF LIVES]\nType=V5Native1\nMHFlags=01\n80123456 00AB\n30000001 00AB\n\n") != std::string::npos);
    c = v6_render_chunk(b.data(), b.size(), 1, out);
    CHECK_EQ(c.len, 0u);
    CHECK(c.last);

    // a text of several chunks: every chunk equals the matching slice of the reference text
    std::vector<E6> big;
    for (int i = 0; i < 3; ++i) {
        E6 e{"BIG " + std::to_string(i), 0x08, static_cast<uint16_t>(i), {}};
        for (int k = 0; k < 160; ++k) e.lines.push_back({0x80, static_cast<uint32_t>(0x100000 + i * 0x1000 + k)});
        big.push_back(e);
    }
    Bytes bb = build_v6(5, big);
    CHECK(v6_validate(bb.data(), bb.size()));
    const std::string rb = reference_text(bb);
    CHECK(rb.size() > 3 * kChunkBytes);
    size_t pos = 0;
    for (uint32_t k = 0; pos < rb.size(); ++k) {
        c = v6_render_chunk(bb.data(), bb.size(), k, out);
        const size_t want = std::min<size_t>(kChunkBytes, rb.size() - pos);
        CHECK_EQ(c.len, want);
        CHECK(std::memcmp(out, rb.data() + pos, want) == 0);
        pos += want;
        CHECK_EQ(c.last, pos == rb.size());
    }
    // the chunk that ends exactly on the text end is the last one (len == 0x800, last set)
    c = v6_render_chunk(bb.data(), bb.size(), static_cast<uint32_t>(rb.size() / kChunkBytes) + 5, out);
    CHECK_EQ(c.len, 0u);
    CHECK(c.last);
}

void test_v6_rejections() {
    const Bytes good = build_v6(0x100, sample_v6());
    const uint32_t size = static_cast<uint32_t>(good.size());
    CHECK(v6_validate(good.data(), size));
    // entry i at 0x40 + 8 i: name_off +0, lines_off +2, id +4, lines +6, flags +7
    struct Mut { const char* what; size_t off; uint32_t val; int width; };
    const Mut muts[] = {
        {"magic", 1, 'x', 1}, {"version 4", 8, 4, 2}, {"version 7", 8, 7, 2}, {"header size", 0xA, 0x48, 2}, {"+0xc", 0xC, 1, 4},
        {"count 0", 0x10, 0, 4}, {"count 4", 0x10, 4, 4}, {"count 0x101", 0x10, 0x101, 4}, {"flag0", 0x14, 3, 4}, {"lines", 0x18, 9, 4},
        {"entry size", 0x1C, 0x60, 4}, {"table off", 0x20, 0x48, 4}, {"names off", 0x24, 0x60, 4}, {"total", 0x28, 0x99, 4},
        {"title big", 0x2C, 0x3EC, 4}, {"variant != title", 0x30, 0x101, 4}, {"db crc", 0x34, 0xA06CFC30, 4}, {"scope 1", 0x38, 1, 1},
        {"scope 3", 0x38, 3, 1}, {"scope 0", 0x38, 0, 1}, {"game 0", 0x39, 0, 1}, {"game 2", 0x39, 2, 1}, {"+0x3a", 0x3A, 1, 1},
        {"dup id", 0x40 + 8 + 4, 1, 2}, {"id 0x100", 0x40 + 4, 0x100, 2}, {"lines 0", 0x40 + 6, 0, 1}, {"lines+1", 0x40 + 6, 3, 1},
        {"flags 0", 0x40 + 7, 0x00, 1}, {"flags bit2", 0x40 + 7, 0x0D, 1}, {"flags bit6", 0x40 + 7, 0x49, 1}, {"flags bit7", 0x40 + 7, 0x89, 1},
        {"flags 3 w/o 8", 0x40 + 7, 0x01, 1}, {"flags&3==2", 0x40 + 8 + 7, 0x0A, 1},
        {"name_off", 0x40 + 8, 0x61, 2}, {"lines_off", 0x40 + 2, 0x4F, 2},
    };
    for (const Mut& m : muts) {
        Bytes b = good;
        if (m.width == 1) b[m.off] = static_cast<uint8_t>(m.val);
        else if (m.width == 2) w16(b, m.off, m.val);
        else w32(b, m.off, m.val);
        seal(b, size);
        if (v6_validate(b.data(), size)) {
            std::fprintf(stderr, "v6 mutation accepted: %s\n", m.what);
            ++g_failures;
        }
        ++g_checks;
    }
    CHECK(!v6_validate(good.data(), size - 1));
    CHECK(!v6_validate(good.data(), 0x3F));
    Bytes unsealed = good;
    unsealed[0x3E] ^= 4;
    CHECK(!v6_validate(unsealed.data(), size));
    // name alphabet, name length and opcode bytes
    auto with_name = [&](const std::string& n) { return build_v6(1, {{n, 0x08, 1, {{0x80, 1}}}}); };
    auto accepts = [&](const Bytes& b) { return v6_validate(b.data(), static_cast<uint32_t>(b.size())); };
    CHECK(accepts(with_name("A B&C'D-E.F0123456789")));
    CHECK(!accepts(with_name("lower")));
    CHECK(!accepts(with_name("A:B")));
    CHECK(!accepts(with_name("A_B")));
    CHECK(!accepts(with_name("A(B)")));
    CHECK(!accepts(with_name("")));
    CHECK(accepts(with_name(std::string(47, 'A'))));
    CHECK(!accepts(with_name(std::string(48, 'A'))));
    for (uint32_t op : {0x00u, 0x80u, 0x20u, 0x21u, 0x30u, 0x31u, 0x70u, 0x90u, 0xC1u}) {
        CHECK(accepts(build_v6(1, {{"A", 0x08, 1, {{static_cast<uint8_t>(op), 1}}}})));
    }
    for (uint32_t op : {0x01u, 0x10u, 0x22u, 0x40u, 0x71u, 0x91u, 0xC0u, 0xD0u, 0xFFu}) {
        CHECK(!accepts(build_v6(1, {{"A", 0x08, 1, {{static_cast<uint8_t>(op), 1}}}})));
    }
    // 0x1FF code lines are refused, 0x1FE are fine (several entries of at most 255 lines)
    auto lines_blob = [&](int total) {
        std::vector<E6> es;
        for (int left = total, i = 0; left > 0; ++i) {
            E6 e{"E" + std::to_string(i), 0x08, static_cast<uint16_t>(i), {}};
            for (int k = 0; k < std::min(left, 200); ++k) e.lines.push_back({0x80, 0x10});
            left -= static_cast<int>(e.lines.size());
            es.push_back(e);
        }
        return build_v6(1, es);
    };
    CHECK(accepts(lines_blob(0x1FE)));
    CHECK(!accepts(lines_blob(0x1FF)));
}

}  // namespace

int main() {
    test_v4_valid_and_text();
    test_v4_rejections();
    test_v6_valid_and_chunks();
    test_v6_rejections();
    TEST_MAIN_END();
}
