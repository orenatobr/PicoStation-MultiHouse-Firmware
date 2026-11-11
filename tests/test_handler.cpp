// Host tests of the PS1 command handler (src/mh/handler.cpp) against a mounted card image built by
// tests/card_image.py (A's generator) and patched here with cheat blobs. Expected bytes come from
// docs/re/handler.md, not from the code under test.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "mh/handler.h"

using namespace mh;
using namespace mh::handler;
using Bytes = std::vector<uint8_t>;

namespace {

Bytes g_img;
int g_init_result = 0;
int g_reads = 0;
int fake_init() { return g_init_result; }
int fake_read(uint32_t lba, uint32_t blocks, void* dst) {
    ++g_reads;
    if ((static_cast<uint64_t>(lba) + blocks) * 512 > g_img.size()) return 1;
    std::memcpy(dst, g_img.data() + static_cast<size_t>(lba) * 512, static_cast<size_t>(blocks) * 512);
    return 0;
}

Bytes make_image() {
    const char* out = "handler_test.img";
    if (std::system("python3 -I card_image.py handler_test.img basic") != 0) {
        std::fprintf(stderr, "image generator failed\n");
        std::exit(2);
    }
    FILE* f = std::fopen(out, "rb");
    Bytes v;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
    std::fclose(f);
    std::remove(out);
    return v;
}

uint8_t* at(uint32_t lba, uint32_t off = 0) { return g_img.data() + static_cast<size_t>(lba) * 512 + off; }
void p32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
uint32_t g32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint32_t g16(const uint8_t* p) { return p[0] | p[1] << 8; }
uint32_t crc(const void* p, uint32_t n) { return card::crc32(p, n); }

constexpr uint32_t kBase = 0x2800;  // package base of disc id n: kBase + 0x100 * n

void seal_disc(int id) { p32(at(0x32, id * 0x100 + 0xFC), crc(at(0x32, id * 0x100), 0xFC)); }

// Gives disc `id` a cheat blob (cheat_lba = base + 5, cheat_size = blob size).
void set_cheat(int id, const Bytes& blob) {
    const uint32_t base = kBase + 0x100 * id;
    std::memcpy(at(base + 5), blob.data(), blob.size());
    p32(at(0x32, id * 0x100 + 0x64), base + 5);
    p32(at(0x32, id * 0x100 + 0x68), 0);
    p32(at(0x32, id * 0x100 + 0x6C), static_cast<uint32_t>(blob.size()));
    seal_disc(id);
}

// ---- cheat blobs (fields: docs/re/handler.md section 4) ---------------------------------------
void w16(Bytes& b, size_t o, uint32_t v) { b[o] = v; b[o + 1] = v >> 8; }
void w32(Bytes& b, size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) b[o + i] = v >> (8 * i); }
void seal_blob(Bytes& b) {
    w32(b, 0x3C, 0);
    w32(b, 0x3C, crc(b.data(), static_cast<uint32_t>(b.size())));
}

// One custom v4 entry "NAME" with `nlines` lines 80000000+i / 0x1000+i.
Bytes blob_v4(const std::string& name, int nlines, bool corrupt = false) {
    const uint32_t table_end = 0x40 + 0x60, total = table_end + nlines * 6;
    Bytes b(total, 0);
    std::memcpy(b.data(), "PSTCHT1\0", 8);
    w16(b, 8, 4); w16(b, 0xA, 0x40); w32(b, 0x10, 1); w32(b, 0x18, nlines);
    w32(b, 0x1C, 0x60); w32(b, 0x20, 0x40); w32(b, 0x24, table_end); w32(b, 0x28, total);
    w32(b, 0x2C, 0xFFFFFFFF); w32(b, 0x30, 0xFFFFFFFF);
    b[0x38] = 2;
    std::memcpy(b.data() + 0x40, name.data(), name.size());
    w32(b, 0x40 + 0x40, 0xFFFFFFFF); w32(b, 0x40 + 0x44, 0xFFFFFFFF);
    w32(b, 0x40 + 0x48, table_end); w16(b, 0x40 + 0x4C, nlines); b[0x40 + 0x4F] = 0x04;
    for (int i = 0; i < nlines; ++i) {
        uint8_t* l = b.data() + table_end + i * 6;
        const uint32_t a = 0x80000000u + i, v = 0x1000 + i;
        l[0] = a >> 24; l[1] = a >> 16; l[2] = a >> 8; l[3] = a; l[4] = v >> 8; l[5] = v;
    }
    w32(b, 0x40 + 0x50, crc(b.data() + table_end, nlines * 6));
    seal_blob(b);
    if (corrupt) b[table_end] ^= 0x40;  // entry CRC no longer matches
    return b;
}

std::string text_v4(const std::string& name, int nlines) {
    std::string s = std::string(kTextHeader) + "[" + name + "]\nType=V5Native1\nMHFlags=00\n";
    char tmp[40];
    for (int i = 0; i < nlines; ++i) {
        std::snprintf(tmp, sizeof tmp, "%08X %04X\n", 0x80000000u + i, 0x1000 + i);
        s += tmp;
    }
    return s + "\n";
}

