// Legacy "PSXRAW" loaders (V3 FUN_100273c4 / FUN_100270a8 / FUN_1002773c / FUN_10018ff2) on images built by
// tests/legacy_image.py, plus the fallback menu table (FUN_10030000 / FUN_100300b8).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "check.h"
#include "mh/aux_fallback.h"
#include "mh/aux_legacy.h"

using namespace mh::aux;

namespace {

std::vector<uint8_t> generate(const char* kind) {
    char path[64];
    std::snprintf(path, sizeof path, "/tmp/mh_aux_%s_%d.bin", kind, static_cast<int>(std::rand()));
    char cmd[256];
    std::snprintf(cmd, sizeof cmd, "python3 -I legacy_image.py %s %s", kind, path);
    if (std::system(cmd) != 0) {
        std::fprintf(stderr, "cannot run %s\n", cmd);
        std::exit(2);
    }
    std::vector<uint8_t> v;
    if (FILE* f = std::fopen(path, "rb")) {
        uint8_t buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
        std::fclose(f);
    }
    std::remove(path);
    return v;
}

struct Img {
    std::vector<uint8_t> d;
};
bool rd(void* ctx, uint32_t lba, uint32_t n, uint8_t* out) {
    auto& d = static_cast<Img*>(ctx)->d;
    if ((lba + n) * 512ull > d.size()) return false;
    std::memcpy(out, d.data() + lba * 512ull, n * 512u);
    return true;
}

void fix_hdr(uint8_t* b) { put_le32(b + 0x10, crc32_hole(b, 0x930, 0x10, 4)); }
void fix_rec(uint8_t* b, int i) {
    uint8_t* r = b + 0x130 + 0x100 * i;
    put_le32(r + 0xFC, crc32(r, 0xFC));
    fix_hdr(b);
}
void fix_map(uint8_t* m) { put_le32(m + 0xC, crc32_hole(m, 0x930, 0xC, 4)); }

void test_v1_header(const std::vector<uint8_t>& image) {
    LegacyV1 v;
    CHECK(legacy_v1_parse(image.data(), v));
    CHECK_EQ(v.disc_count, 3u);
    CHECK_EQ(v.default_disc, 1u);
    // disc 0: base 5, raw 10, 20 sectors -> 100 blocks; disc 1 base 110; disc 2 base 110 + 5 + 90
    const DiscEntry& d0 = v.discs[0];
    CHECK_EQ(d0.index(), 0u);
    CHECK_EQ(d0.case_index(), 0xFFu);
    CHECK_EQ(d0.slot(), 0u);
    CHECK_EQ(d0.track_count(), 1u);
    CHECK_EQ(le16(d0.b + 4), 0xFFFFu);
    CHECK_EQ(d0.base_lba(), 5u);
    CHECK_EQ(d0.track_lba(), 5u);
    CHECK_EQ(d0.raw_lba(), 10u);
    CHECK_EQ(d0.raw_blocks(), 100u);
    CHECK_EQ(d0.sectors(), 20u);
    CHECK(std::memcmp(d0.serial(), "SLUS-00001", 10) == 0);
    CHECK(std::memcmp(d0.title(), "FIRST DISC", 10) == 0);
    CHECK_EQ(v.discs[1].base_lba(), 110u);
    CHECK_EQ(v.discs[1].track_count(), 2u);
    CHECK_EQ(v.discs[2].base_lba(), 110u + 5 + 90);
    CHECK_EQ(v.discs[2].slot(), 2u);
}

void test_v1_header_rejects(const std::vector<uint8_t>& image) {
    LegacyV1 v;
    auto bad_hdr = [&](const char* name, auto f) {
        std::vector<uint8_t> h(image.begin(), image.begin() + kBlockBytes);
        f(h.data());
        fix_hdr(h.data());
        if (legacy_v1_parse(h.data(), v)) std::fprintf(stderr, "header accepted: %s\n", name);
        CHECK(!legacy_v1_parse(h.data(), v));
    };
    bad_hdr("magic", [](uint8_t* b) { b[5] = 'X'; });
    bad_hdr("version", [](uint8_t* b) { b[6] = 2; });
    bad_hdr("count 1", [](uint8_t* b) { b[7] = 1; });
    bad_hdr("count 0", [](uint8_t* b) { b[7] = 0; });
    bad_hdr("count 7", [](uint8_t* b) { b[7] = 7; });
    bad_hdr("default == count", [](uint8_t* b) { b[8] = 3; });
    bad_hdr("byte 9", [](uint8_t* b) { b[9] = 1; });
    bad_hdr("unit", [](uint8_t* b) { b[10] = 0x31; });
    bad_hdr("zero range", [](uint8_t* b) { b[0x14] = 1; });
    bad_hdr("zero range end", [](uint8_t* b) { b[0x12F] = 1; });
    bad_hdr("tail", [](uint8_t* b) { b[0x930] = 1; });
    bad_hdr("tail end", [](uint8_t* b) { b[0x9FF] = 1; });
    bad_hdr("unused record", [](uint8_t* b) { b[0x130 + 3 * 0x100 + 0x80] = 1; });
    {
        std::vector<uint8_t> h(image.begin(), image.begin() + kBlockBytes);
        h[0x40] ^= 1;  // header CRC must fail
        CHECK(!legacy_v1_parse(h.data(), v));
    }
    auto bad_rec = [&](const char* name, auto f) {
        std::vector<uint8_t> h(image.begin(), image.begin() + kBlockBytes);
        f(h.data() + 0x130 + 0x100);  // disc 1
        fix_rec(h.data(), 1);
        if (legacy_v1_parse(h.data(), v)) std::fprintf(stderr, "record accepted: %s\n", name);
        CHECK(!legacy_v1_parse(h.data(), v));
    };
    bad_rec("magic", [](uint8_t* r) { r[0] = 'X'; });
    bad_rec("used", [](uint8_t* r) { r[4] = 0; });
    bad_rec("id", [](uint8_t* r) { r[5] = 2; });
    bad_rec("case", [](uint8_t* r) { r[6] = 0; });
    bad_rec("slot", [](uint8_t* r) { r[7] = 0; });
    bad_rec("+8", [](uint8_t* r) { r[8] = 1; });
    bad_rec("base", [](uint8_t* r) { r[0x3C] += 1; });
    bad_rec("track", [](uint8_t* r) { r[0x44] += 1; });
    bad_rec("raw", [](uint8_t* r) { r[0x4C] += 1; });
    bad_rec("blocks", [](uint8_t* r) { r[0x54] += 1; });
    bad_rec("sectors", [](uint8_t* r) { r[0x5C] += 1; });
    bad_rec("sectors 0", [](uint8_t* r) { put_le32(r + 0x5C, 0); put_le32(r + 0x54, 0); });
    bad_rec("tracks 0", [](uint8_t* r) { r[0x60] = 0; });
    bad_rec("tracks 99", [](uint8_t* r) { r[0x60] = 99; });
    bad_rec("art", [](uint8_t* r) { r[0x62] = 0; });
    bad_rec("hi dword", [](uint8_t* r) { r[0x41] = 1; });
    bad_rec("zero +0x64", [](uint8_t* r) { r[0x64] = 1; });
    bad_rec("zero +0x94", [](uint8_t* r) { r[0xFB] = 1; });
    {
        std::vector<uint8_t> h(image.begin(), image.begin() + kBlockBytes);
        h[0x130 + 0x100 + 0x10] ^= 1;  // record CRC must fail
        fix_hdr(h.data());
        CHECK(!legacy_v1_parse(h.data(), v));
    }
    {
        std::vector<uint8_t> h(image.begin(), image.begin() + kBlockBytes);
        put_le32(h.data() + 0x130 + 0x5C, 450001);  // largest accepted is 450000 sectors
        put_le32(h.data() + 0x130 + 0x54, 450001 * 5);
        fix_rec(h.data(), 0);
        CHECK(!legacy_v1_parse(h.data(), v));
    }
}

void test_track_map(const std::vector<uint8_t>& image) {
    Img img{image};
    BlockReader r{rd, &img};
    LegacyV1 v;
    CHECK(legacy_v1_parse(image.data(), v));

    // disc 1: data 0..12, audio 12..18 with index 1 at 15; default disc builds the table
    DiscTable t;
    CHECK(legacy_v1_mount(image.data(), r, v, t));
    CHECK_EQ(t.track_count(), 2u);
    CHECK_EQ(le32(t.track(0) + 0x14), kTocOffset);
    CHECK_EQ(t.size(1), 12u);
    CHECK_EQ(t.file_offset(1), 0u);
    CHECK_EQ(t.index_count(1), 1u);
    CHECK_EQ(t.index0(1), kTocOffset);
    CHECK_EQ(t.index1(1), kTocOffset);
    CHECK_EQ(t.type(1), 2u);
    CHECK_EQ(t.size(2), 6u);
    CHECK_EQ(t.file_offset(2), 12u);
    CHECK_EQ(t.index0(2), 12 + kTocOffset);
    CHECK_EQ(t.index1(2), 15 + kTocOffset);
    CHECK_EQ(t.type(2), 1u);
    CHECK_EQ(t.file_offset(3), 18u);  // lead-out
    CHECK_EQ(t.index0(3), 18 + kTocOffset);
    CHECK_EQ(t.index1(3), 18 + kTocOffset);
    CHECK(t.has_data());
    CHECK(t.tail_flag());
    CHECK_EQ(t.raw_base, v.discs[1].raw_lba());
    CHECK_EQ(t.end_sector, 18u + 0x2454);
    CHECK_EQ(le32(t.b + 0x1474), 0u);

    // disc 2 carries an SBI block, disc 0 a single data track
    DiscTable t2;
    CHECK(legacy_track_map(v.discs[2], r, t2));
    CHECK_EQ(t2.type(1), 2u);  // map type 1 (MODE1) is a data track
    // mutation tests on disc 1's map
    const uint32_t lba = v.discs[1].track_lba();
    auto try_map = [&](const char* name, auto f, bool expect, int disc = 1) {
        Img m{image};
        uint8_t* blk = m.d.data() + v.discs[disc].track_lba() * 512ull;
        f(blk);
        BlockReader rr{rd, &m};
        DiscTable tt;
        bool ok = legacy_track_map(v.discs[disc], rr, tt);
        if (ok != expect) std::fprintf(stderr, "map %s: got %d\n", name, ok);
        CHECK_EQ(ok, expect);
    };
    (void)lba;
    try_map("unchanged", [](uint8_t*) {}, true);
    try_map("magic", [](uint8_t* m) { m[0] = 'X'; fix_map(m); }, false);
    try_map("count", [](uint8_t* m) { m[8] = 1; fix_map(m); }, false);
    try_map("entry size", [](uint8_t* m) { m[9] = 0x11; fix_map(m); }, false);
    try_map("sbi flag 2", [](uint8_t* m) { m[10] = 2; fix_map(m); }, false);
    try_map("bad crc", [](uint8_t* m) { m[0x20] ^= 1; }, false);
    try_map("tail", [](uint8_t* m) { m[0x930] = 1; fix_map(m); }, false);
    try_map("gap", [](uint8_t* m) { m[0x20] = 13; fix_map(m); }, false);       // entry 1 start 13
    try_map("len 0", [](uint8_t* m) { put_le32(m + 0x28, 0); fix_map(m); }, false);
    try_map("end != sectors", [](uint8_t* m) { m[0x28] = 5; fix_map(m); }, false);
    try_map("past sectors", [](uint8_t* m) { m[0x28] = 7; fix_map(m); }, false);
    try_map("index1 before start", [](uint8_t* m) { m[0x24] = 11; fix_map(m); }, false);
    try_map("index1 at end", [](uint8_t* m) { m[0x24] = 18; fix_map(m); }, false);
    try_map("type 3", [](uint8_t* m) { m[0x2C] = 3; fix_map(m); }, false);
    try_map("kind 16", [](uint8_t* m) { m[0x2D] = 16; fix_map(m); }, false);
    try_map("flag bits 8", [](uint8_t* m) { m[0x2E] = 8; fix_map(m); }, false);
    try_map("reserved", [](uint8_t* m) { m[0x2F] = 1; fix_map(m); }, false);
    try_map("first track audio", [](uint8_t* m) { m[0x1C] = 0; fix_map(m); }, false);
    try_map("beyond count", [](uint8_t* m) { m[0x10 + 2 * 16] = 1; fix_map(m); }, false);
    try_map("last zero entry", [](uint8_t* m) { m[0x10 + 98 * 16 + 15] = 1; fix_map(m); }, false);
    try_map("all data", [](uint8_t* m) { m[0x2C] = 2; fix_map(m); }, true);
    // disc 2 SBI
    try_map("sbi ok", [](uint8_t*) {}, true, 2);
    try_map("sbi magic", [](uint8_t* m) { m[0x648] = 'X'; fix_map(m); }, false, 2);
    try_map("sbi crc", [](uint8_t* m) { m[0x64A] ^= 1; fix_map(m); }, false, 2);
    try_map("sbi len 3", [](uint8_t* m) { m[0x640] = 3; fix_map(m); }, false, 2);
    try_map("sbi len 0x2E9", [](uint8_t* m) { m[0x640] = 0xE9; m[0x641] = 2; fix_map(m); }, false, 2);
    try_map("sbi flag clear ignores block", [](uint8_t* m) { m[10] = 0; m[0x648] = 'X'; fix_map(m); }, true, 2);
    try_map("sbi max length", [](uint8_t* m) {
        m[0x640] = 0xE8; m[0x641] = 2;
        std::memset(m + 0x64C, 0x11, 0x2E8 - 4);
        put_le32(m + 0x644, crc32(m + 0x648, 0x2E8));
        fix_map(m);
    }, true, 2);
    // an unreadable map
    Img empty{std::vector<uint8_t>(512 * 8)};
    BlockReader re{rd, &empty};
    CHECK(!legacy_track_map(v.discs[1], re, t2));
    DiscEntry z = v.discs[1];
    z.b[3] = 0;
    CHECK(!legacy_track_map(z, r, t2));
    z = v.discs[1];
    z.b[3] = 99;
    CHECK(!legacy_track_map(z, r, t2));
    z = v.discs[1];
    put_le32(z.b + 0x1C, 0);
    CHECK(!legacy_track_map(z, r, t2));
}

struct V2Fake {
    CaseView c;
    DiscEntry discs[5];
    const std::vector<uint8_t>* image;
};
bool fake_case(void* ctx, uint32_t idx, CaseView& out) {
    auto* f = static_cast<V2Fake*>(ctx);
    if (idx != f->c.index) return false;
    out = f->c;
    return true;
}
bool fake_disc(void* ctx, uint32_t id, DiscEntry& out) {
    auto* f = static_cast<V2Fake*>(ctx);
    if (id >= 5) return false;
    out = f->discs[id];
    return true;
}

void test_swap(const std::vector<uint8_t>& image) {
    Img img{image};
    BlockReader r{rd, &img};
    LegacyV1 v;
    CHECK(legacy_v1_parse(image.data(), v));
    DiscTable t;
    SwapResult s = legacy_swap_v1(v, 1, r, t);
    CHECK(s.status == SwapStatus::Ok);
    CHECK_EQ(s.next, 2u);
    CHECK_EQ(t.raw_base, v.discs[2].raw_lba());
    s = legacy_swap_v1(v, 2, r, t);  // wraps
    CHECK(s.status == SwapStatus::Ok);
    CHECK_EQ(s.next, 0u);
    CHECK_EQ(t.end_sector, 20u + 0x2454);
    LegacyV1 one = v;
    one.disc_count = 1;
    CHECK(legacy_swap_v1(one, 0, r, t).status == SwapStatus::Skipped);
    Img broken{image};
    broken.d[v.discs[0].track_lba() * 512ull + 0x100] ^= 1;
    BlockReader rb{rd, &broken};
    CHECK(legacy_swap_v1(v, 2, rb, t).status == SwapStatus::TrackMap);

    // mode 2: a Case with the three discs, ids 4, 2, 0 (slots 0..2)
    V2Fake f;
    f.image = &image;
    f.c.index = 9;
    f.c.disc_count = 3;
    const uint8_t ids[3] = {4, 2, 0};
    const int src[3] = {0, 1, 2};
    for (int k = 0; k < 3; ++k) {
        f.c.disc_ids[k] = ids[k];
        DiscEntry e = v.discs[src[k]];
        e.b[0] = ids[k];
        e.b[1] = 9;
        e.b[2] = static_cast<uint8_t>(k);
        f.discs[ids[k]] = e;
    }
    SwapEnv env{&f, fake_case, fake_disc};
    s = legacy_swap_v2(f.discs[4], env, r, t);
    CHECK(s.status == SwapStatus::Ok);
    CHECK_EQ(s.next, 1u);
    CHECK_EQ(s.entry.index(), 2u);
    CHECK_EQ(t.raw_base, v.discs[1].raw_lba());
    s = legacy_swap_v2(f.discs[0], env, r, t);  // last slot wraps to slot 0
    CHECK(s.status == SwapStatus::Ok);
    CHECK_EQ(s.next, 0u);
    CHECK_EQ(s.entry.index(), 4u);
    DiscEntry cur = f.discs[2];
    cur.b[1] = 0xFF;
    CHECK(legacy_swap_v2(cur, env, r, t).status == SwapStatus::CaseLookup);
    cur = f.discs[2];
    cur.b[1] = 3;  // unknown Case
    CHECK(legacy_swap_v2(cur, env, r, t).status == SwapStatus::CaseLookup);
    cur = f.discs[2];
    cur.b[2] = 0;  // slot does not point at this disc id
    CHECK(legacy_swap_v2(cur, env, r, t).status == SwapStatus::CaseLookup);
    cur = f.discs[2];
    cur.b[2] = 3;  // slot out of range
    CHECK(legacy_swap_v2(cur, env, r, t).status == SwapStatus::CaseLookup);
    f.c.disc_count = 1;  // a Case must have 2..5 discs
    CHECK(legacy_swap_v2(f.discs[4], env, r, t).status == SwapStatus::CaseLookup);
    f.c.disc_count = 3;
    f.discs[2].b[2] = 7;  // next disc record says another slot
    CHECK(legacy_swap_v2(f.discs[4], env, r, t).status == SwapStatus::DiscLookup);
    f.discs[2].b[2] = 1;
    f.discs[2].b[3] = 5;  // track map no longer matches
    CHECK(legacy_swap_v2(f.discs[4], env, r, t).status == SwapStatus::TrackMap);
}

void test_raw_header() {
    std::vector<uint8_t> s = generate("rawhdr");
    CHECK_EQ(s.size(), 512u);
    DiscTable t;
    CHECK(raw_header_parse(s.data(), t));
    CHECK_EQ(t.track_count(), 3u);
    CHECK_EQ(le32(t.track(0) + 0x14), kTocOffset);
    // entry 0: data, start 0, length 2000, no pregap
    CHECK_EQ(t.size(1), 2000u);
    CHECK_EQ(t.file_offset(1), 0u);
    CHECK_EQ(t.index0(1), kTocOffset);
    CHECK_EQ(t.index1(1), kTocOffset);
    CHECK_EQ(t.type(1), 2u);
    // entry 1: audio, start 2000, length 300, pregap 150
    CHECK_EQ(t.size(2), 300u);
    CHECK_EQ(t.file_offset(2), 2000u);
    CHECK_EQ(t.index0(2), kTocOffset + 2000 - 150);
    CHECK_EQ(t.index1(2), kTocOffset + 2000);
    CHECK_EQ(t.type(2), 1u);
    CHECK_EQ(t.index_count(2), 0u);  // not written by this reader
    CHECK_EQ(t.index1(3), kTocOffset + 2300);
    CHECK(t.has_data());
    // lead-out
    CHECK_EQ(t.file_offset(4), 2500u);
    CHECK_EQ(t.index0(4), kTocOffset + 2500);
    CHECK_EQ(t.end_sector, 2500u + 2 * kTocOffset);
    CHECK(t.tail_flag());
    std::vector<uint8_t> bad = s;
    bad[0] = 'X';
    CHECK(!raw_header_parse(bad.data(), t));
    bad = s;
    bad[4] = 'Z';
    CHECK(!raw_header_parse(bad.data(), t));
    bad = s;
    bad[0xC] = 49;
    CHECK(raw_header_parse(bad.data(), t));
    bad[0xC] = 50;  // would read past the 512 byte sector
    CHECK(!raw_header_parse(bad.data(), t));
    bad = s;
    bad[0xC] = 0;  // no tracks: lead-in and lead-out only
    CHECK(raw_header_parse(bad.data(), t));
    CHECK_EQ(t.end_sector, 0x2454u);
    CHECK_EQ(t.file_offset(1), 0u);
    CHECK(!t.has_data());
}

void test_fallback() {
    DiscTable t;
    fallback_table(t);
    // dword layout written by FUN_10030000
    CHECK_EQ(le32(t.b + 0 * 4), 1u);
    CHECK_EQ(le32(t.b + 6 * 4), 0x122Au);
    CHECK_EQ(le32(t.b + 0xF * 4), 0x5Du);
    CHECK_EQ(le32(t.b + 0x10 * 4), 0u);
    CHECK_EQ(le32(t.b + 0x11 * 4), 1u);
    CHECK_EQ(le32(t.b + 0x12 * 4), 0x122Au);
    CHECK_EQ(le32(t.b + 0x13 * 4), 0x122Au);
    CHECK_EQ(le32(t.b + 0x15 * 4), 2u);
    CHECK_EQ(le32(t.b + 0x1D * 4), 0x5Du);
    CHECK_EQ(le32(t.b + 0x1F * 4), 0x1287u);
    CHECK_EQ(le32(t.b + 0x20 * 4), 0x1287u);
    CHECK_EQ(t.b[0x1470], 1u);
    CHECK_EQ(le32(t.b + 0x1474), 0u);
    CHECK_EQ(t.b[0x147A], 1u);
    CHECK_EQ(t.raw_base, 0u);
    CHECK_EQ(t.end_sector, 0x24B1u);
    // decision
    CHECK(fallback_serves(true, 1, 0, 1, 0x96));
    CHECK(fallback_serves(true, 1, 0, 1, 0x96 + 0x5C));
    CHECK(!fallback_serves(true, 1, 0, 1, 0x96 + 0x5D));
    CHECK(!fallback_serves(true, 1, 0, 1, 0x95));
    CHECK(!fallback_serves(true, 1, 1, 1, 0x96));  // SD menu present
    CHECK(!fallback_serves(true, 2, 0, 1, 0x96));
    CHECK(!fallback_serves(false, 1, 0, 1, 0x96));
    CHECK(!fallback_serves(true, 1, 0, 2, 0x96));
    // conversion
    uint8_t img[kSectorBytes], scr[kSectorBytes];
    for (uint32_t i = 0; i < kSectorBytes; ++i) {
        img[i] = static_cast<uint8_t>(i * 3 + 1);
        scr[i] = static_cast<uint8_t>(i * 5 + 2);
    }
    uint32_t out[kSectorBytes / 2];
    fallback_convert(img, scr, out);
    for (uint32_t i = 0; i < kSectorBytes / 2; ++i) {
        uint32_t w = (le16(img + 2 * i) ^ le16(scr + 2 * i)) << 8;
        if (w & 0x100) w |= 0xFF;
        CHECK_EQ(out[i], w);
    }
    fallback_convert(nullptr, scr, out);
    CHECK_EQ(out[0], (le16(scr) << 8) | ((le16(scr) & 1) ? 0xFFu : 0u));
    std::vector<uint8_t> menu(0x1D * kSectorBytes);
    CHECK(fallback_sector(menu.data(), 0) == menu.data());
    CHECK(fallback_sector(menu.data(), 0x1C) == menu.data() + 0x1C * kSectorBytes);
    CHECK(fallback_sector(menu.data(), 0x1D) == nullptr);
    FallbackState fs;
    CHECK_EQ(fs.phase, 1u);
    CHECK_EQ(fs.menu_sectors, 0u);
    CHECK_EQ(legacy_mode_after_v2(0), 1u);
    CHECK_EQ(legacy_mode_after_v2(1), 2u);
}

}  // namespace

int main() {
    std::vector<uint8_t> image = generate("v1");
    CHECK(image.size() > kBlockBytes);
    test_v1_header(image);
    test_v1_header_rejects(image);
    test_track_map(image);
    test_swap(image);
    test_raw_header();
    test_fallback();
    TEST_MAIN_END();
}