// v6: `n` entries "E<i>" with `nlines` lines each.
Bytes blob_v6(int n, int nlines, std::string* text) {
    Bytes b(0x40 + n * 8, 0);
    *text = kTextHeader;
    char tmp[64];
    for (int i = 0; i < n; ++i) {
        const uint32_t name_off = b.size();
        const std::string nm = "E" + std::to_string(i);
        b.insert(b.end(), nm.begin(), nm.end());
        b.push_back(0);
        const uint32_t lines_off = b.size();
        *text += "[" + nm + "]\nType=V5Native1\nMHFlags=00\n";
        for (int k = 0; k < nlines; ++k) {
            const uint32_t a = 0x80100000u + i * 0x1000 + k;
            b.push_back(a >> 24); b.push_back(a >> 16); b.push_back(a >> 8); b.push_back(a);
            b.push_back(0x00); b.push_back(0x7F);
            std::snprintf(tmp, sizeof tmp, "%08X 007F\n", a);
            *text += tmp;
        }
        *text += "\n";
        w16(b, 0x40 + i * 8, name_off); w16(b, 0x40 + i * 8 + 2, lines_off); w16(b, 0x40 + i * 8 + 4, i);
        b[0x40 + i * 8 + 6] = nlines; b[0x40 + i * 8 + 7] = 0x08;
    }
    std::memcpy(b.data(), "PSTCHT1\0", 8);
    w16(b, 8, 6); w16(b, 0xA, 0x40); w32(b, 0x10, n); w32(b, 0x18, n * nlines);
    w32(b, 0x1C, 8); w32(b, 0x20, 0x40); w32(b, 0x24, (n + 8) * 8); w32(b, 0x28, b.size());
    w32(b, 0x2C, 5); w32(b, 0x30, 5); w32(b, 0x34, kCheatDbCrc);
    b[0x38] = 2; b[0x39] = 1;
    seal_blob(b);
    return b;
}

// ---- harness ----------------------------------------------------------------------------------
card::CardDisc g_disc;

void setup() {
    g_img = make_image();
    g_init_result = 0;
    card::set_sd({fake_init, fake_read});
}

void mount() {
    std::memset(&g_disc, 0, sizeof g_disc);
    CHECK(card::mount(&g_disc));
    init(&g_disc);
    env() = Env{};
}

struct Resp {
    bool ok = false;
    Bytes v;
    const uint8_t* d() const { return v.data(); }
};
Resp rd(uint32_t lba, bool loc1 = true) {
    Resp r;
    const uint8_t* p = nullptr;
    r.ok = read_sector(lba, loc1, &p);
    if (r.ok) r.v.assign(p, p + card::kRespSize);
    return r;
}

void pad(uint32_t sub, uint32_t arg) { on_pad_command(0xF00000 | sub << 16 | (arg & 0xFFFF)); }
card::CardState& st() { return card::state(); }

std::string cstr(const void* v, size_t max) { const char* p = static_cast<const char*>(v); return std::string(p, strnlen(p, max)); }

// Selects Case `index` (n-th used) and requests the info page of Disc `slot`.
Resp select_info(uint32_t case_n, uint32_t slot) {
    pad(3, case_n);
    CHECK(rd(kLbaListing).ok);
    pad(0xC, slot);
    return rd(kLbaInfo);
}

// Delivers a PCR2 request for the selected disc. `tweak` may damage it before the CRC is computed.
Bytes make_request() {
    Bytes q(0x24, 0);
    p32(q.data(), kMagicPcr2);
    q[4] = 0x24;
    q[6] = st().sel_slot;
    p32(q.data() + 8, st().event.reserved14);
    p32(q.data() + 0xC, st().event.seq);
    std::memcpy(q.data() + 0x10, st().disc.serial, 16);
    return q;
}
void send_request(Bytes q, bool good_crc = true) {
    p32(q.data() + 0x20, good_crc ? crc(q.data(), 0x20) : 0x12345678);
    pad(6, 2);
    for (size_t i = 0; i < 0x24; i += 2) pad(7, q[i] << 8 | q[i + 1]);
}

// Checks a PCH2 chunk and returns its data.
std::string check_chunk(const Resp& r, uint32_t chunk, uint8_t status, bool last) {
    CHECK(r.ok);
    if (!r.ok) return "";
    const uint8_t* p = r.d();
    CHECK_EQ(g32(p), kMagicPch2);
    CHECK_EQ(g16(p + 4), 0x30u);
    CHECK_EQ(g16(p + 6), chunk);
    CHECK_EQ(p[0xA], status);
    CHECK_EQ(p[0xB], last ? 1u : 0u);
    CHECK_EQ(g16(p + 0xC), st().sel_slot);
    CHECK_EQ(g32(p + 0x10), st().event.reserved14);
    CHECK_EQ(g32(p + 0x14), st().event.seq);
    CHECK(std::memcmp(p + 0x18, st().disc.serial, 16) == 0);
    const uint32_t len = g16(p + 8);
    CHECK(len <= 0x800);
    CHECK_EQ(g32(p + 0x28), crc(p + 0x30, len));
    CHECK_EQ(g32(p + 0x2C), crc(p, 0x2C));
    return std::string(reinterpret_cast<const char*>(p + 0x30), len);
}

// ---- tests ------------------------------------------------------------------------------------
void test_routing_and_config() {
    setup();
    mount();
    CHECK_EQ(st().phase, 1u);
    Resp r = rd(kLbaListing);
    CHECK(r.ok);
    CHECK_EQ(cstr(r.d() + 2, r.d()[0]), std::string("Alundra 2"));
    CHECK_EQ(r.d()[1], 1u);                       // Case entries carry flag 1
    CHECK_EQ(st().cache_key, 100u);
    CHECK(!rd(kLbaListing, false).ok);            // location != 1
    CHECK(!rd(0x95).ok);                          // below the first data sector
    CHECK(!rd(0x96 + 0x5C).ok);                   // SD menu image sectors are not answered
    CHECK(rd(0x96 + 0x5D).ok);                    // first virtual sector (zero page)
    CHECK(card::all_zero(rd(0x96 + 0x5D).d(), 0x914));
    st().magic = 0;
    CHECK(!rd(kLbaListing).ok);                   // state not initialised
    mount();

    r = rd(kLbaConfig);
    CHECK(r.ok);
    CHECK_EQ(g32(r.d()), 0x31474643u);            // "CFG1"
    CHECK_EQ(g16(r.d() + 4), 0x800u);
    CHECK(std::memcmp(r.d() + 0x114, at(0x1685), 0x800) == 0);
    CHECK_EQ(cstr(r.d() + 0x114, 8), std::string("PSTCFG2"));
    CHECK_EQ(st().cache_key, 0x96u);

    // unreadable config slot: "DEAD"
    st().cfg_lba = 0x7FFFFFF;
    card::reset_response_cache();
    r = rd(kLbaConfig);
    CHECK(r.ok);
    CHECK_EQ(g32(r.d()), 0x44414544u);
}

void test_list_requests() {
    setup();
    mount();
    pad(3, 1);  // select the second Case
    CHECK_EQ(st().cmd, 3u);
    CHECK_EQ(st().cmd_arg, 1u);
    Resp r = rd(kLbaListing);
    CHECK(r.ok);
    CHECK_EQ(st().sel_case, 1u);
    CHECK_EQ(st().cmd, 0u);
    CHECK_EQ(st().list_offset, 0u);
    // original: Disc entries carry flag 0 (FUN_10022284 called with r2 = 0); card.cpp writes 1, see docs
    CHECK_EQ(cstr(r.d() + 2, r.d()[0]), std::string("Chrono Cross (Disc 1) - DISCO 1"));
    uint32_t pos = 2 + r.d()[0];
    CHECK_EQ(cstr(r.d() + pos + 2, r.d()[pos]), std::string("Chrono Cross (Disc 2) - DISCO 2"));
    pad(4, 1);  // page offset 1
    CHECK_EQ(st().list_offset, 1u);
    r = rd(kLbaListing);
    CHECK_EQ(cstr(r.d() + 2, r.d()[0]), std::string("Chrono Cross (Disc 2) - DISCO 2"));
    pad(2, 0);  // back to all Cases
    CHECK_EQ(st().list_offset, 0u);
    r = rd(kLbaListing);
    CHECK_EQ(st().sel_case, 0xFFFFFFFFu);
    CHECK_EQ(cstr(r.d() + 2, r.d()[0]), std::string("Alundra 2"));
    pad(3, 99);  // no such Case: back to the whole library
    rd(kLbaListing);
    CHECK_EQ(st().sel_case, 0xFFFFFFFFu);
    pad(1, 0);
    CHECK_EQ(st().cmd, 1u);
    // the listing is served from cache until a command invalidates it
    r = rd(kLbaListing);
    CHECK(r.ok);
    CHECK_EQ(st().cache_key, 100u);
    // an unrelated sector with a pending list request leaves the request alone
    pad(3, 1);
    CHECK(rd(kLbaConfig).ok);
    CHECK_EQ(st().cmd, 3u);
}

void test_art() {
    setup();
    mount();
    pad(0xB, 1);  // art of the n-th used Case (1: Chrono Cross, art id 1)
    Resp r = rd(kLbaArt);
    CHECK(r.ok);
    CHECK_EQ(st().art_block, 0xB4u + 0x14u);
    CHECK_EQ(st().cmd, 0u);
    CHECK_EQ(g32(r.d()), 0x54524150u);            // "PART"
    CHECK_EQ(r.d()[4], 0u);
    CHECK_EQ(r.d()[5], 5u);
    CHECK(std::memcmp(r.d() + 6, at(0xB4 + 0x14), 0x842) == 0);
    for (uint32_t k = 1; k < 5; ++k) {
        r = rd(kLbaArt + k);
        const uint32_t len = k < 4 ? 0x842 : 0x2800 - 4 * 0x842;
        CHECK_EQ(g32(r.d()), 0x54524150u);
        CHECK_EQ(r.d()[4], k);
        CHECK(std::memcmp(r.d() + 6, at(0xB4 + 0x14) + k * 0x842, len) == 0);
        CHECK(card::all_zero(r.d() + 6 + len, 0x914 - 6 - len));
    }
    for (uint32_t k = 5; k < 10; ++k) {
        r = rd(kLbaArt + k);
        CHECK_EQ(g32(r.d()), 0x54524146u);        // "FART"
        CHECK(card::all_zero(r.d() + 4, 0x914 - 4));
    }
    // Disc art of a Disc without art id, and of a missing Disc
    pad(3, 0);
    rd(kLbaListing);
    pad(0xB, 0);
    r = rd(kLbaArt);
    CHECK_EQ(st().art_block, 0u);
    CHECK_EQ(g32(r.d()), 0x54524146u);
    pad(0xB, 1);
    rd(kLbaArt);
    CHECK_EQ(st().art_block, 0u);
    // mode 5 outside the art window is not consumed
    pad(0xB, 1);
    rd(kLbaConfig);
    CHECK_EQ(st().cmd, 5u);
}

void test_disc_info() {
    setup();
    mount();
    Resp r = select_info(1, 1);  // Chrono Cross, disc 2: serial SLUS-01042
    CHECK(r.ok);
    const uint8_t* p = r.d();
    CHECK_EQ(g32(p), kMagicPgm1);
    CHECK_EQ(p[4], 1u);
    CHECK_EQ(p[5], 1u);
    CHECK_EQ(g16(p + 6), 1u);
    card::DiscInfo d;
    CHECK(card::disc_by_id(2, &d));
    const uint32_t x = (1u << 8) ^ st().generation ^ d.record_crc ^ d.data_crc32 ^ (2u << 24);
    const uint32_t token = x == 0x314C4553u ? 1u : x ^ 0x314C4553u;
    CHECK_EQ(g32(p + 8), token);
    CHECK_EQ(p[0xC], 2u);                          // no cheats
    CHECK_EQ(cstr(p + 0x10, 16), std::string("SLUS-01042"));
    CHECK_EQ(cstr(p + 0x20, 0x20), std::string("Chrono Cross (Disc 2)"));
    CHECK_EQ(cstr(p + 0x80, 0x80), std::string("cdrom:\\SLUS_010.42;1"));
    CHECK_EQ(g32(p + 0x100), crc(p, 0x100));
    CHECK(card::all_zero(p + 0x104, 0x914 - 0x104));
    CHECK_EQ(st().cache_key, 400u);
    CHECK_EQ(st().sel_slot, 1u);
    CHECK_EQ(st().sel_disc_id, 2u);
    CHECK_EQ(cstr(st().disc.serial, 16), std::string("SLUS-01042"));
    CHECK_EQ(cstr(st().game_id, 0x80), std::string("cdrom:\\SLUS_010.42;1"));
    const card::CardEvent& ev = st().event;
    CHECK_EQ(ev.reserved14, token);
    CHECK_EQ(ev.token, 1u);
    uint32_t sess = ((token >> 16) ^ token) & 0x7FFF;
    CHECK_EQ(ev.session, sess ? sess : 1u);
    CHECK_EQ(ev.reserved18, st().generation);
    CHECK_EQ(ev.reserved1c, d.data_crc32);
    CHECK_EQ(ev.flags, 0xDu);                      // 9, then post_event ORs 4
    CHECK_EQ(ev.code, 0u);
    CHECK_EQ(st().cmd, 0u);

    // a repeat read is served from cache, byte for byte
    Resp again = rd(kLbaInfo);
    CHECK(again.v == r.v);

    // quirk of the original: 0x226 lies inside the cheat window (chunk 100), so once the cache is gone an
    // info read is answered as cheat chunk 100 (no request pending: status 3), not as PGM1
    rd(kLbaConfig);
    Resp stale = rd(kLbaInfo);
    CHECK(stale.ok);
    CHECK_EQ(g32(stale.d()), kMagicPch2);
    CHECK_EQ(g16(stale.d() + 6), 100u);
    CHECK_EQ(stale.d()[0xA], 3u);

    // invalid serial (case 3: "XXXXXXXXXXXXXXXY"): status 2, no serial and no boot path
    mount();
    r = select_info(3, 0);
    p = r.d();
    CHECK_EQ(g32(p), kMagicPgm1);
    CHECK_EQ(p[5], 2u);
    CHECK_EQ(p[0xC], 2u);
    CHECK(card::all_zero(p + 0x10, 16));
    CHECK(card::all_zero(p + 0x80, 0x80));
    CHECK_EQ(cstr(p + 0x20, 0x20), std::string(32, 'T'));
    CHECK_EQ(g32(p + 0x100), crc(p, 0x100));
    CHECK_EQ(st().event.flags, 0xDu);

    // unknown slot: the card layer's error page and event 10
    mount();
    r = select_info(0, 3);
    CHECK(r.ok);
    CHECK_EQ(g32(r.d()), kMagicPgm1);
    CHECK_EQ(r.d()[5], 3u);
    CHECK_EQ(st().event.code, 8u);                 // post_event_ack(10, slot)
    CHECK_EQ(st().event.arg, 10u);
    CHECK_EQ(st().event.extra, 3u);

    // serial format
    CHECK(serial_valid("SLUS-01234"));
    CHECK(!serial_valid("SLUS_01234"));
    CHECK(!serial_valid("slus-01234"));
    CHECK(!serial_valid("SLUS-0123A"));
    CHECK(!serial_valid("SLUS-012345"));
    CHECK(!serial_valid("SLUS-0123"));
    char path[32];
    build_boot_path("SCES-12345", path);
    CHECK_EQ(std::string(path), std::string("cdrom:\\SCES_123.45;1"));
}

void test_cheat_v6() {
    setup();
    std::string text;
    Bytes blob = blob_v6(3, 160, &text);
    set_cheat(0, blob);
    mount();
    Resp r = select_info(0, 0);
    CHECK_EQ(r.d()[0xC], 3u);                      // has cheats
    send_request(make_request());
    CHECK_EQ(st().cmd, 11u);
    CHECK_EQ(st().pcr2_ready, 1u);
    const size_t chunks = (text.size() + 0x7FF) / 0x800;
    CHECK(chunks >= 3);
    std::string got;
    for (uint32_t k = 0; k < chunks; ++k) {
        const int reads_before = g_reads;
        const bool last = k + 1 == chunks;
        got += check_chunk(rd(kLbaCheat + k), k, 0, last);
        if (k > 0) CHECK_EQ(g_reads, reads_before);  // validated blob is cached after chunk 0
    }
    CHECK(got == text);
    check_chunk(rd(kLbaCheat + chunks), static_cast<uint32_t>(chunks), 2, true);  // past the end
    CHECK_EQ(st().cmd, 11u);                       // the fast path does not consume the request

    // a damaged blob: status 3
    Bytes bad = blob;
    bad[0x50] ^= 1;
    set_cheat(0, bad);
    mount();
    select_info(0, 0);
    send_request(make_request());
    check_chunk(rd(kLbaCheat), 0, 3, true);

    // disc without cheats: status 1
    mount();
    select_info(1, 0);
    send_request(make_request());
    check_chunk(rd(kLbaCheat), 0, 1, true);

    // request faults, one field at a time
    set_cheat(0, blob);
    struct Fault { const char* what; int off; uint8_t xor_; };
    const Fault faults[] = {{"magic", 0, 1}, {"size", 4, 1}, {"slot", 6, 1}, {"token", 8, 1}, {"seq", 0xC, 1}, {"serial", 0x10, 1}, {"serial last", 0x1F, 1}};
    for (const Fault& f : faults) {
        mount();
        select_info(0, 0);
        Bytes q = make_request();
        q[f.off] ^= f.xor_;
        send_request(q);
        const Resp c = rd(kLbaCheat);
        if (!(c.ok && c.d()[0xA] == 3 && c.d()[0xB] == 1 && g16(c.d() + 8) == 0)) {
            std::fprintf(stderr, "request fault accepted: %s\n", f.what);
            ++g_failures;
        }
        ++g_checks;
    }
    mount();
    select_info(0, 0);
    send_request(make_request(), false);
    check_chunk(rd(kLbaCheat), 0, 3, true);        // bad request CRC
    mount();
    select_info(0, 0);
    check_chunk(rd(kLbaCheat), 0, 3, true);        // no request at all
    mount();
    select_info(0, 0);
    send_request(make_request());
    st().event.code = 1;                           // last event is not 0
    check_chunk(rd(kLbaCheat), 0, 3, true);
    mount();
    select_info(0, 0);
    send_request(make_request());
    st().event.flags &= ~1u;                       // session flag bit0 clear
    check_chunk(rd(kLbaCheat), 0, 3, true);

    // the hold flag disables the fast path; the main handler refuses version 6 (status 3)
    mount();
    select_info(0, 0);
    send_request(make_request());
    env().hold = true;
    check_chunk(rd(kLbaCheat), 0, 3, true);
    env().hold = false;

    // outside the menu phase the fast path steps aside; game phase answers only N=900
    mount();
    select_info(0, 0);
    send_request(make_request());
    st().phase = card::kPhaseGame;
    CHECK(!rd(kLbaCheat).ok);
    CHECK(!rd(kLbaListing).ok);
    CHECK(rd(kLbaSession).ok);
}

void test_cheat_v4_main() {
    setup();
    set_cheat(0, blob_v4("NAME", 3));
    mount();
    select_info(0, 0);
    send_request(make_request());
    const std::string text = text_v4("NAME", 3);
    CHECK(text.size() < 0x800);
    CHECK_EQ(check_chunk(rd(kLbaCheat), 0, 0, true), text);
    CHECK_EQ(st().cmd, 0u);                        // the main handler consumed the request
    CHECK_EQ(st().cheat_status, 0u);
    CHECK_EQ(st().cheat_bytes, text.size());
    check_chunk(rd(kLbaCheat + 1), 1, 2, true);    // past the end
    check_chunk(rd(kLbaCheat + 0x1FF), 0x1FF, 2, true);

    // several chunks
    set_cheat(0, blob_v4("BIGNAME", 200));
    mount();
    select_info(0, 0);
    send_request(make_request());
    const std::string big = text_v4("BIGNAME", 200);
    std::string got;
    const size_t chunks = (big.size() + 0x7FF) / 0x800;
    CHECK(chunks >= 2);
    for (uint32_t k = 0; k < chunks; ++k) got += check_chunk(rd(kLbaCheat + k), k, 0, k + 1 == chunks);
    CHECK(got == big);

    // damaged entry CRC: status 3, and the verdict sticks for the session
    set_cheat(0, blob_v4("NAME", 3, true));
    mount();
    select_info(0, 0);
    send_request(make_request());
    check_chunk(rd(kLbaCheat), 0, 3, true);
    check_chunk(rd(kLbaCheat + 1), 1, 3, true);
    CHECK_EQ(st().cheat_status, 3u);

    // a new request restarts the load (pcr2 receive start clears cheat_pending)
    set_cheat(0, blob_v4("NAME", 3));
    mount();
    select_info(0, 0);
    send_request(make_request());
    check_chunk(rd(kLbaCheat), 0, 0, true);
    CHECK_EQ(st().cheat_pending, 1u);
    pad(6, 2);
    CHECK_EQ(st().cheat_pending, 0u);
    CHECK_EQ(st().pcr2_ready, 0u);
    pad(6, 0);
    CHECK_EQ(st().cmd, 0u);

    // no cheat blob on the disc: status 1
    mount();
    select_info(1, 1);
    send_request(make_request());
    check_chunk(rd(kLbaCheat), 0, 1, true);  // answered by the v6 fast path
}

void test_session() {
    setup();
    mount();
    select_info(0, 0);
    const uint32_t sess = st().event.session;
    CHECK(sess != 0);
    uint32_t seq = st().event.seq;

    // wrong slot: event 8 with argument 2
    pad(5, 1);
    Resp r = rd(kLbaSession);
    CHECK(r.ok);
    CHECK_EQ(st().event.code, 8u);
    CHECK_EQ(st().event.arg, 2u);
    CHECK_EQ(st().event.extra, 1u);
    CHECK_EQ(st().event.seq, seq + 1);
    CHECK_EQ(st().phase, 1u);
    CHECK_EQ(g32(r.d()), kMagicPls1);
    CHECK_EQ(g32(r.d() + 0x24), crc(r.d(), 0x24));
    CHECK_EQ(r.d()[0x10], 8u);

    // confirm before any switch: the last event is not 2 or 6
    pad(0xD, sess);
    rd(kLbaSession);
    CHECK_EQ(st().event.code, 8u);
    CHECK_EQ(st().event.arg, 4u);
    // wrong session id
    pad(0xD, sess ^ 1);
    rd(kLbaSession);
    CHECK_EQ(st().event.arg, 3u);
    pad(0xE, sess ^ 1);
    rd(kLbaSession);
    CHECK_EQ(st().event.arg, 3u);
    pad(0xE, 0x8000 | (sess ^ 1));
    rd(kLbaSession);
    CHECK_EQ(st().event.arg, 3u);
    // finish before confirm: wrong event
    pad(0xE, sess);
    rd(kLbaSession);
    CHECK_EQ(st().event.arg, 4u);

    // proper switch
    seq = st().event.seq;
    pad(5, 0);
    r = rd(kLbaSession);
    CHECK_EQ(st().phase, 2u);
    CHECK_EQ(st().event.code, 2u);
    CHECK_EQ(st().event.seq, seq + 2);             // events 1 and 2
    CHECK_EQ(st().event.flags & 2, 2u);
    CHECK_EQ(card::globals().raw_base, st().disc.raw_lba);
    CHECK_EQ(card::globals().sector_cursor, 0u);
    CHECK_EQ(card::globals().total_sectors, st().disc.sectors + 2 * card::kLeadIn);
    CHECK_EQ(r.d()[0x10], 2u);
    CHECK_EQ(g32(r.d() + 8), seq + 2);
    CHECK_EQ(st().cmd, 0u);
    CHECK(g_disc.cue.track_count != 0);            // the track map was loaded into the disc table
    // game phase: only N=900 is answered, the cached page is served again
    CHECK(!rd(kLbaListing).ok);
    CHECK(!rd(kLbaConfig).ok);
    CHECK(rd(kLbaSession).ok);

    // confirm (code 2 is confirmable): events 3 then 4
    seq = st().event.seq;
    pad(0xD, sess);
    rd(kLbaSession);
    CHECK_EQ(st().event.code, 4u);
    CHECK_EQ(st().event.seq, seq + 2);
    // finish: events 5 then 9, flags 0x10|0x20, the page read moves to phase 4
    seq = st().event.seq;
    pad(0xE, sess);
    r = rd(kLbaSession);
    CHECK(r.ok);
    CHECK_EQ(r.d()[0x10], 9u);
    CHECK_EQ(st().event.seq, seq + 2);
    CHECK_EQ(st().event.flags & 0x30, 0x30u);
    CHECK_EQ(st().switch_count, 1u);
    CHECK_EQ(st().phase, 4u);
    // phase 4: other sectors are answered again, but the session page keeps counting
    CHECK(rd(kLbaListing).ok);
    rd(kLbaSession);
    CHECK_EQ(st().switch_count, 2u);
}

void test_session_cancel_and_failures() {
    // cancel after a switch: back to the menu, flags 0x32 cleared
    setup();
    mount();
    select_info(0, 0);
    const uint32_t sess = st().event.session;
    pad(5, 0);
    rd(kLbaSession);
    CHECK_EQ(st().phase, 2u);
    uint32_t seq = st().event.seq;
    pad(0xE, 0x8000 | sess);
    rd(kLbaSession);
    CHECK_EQ(st().phase, 1u);
    CHECK_EQ(st().event.code, 7u);
    CHECK_EQ(st().event.seq, seq + 2);             // events 6 and 7
    CHECK_EQ(st().event.flags & 0x32, 0u);
    CHECK(rd(kLbaListing).ok);

    // a damaged track map: the switch fails, event 8 with argument 7, menu phase
    setup();
    g_img[static_cast<size_t>(kBase) * 512 + 0xC] ^= 0xFF;  // track map CRC
    mount();
    select_info(0, 0);
    pad(5, 0);
    rd(kLbaSession);
    CHECK_EQ(st().phase, 1u);
    CHECK_EQ(st().event.code, 8u);
    CHECK_EQ(st().event.arg, 7u);
    CHECK_EQ(st().event.flags & 2, 0u);

    // no disc table registered: the switch falls back to the menu
    setup();
    mount();
    init(nullptr);
    select_info(0, 0);
    pad(5, 0);
    rd(kLbaSession);
    CHECK_EQ(st().phase, 1u);
    CHECK_EQ(st().event.arg, 7u);

    // bit0 of the session flags missing: refused with argument 2
    setup();
    mount();
    select_info(0, 0);
    st().event.flags &= ~1u;
    pad(5, 0);
    rd(kLbaSession);
    CHECK_EQ(st().event.code, 8u);
    CHECK_EQ(st().event.arg, 2u);
}

void test_hooks() {
    // DEAD hook: while a fresh listing is pending the SD is probed
    setup();
    mount();
    g_init_result = 1;
    pad(1, 0);
    Resp r = rd(kLbaListing);
    CHECK(r.ok);
    CHECK_EQ(g32(r.d()), 0x44414544u);
    CHECK(card::all_zero(r.d() + 4, 0x914 - 4));
    CHECK_EQ(st().cmd, 0u);
    CHECK_EQ(st().cache_key, 0xFFFFFFFFu);
    // SD healthy: the normal listing follows
    g_init_result = 0;
    pad(1, 0);
    r = rd(kLbaListing);
    CHECK_EQ(cstr(r.d() + 2, r.d()[0]), std::string("Alundra 2"));
    // busy service flag skips the probe
    g_init_result = 1;
    env().service_busy = true;
    pad(2, 0);
    r = rd(kLbaListing);
    CHECK_EQ(cstr(r.d() + 2, r.d()[0]), std::string("Alundra 2"));
    env().service_busy = false;
    // other commands do not probe
    pad(3, 0);
    r = rd(kLbaListing);
    CHECK_EQ(st().sel_case, 0u);
    g_init_result = 0;

    // 0xF9 confirmation after a switch: armed, judged, then N=900 is left to the disc image
    setup();
    mount();
    select_info(0, 0);
    const uint32_t sess = st().event.session;
    pad(5, 0);
    rd(kLbaSession);
    CHECK_EQ(st().event.code, 2u);
    on_confirm_command(0xF90000 | (sess ^ 1));     // wrong id: not armed
    CHECK_EQ(st().cmd, 0u);
    on_confirm_command(0xF80000 | sess);           // wrong command: not armed
    CHECK_EQ(st().cmd, 0u);
    on_confirm_command(0xF90000 | sess);           // armed without bit 15
    CHECK_EQ(st().cmd, 12u);
    CHECK_EQ(st().cmd_arg, sess);
    finish_confirm();
    CHECK_EQ(st().cmd, 0u);
    CHECK_EQ(st().cmd_arg, sess);                  // denied: only the 16 bit word remains
    CHECK(rd(kLbaSession).ok);
    on_confirm_command(0xF90000 | 0x8000 | sess);
    CHECK_EQ(st().cmd, 12u);
    finish_confirm();
    CHECK_EQ(st().cmd, 0u);
    CHECK_EQ(st().cmd_arg, 0x80008000u | sess);    // confirmed: bit 31 set
    CHECK(!rd(kLbaSession).ok);                    // reject hook
    CHECK(!rd(kLbaSession, false).ok);
    st().cmd_arg = sess;                           // without bit 31 the page is served
    CHECK(rd(kLbaSession).ok);
    finish_confirm();                              // nothing pending: no change
    CHECK_EQ(st().cmd_arg, sess);
    // not armed outside a running game
    st().phase = 1;
    on_confirm_command(0xF90000 | sess);
    CHECK_EQ(st().cmd, 0u);

    // abort_cheat_request drops a complete PCR2 request
    setup();
    mount();
    select_info(0, 0);
    send_request(make_request());
    CHECK_EQ(st().cmd, 11u);
    abort_cheat_request();
    CHECK_EQ(st().cmd, 0u);
    abort_cheat_request();                         // idempotent
}

void test_pad_commands() {
    setup();
    mount();
    on_pad_command(0x00710000);                    // (word & 0xF00000) != 0xF00000: not a command
    CHECK_EQ(st().cmd, 0u);
    on_pad_command(0x00D10000);
    CHECK_EQ(st().cmd, 0u);
    pad(1, 0x1234);
    CHECK_EQ(st().cmd, 1u);
    CHECK_EQ(st().cmd_arg, 0x1234u);
    CHECK_EQ(st().list_offset, 0u);
    pad(4, 7);
    CHECK_EQ(st().cmd, 4u);
    CHECK_EQ(st().list_offset, 7u);
    pad(0, 0);
    CHECK_EQ(st().cmd, 0u);
    const struct { uint32_t sub, cmd; } map[] = {{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 7}, {0xB, 5}, {0xC, 6}, {0xD, 8}};
    for (auto m : map) {
        pad(m.sub, 9);
        CHECK_EQ(st().cmd, m.cmd);
    }
    pad(0xE, 0x0123);
    CHECK_EQ(st().cmd, 9u);
    CHECK_EQ(st().cmd_arg, 0x0123u);
    pad(0xE, 0x8123);
    CHECK_EQ(st().cmd, 10u);
    CHECK_EQ(st().cmd_arg, 0x0123u);
    pad(9, 1);                                     // unassigned sub commands only invalidate the cache
    CHECK_EQ(st().cmd, 10u);
    CHECK_EQ(st().cache_key, 0xFFFFFFFFu);
    pad(0xA, 1);
    CHECK_EQ(st().cmd, 10u);
    pad(0xF, 1);
    CHECK_EQ(st().cmd, 10u);

    // PCR2 packet assembly: 18 words, the last completes it
    pad(0, 0);
    pad(7, 0x1111);                                // ignored: not receiving
    CHECK_EQ(st().pcr2_fill, 0u);
    pad(6, 2);
    CHECK_EQ(st().pcr2_mode, 2u);
    for (int i = 0; i < 17; ++i) pad(7, 0xAB00 + i);
    CHECK_EQ(st().pcr2_fill, 34u);
    CHECK_EQ(st().pcr2_ready, 0u);
    CHECK_EQ(st().pcr2[0], 0xABu);
    CHECK_EQ(st().pcr2[1], 0u);
    CHECK_EQ(st().pcr2[33], 16u);
    pad(7, 0xCDEF);
    CHECK_EQ(st().pcr2_fill, 36u);
    CHECK_EQ(st().pcr2_ready, 1u);
    CHECK_EQ(st().cmd, 11u);
    CHECK_EQ(st().pcr2_mode, 0u);
    CHECK_EQ(st().pcr2[34], 0xCDu);
    CHECK_EQ(st().pcr2[35], 0xEFu);
    pad(7, 0x2222);                                // mode is idle again
    CHECK_EQ(st().pcr2_fill, 36u);
    pad(6, 2);                                     // restart clears the buffer
    CHECK_EQ(st().pcr2_fill, 0u);
    CHECK_EQ(st().pcr2[0], 0u);
    // phase 0 or unmounted state: commands ignored
    st().phase = 0;
    pad(1, 0);
    CHECK_EQ(st().cmd, 11u);
}

}  // namespace

int main() {
    test_pad_commands();
    test_routing_and_config();
    test_list_requests();
    test_art();
    test_disc_info();
    test_cheat_v6();
    test_cheat_v4_main();
    test_session();
    test_session_cancel_and_failures();
    test_hooks();
    TEST_MAIN_END();
}
