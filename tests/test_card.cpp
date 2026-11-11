// Host tests of the V2 card layer (src/mh/card*.cpp) against images built by tests/card_image.py.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "mh/card.h"

using namespace mh::card;

namespace {

std::vector<uint8_t> g_img;
int g_fail_reads = 0;      // next N reads fail
int g_always_fail = 0;
std::vector<std::pair<uint32_t, uint32_t>> g_calls;

int fake_init() { return 0; }
int fake_read(uint32_t lba, uint32_t blocks, void* dst) {
    g_calls.emplace_back(lba, blocks);
    if (g_always_fail) return 1;
    if (g_fail_reads > 0) {
        --g_fail_reads;
        return 1;
    }
    if ((static_cast<uint64_t>(lba) + blocks) * 512 > g_img.size()) return 1;
    std::memcpy(dst, g_img.data() + static_cast<size_t>(lba) * 512, static_cast<size_t>(blocks) * 512);
    return 0;
}

std::vector<uint8_t> make_image(const char* kind) {
    std::string out = std::string("card_test_") + kind + ".img";
    std::string cmd = "python3 -I card_image.py " + out + " " + kind;
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "image generator failed\n");
        std::exit(2);
    }
    FILE* f = std::fopen(out.c_str(), "rb");
    std::vector<uint8_t> v;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
    std::fclose(f);
    std::remove(out.c_str());
    return v;
}

void use(const std::vector<uint8_t>& img) {
    g_img = img;
    g_fail_reads = 0;
    g_always_fail = 0;
    g_calls.clear();
    set_sd({fake_init, fake_read});
}

uint8_t* at(uint32_t lba, uint32_t off = 0) { return g_img.data() + static_cast<size_t>(lba) * 512 + off; }
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
uint32_t get32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

void seal_header(uint32_t lba) { put32(at(lba, 0x92C), crc32(at(lba), 0x92C)); }
void seal_case(int i) { put32(at(10, i * 0x50 + 0x4C), crc32(at(10, i * 0x50), 0x4C)); }
void seal_disc(int i) { put32(at(0x32, i * 0x100 + 0xFC), crc32(at(0x32, i * 0x100), 0xFC)); }
void seal_map(uint32_t lba) {
    put32(at(lba, 0xC), 0);
    put32(at(lba, 0xC), crc32(at(lba), 0x930));
}

CardDisc g_disc;
bool mount_ok() { return mount(&g_disc); }

std::string listing_entry(const uint8_t* r, uint32_t* pos) {
    uint32_t n = r[*pos];
    std::string s(reinterpret_cast<const char*>(r + *pos + 2), n);
    *pos += 2 + n;
    return s;
}

// ---------------------------------------------------------------------------------------------
void test_helpers() {
    CHECK_EQ(crc32("123456789", 9), 0xCBF43926u);
    CHECK_EQ(crc32_update(0xFFFFFFFFu, "123456789", 9), ~0xCBF43926u);
    uint8_t z[4] = {0, 0, 0, 0}, nz[4] = {0, 0, 1, 0};
    CHECK(all_zero(z, 4));
    CHECK(!all_zero(nz, 4));
    CHECK(all_zero(nz, 0));
    CHECK(equal("abc", "abc", 3));
    CHECK(!equal("abc", "abd", 3));
    uint8_t w[7];
    zero_fill(w, 7);
    CHECK(all_zero(w, 7));
    store_u32(w + 1, 0x11223344);
    CHECK_EQ(read_u32(w + 1), 0x11223344u);

    char out[0x30];
    const uint8_t name[] = "Caf\xc3\xa9 ok\x7f\x1f~";
    copy_printable(out, 0x21, name, 0x20);
    CHECK_EQ(std::string(out), std::string("Caf   ok  ~"));  // 0x7F and 0x1F are blanks, 0x7E stays
    uint8_t full[0x20];
    std::memset(full, 'A', sizeof full);
    copy_printable(out, 0x21, full, 0x20);  // a full 0x20 byte field: 32 characters
    CHECK_EQ(std::strlen(out), 32u);
    uint8_t thirty_one[0x20];
    std::memset(thirty_one, 'B', 31);
    thirty_one[31] = 0;
    copy_printable(out, 0x21, thirty_one, 0x20);
    CHECK_EQ(std::strlen(out), 31u);
    copy_printable(out, 0x10, full, 0x10);  // serial: 15 characters
    CHECK_EQ(std::strlen(out), 15u);

    TextBuf& t = text();
    t.len = 0;
    CHECK(text_append_str(t, "AB"));
    CHECK(text_append_hex(t, 0x1AF, 4));
    CHECK_EQ(std::string(t.data), std::string("AB01AF"));
    CHECK_EQ(t.len, 6u);
    t.len = 0x2FFE;
    CHECK(text_append_char(t, 'x'));   // len 0x2ffe is still accepted
    CHECK(!text_append_char(t, 'y'));  // len 0x2fff is not
    CHECK_EQ(t.len, 0x2FFFu);
    CHECK_EQ(t.data[0x2FFE], 'x');
    CHECK_EQ(t.data[0x2FFF], 0);
    t.len = 0;
}

void test_block_io() {
    use(make_image("basic"));
    g_calls.clear();
    uint8_t buf[0x1400];
    CHECK(read_bytes(0xB4, 0, buf, 0x1400));  // 10 aligned blocks: 5 + 5
    CHECK_EQ(g_calls.size(), 2u);
    CHECK(g_calls[0] == std::make_pair(0xB4u, 5u));
    CHECK(g_calls[1] == std::make_pair(0xB9u, 5u));
    CHECK(std::memcmp(buf, at(0xB4), 0x1400) == 0);
    g_calls.clear();
    CHECK(read_bytes(0x32, 0x1C0, buf, 0x100));  // crosses a block boundary: two single block reads
    CHECK_EQ(g_calls.size(), 2u);
    CHECK(g_calls[0] == std::make_pair(0x32u, 1u));
    CHECK(g_calls[1] == std::make_pair(0x33u, 1u));
    CHECK(std::memcmp(buf, at(0x32, 0x1C0), 0x100) == 0);
    g_calls.clear();
    CHECK(read_bytes(0x32, 0x200, buf, 0x300));  // aligned but 1.5 blocks: one block read of 1 then the tail
    CHECK_EQ(g_calls[0].second, 1u);
    CHECK(std::memcmp(buf, at(0x33), 0x300) == 0);

    g_calls.clear();
    g_fail_reads = 2;  // two failures then success: 3 tries
    CHECK(read_blocks(10, 1));
    CHECK_EQ(g_calls.size(), 3u);
    g_calls.clear();
    g_fail_reads = 3;
    CHECK(!read_blocks(10, 1));
    CHECK_EQ(g_calls.size(), 3u);
    g_fail_reads = 0;
    CHECK(!read_blocks(10, 6));  // more than 5 blocks is refused

    // slot: 16 tries, padding must be zero
    g_calls.clear();
    g_always_fail = 1;
    CHECK(!read_slot(0, 0x10));
    CHECK_EQ(g_calls.size(), 48u);
    g_always_fail = 0;
    CHECK(read_slot(0, 0x10));
    *at(0, 0x930) = 1;
    CHECK(!read_slot(0, 2));
    *at(0, 0x9FF) = 1;
    CHECK(!read_slot(0, 2));
    *at(0, 0x930) = 0;
    *at(0, 0x9FF) = 0;
    CHECK(read_slot(0, 2));
}

void test_mount_basic() {
    use(make_image("basic"));
    CHECK(mount_ok());
    CardState& s = state();
    CHECK_EQ(s.phase, 1u);
    CHECK_EQ(s.magic, kStateMagic);
    CHECK_EQ(s.generation, 1u);
    CHECK_EQ(s.case_count, 5u);
    CHECK_EQ(s.disc_count, 6u);
    CHECK_EQ(s.case_lba, 10u);
    CHECK_EQ(s.disc_lba, 0x32u);
    CHECK_EQ(s.art_lba, 0xB4u);
    CHECK_EQ(s.menu_lba, 0x14B4u);
    CHECK_EQ(s.menu_sectors, 2u);
    CHECK_EQ(s.cfg_lba, 0x1685u);
    CHECK_EQ(s.art_size, 0x2800u);
    CHECK_EQ(s.valid, 1u);
    CHECK_EQ(s.cache_key, 0xFFFFFFFFu);
    CHECK_EQ(s.sel_case, 0xFFFFFFFFu);
    CHECK_EQ(s.crc_cases, 0u);
    // event 0 posted
    CHECK_EQ(s.event.magic, 0x31534C50u);
    CHECK_EQ(s.event.seq, 1u);
    CHECK_EQ(s.event.code, 0);
    CHECK_EQ(s.event.flags, 12);  // 8 from mount | 4 from post
    CHECK_EQ(s.event.crc, crc32(&s.event, 0x24));
    // default disc = the menu
    CHECK_EQ(g_disc.cue.track_count, 1u);
    CHECK_EQ(g_disc.cue.tracks[1].size, 0x385u);
    CHECK_EQ(g_disc.cue.tracks[1].indices[0], 0x122Au);
    CHECK_EQ(g_disc.cue.tracks[1].track_type, 2u);
    CHECK_EQ(g_disc.cue.tracks[2].file_offset, 0x385u);
    CHECK_EQ(g_disc.cue.tracks[2].indices[1], 0x15AFu);
    CHECK_EQ(g_disc.has_data, 1);
    CHECK_EQ(g_disc.sd_menu, 1);
    CHECK_EQ(globals().raw_base, 0x14B4u);
    CHECK_EQ(globals().total_sectors, 0x27D9u);
    CHECK(!mount(nullptr));
}

void test_header_ab_rule() {
    const uint32_t vals[] = {0, 1, 2, 5, 0x7FFFFFFE, 0x7FFFFFFF, 0x80000000u, 0x80000001u, 0x80000002u, 0xFFFFFFFEu, 0xFFFFFFFFu};
    const auto base = make_image("empty");  // small and fast: 1 menu sector
    for (uint32_t a : vals) {
        for (uint32_t b : vals) {
            use(base);
            put32(at(0, 0xC), a);
            put32(at(5, 0xC), b);
            put32(at(5, 0x3C), 3);  // marks header B (A keeps menu_sectors 1)
            seal_header(0);
            seal_header(5);
            CHECK(mount_ok());
            const bool take_b = static_cast<int32_t>(b - a) > 0;
            CHECK_EQ(state().generation, take_b ? b : a);
            CHECK_EQ(state().menu_sectors, take_b ? 3u : 1u);
        }
    }
    // one header damaged: the other is used whatever the generations
    use(base);
    put32(at(0, 0xC), 9);
    put32(at(5, 0xC), 4);
    seal_header(0);
    seal_header(5);
    *at(0, 0x20) ^= 1;  // A damaged
    CHECK(mount_ok());
    CHECK_EQ(state().generation, 4u);
    use(base);
    put32(at(0, 0xC), 4);
    put32(at(5, 0xC), 9);
    seal_header(0);
    seal_header(5);
    *at(5, 0x20) ^= 1;  // B damaged
    CHECK(mount_ok());
    CHECK_EQ(state().generation, 4u);
    // B unreadable (padding not zero) with a good A
    use(base);
    *at(5, 0x940) = 7;
    CHECK(mount_ok());
    CHECK_EQ(state().generation, 1u);
}

void test_header_damage() {
    const auto base = make_image("empty");
    // every byte of the CRC covered area and the CRC itself, in both slots: the card is refused
    for (uint32_t off = 0; off < 0x930; off += (off < 0x70 ? 1 : 0x11)) {
        use(base);
        *at(0, off) ^= 0x40;
        *at(5, off) ^= 0x40;
        CHECK(!mount_ok());
    }
    // padding of one slot damaged: that slot is refused but the other mounts; both: refused
    for (uint32_t off : {0x930u, 0x940u, 0x9FFu}) {
        use(base);
        *at(0, off) = 1;
        CHECK(mount_ok());
        *at(5, off) = 1;
        CHECK(!mount_ok());
    }
    // logical rules with a valid CRC
    struct Mut { uint32_t off; uint32_t val; unsigned size; };
    const Mut muts[] = {
        {6, 1 << 16 >> 16, 1},   // placeholder replaced below (version 1 goes to the legacy branch)
    };
    (void)muts;
    auto refused = [&](uint32_t off, uint32_t val, unsigned size) {
        use(base);
        for (uint32_t lba : {0u, 5u}) {
            std::memcpy(at(lba, off), &val, size);
            seal_header(lba);
        }
        return !mount_ok();
    };
    CHECK(refused(6, 3, 1));          // version
    CHECK(refused(7, 0, 1));          // flags bit 0 clear
    CHECK(refused(8, 0x101, 2));      // case_count > 0x100
    CHECK(refused(0xA, 0x101, 2));    // disc_count > 0x100
    CHECK(refused(8, 1, 2));          // case_count 1 with disc_count 0
    CHECK(refused(0x10, 0x400, 4));   // unit
    CHECK(refused(0x14, 6, 4));       // b_lba
    CHECK(refused(0x18, 1, 4));       // b_lba high
    CHECK(refused(0x1C, 11, 4));      // case_lba
    CHECK(refused(0x24, 0x33, 4));    // disc_lba
    CHECK(refused(0x2C, 0xB5, 4));    // art_lba
    CHECK(refused(0x34, 0x14B5, 4));  // menu_lba
    CHECK(refused(0x3C, 0, 4));       // menu_sectors 0
    CHECK(refused(0x3C, 94, 4));      // menu_sectors 94
    CHECK(refused(0x40, 0x1686, 4));  // cfg_lba
    CHECK(refused(0x48, 0x2801, 4));  // art_size
    CHECK(refused(0x4C, 1, 4));       // art_size high
    // values that only the CRC covers but the firmware does not interpret stay accepted
    use(base);
    for (uint32_t lba : {0u, 5u}) {
        put32(at(lba, 0x50), 0x12345678);  // crc_cases
        put32(at(lba, 0x58), 0x9ABCDEF0);  // crc_art
        *at(lba, 0x60) = 0xAA;             // sha1
        *at(lba, 7) = 3;                   // flags bit 1
        *at(lba, 0x1BE) = 0xDA;
        seal_header(lba);
    }
    CHECK(mount_ok());
    CHECK_EQ(state().crc_cases, 0x12345678u);
    // menu_sectors 1 and 93 are the accepted limits
    for (uint32_t v : {1u, 93u}) {
        use(base);
        for (uint32_t lba : {0u, 5u}) {
            put32(at(lba, 0x3C), v);
            seal_header(lba);
        }
        CHECK(mount_ok());
        CHECK_EQ(state().menu_sectors, v);
    }
}

void test_cases() {
    const auto base = make_image("basic");
    use(base);
    CHECK(mount_ok());
    CaseInfo ci;
    CHECK(case_by_index(0, &ci));
    CHECK_EQ(ci.index, 0);
    CHECK_EQ(ci.disc_count, 1);
    CHECK_EQ(ci.art_id, 0);
    CHECK_EQ(ci.ids[0], 0);
    CHECK_EQ(std::string(ci.name), std::string("Alundra 2"));
    CHECK(case_by_index(1, &ci));
    CHECK_EQ(ci.disc_count, 2);
    CHECK_EQ(ci.ids[0], 1);
    CHECK_EQ(ci.ids[1], 2);
    CHECK(case_by_index(3, &ci));
    CHECK_EQ(std::string(ci.name), std::string(31, 'A') + "B");  // 32 characters survive
    CHECK(case_by_index(4, &ci));
    CHECK_EQ(std::string(ci.name), std::string("Caf     ! ~"));  // non ASCII bytes become blanks
    CHECK(!case_by_index(5, &ci));  // empty record
    CHECK(nth_used_case(0, &ci));
    CHECK(nth_used_case(4, &ci));
    CHECK_EQ(ci.index, 4);
    CHECK(!nth_used_case(5, &ci));  // past case_count

    // one damaged byte in every region of a Case record: refused (the CRC covers 0..0x4B)
    const uint32_t offs[] = {0, 3, 4, 5, 6, 7, 8, 0x27, 0x28, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3F, 0x45, 0x4B, 0x4C, 0x4F};
    for (uint32_t off : offs) {
        use(base);
        CHECK(mount_ok());
        *at(10, 0x50 + off) ^= 0x01;  // Case 1
        CHECK(!case_by_index(1, &ci));
    }
    // logical rules with a valid CRC
    auto bad = [&](uint32_t off, uint8_t v) {
        use(base);
        CHECK(mount_ok());
        *at(10, 0x50 + off) = v;
        seal_case(1);
        CaseInfo c;
        return !case_by_index(1, &c);
    };
    CHECK(bad(5, 0));       // disc_count 0
    CHECK(bad(5, 6));       // disc_count 6
    CHECK(bad(6, 7));       // index != position
    CHECK(bad(0x3B, 1));    // second disc id equals the first (1 == 1)
    CHECK(bad(0x3C, 9));    // id beyond disc_count must be 0
    CHECK(bad(0x3F, 1));    // padding
    CHECK(bad(0x4B, 1));
    // art id 0xFFFF and 0xFF accepted, 0x100 refused
    {
        use(base);
        CHECK(mount_ok());
        at(10, 0x50 + 0x38)[0] = 0xFF;
        at(10, 0x50 + 0x38)[1] = 0xFF;
        seal_case(1);
        CHECK(case_by_index(1, &ci));
        CHECK_EQ(ci.art_id, 0xFFFF);
        at(10, 0x50 + 0x38)[0] = 0x00;
        at(10, 0x50 + 0x38)[1] = 0x01;
        seal_case(1);
        CHECK(!case_by_index(1, &ci));
        at(10, 0x50 + 0x38)[0] = 0xFF;
        at(10, 0x50 + 0x38)[1] = 0x00;
        seal_case(1);
        CHECK(case_by_index(1, &ci));
    }
    // an unused record must be entirely zero
    uint8_t rec[0x50] = {0};
    CHECK_EQ(validate_case(rec, 7, nullptr), 0);
    for (uint32_t off = 0; off < 0x50; ++off) {
        if (off == 4) continue;
        rec[off] = 1;
        CHECK_EQ(validate_case(rec, 7, nullptr), -1);
        rec[off] = 0;
    }
    // listing scan: an empty record inside the table is skipped, a dirty one stops nth_used_case
    use(base);
    CHECK(mount_ok());
    std::memset(at(10, 0x50), 0, 0x50);  // Case 1 removed
    CHECK(nth_used_case(1, &ci));
    CHECK_EQ(ci.index, 2);
    *at(10, 0x50 + 0x20) = 1;  // dirty "empty" record
    CHECK(!nth_used_case(1, &ci));
    CHECK(nth_used_case(0, &ci));
}

void test_discs() {
    const auto base = make_image("basic");
    use(base);
    CHECK(mount_ok());
    DiscInfo di;
    CHECK(disc_by_id(1, &di));
    CHECK_EQ(di.id, 1);
    CHECK_EQ(di.case_index, 1);
    CHECK_EQ(di.slot, 0);
    CHECK_EQ(di.track_count, 2);
    CHECK_EQ(di.base_lba, 0x2900u);
    CHECK_EQ(di.track_lba, 0x2900u);
    CHECK_EQ(di.raw_lba, 0x2A00u);
    CHECK_EQ(di.raw_blocks, 25u);
    CHECK_EQ(di.sectors, 5u);
    CHECK_EQ(di.cheat_lba, 0u);
    CHECK_EQ(di.data_crc32, 0xCAFE0001u);
    CHECK_EQ(std::string(di.title), std::string("Chrono Cross (Disc 1)"));
    CHECK_EQ(std::string(di.serial), std::string("SLUS-01041"));
    CHECK_EQ(di.record_crc, get32(at(0x32, 0x100 + 0xFC)));
    CHECK(disc_by_id(4, &di));
    CHECK_EQ(std::string(di.serial), std::string(15, 'X'));  // 16th byte cut
    CHECK_EQ(std::string(di.title), std::string(32, 'T'));
    CHECK(disc_by_id(5, &di));
    CHECK_EQ(std::string(di.title), std::string(9, ' ') + " ok");  // 9 non ASCII bytes
    CHECK(!disc_by_id(6, &di));  // empty record

    const uint32_t offs[] = {0, 3, 4, 5, 6, 7, 8, 0xC, 0x1B, 0x1C, 0x3B, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C,
                             0x60, 0x61, 0x62, 0x64, 0x68, 0x6C, 0x70, 0x74, 0x78, 0x7C, 0x80, 0x93, 0x94, 0xA0, 0xFB, 0xFC, 0xFF};
    for (uint32_t off : offs) {
        use(base);
        CHECK(mount_ok());
        *at(0x32, 0x100 + off) ^= 0x01;
        CHECK(!disc_by_id(1, &di));
    }
    // logical rules with a valid CRC
    auto bad = [&](uint32_t off, uint32_t v, unsigned size = 4) {
        use(base);
        CHECK(mount_ok());
        std::memcpy(at(0x32, 0x100 + off), &v, size);
        seal_disc(1);
        DiscInfo d;
        return !disc_by_id(1, &d);
    };
    auto good = [&](uint32_t off, uint32_t v, unsigned size = 4) {
        use(base);
        CHECK(mount_ok());
        std::memcpy(at(0x32, 0x100 + off), &v, size);
        seal_disc(1);
        DiscInfo d;
        return disc_by_id(1, &d);
    };
    CHECK(bad(5, 2, 1));            // id != position
    CHECK(bad(8, 4));               // flags >= 4
    CHECK(good(8, 2));              // EXT flag alone
    CHECK(bad(0x3C, 0x27FF));       // base below art_size
    CHECK(bad(0x3C, 0x2901));       // base not a multiple of 256
    CHECK(bad(0x40, 1));            // base high dword
    CHECK(bad(0x44, 0x2A00));       // track_lba != base
    CHECK(bad(0x48, 1));
    CHECK(bad(0x4C, 0x2901));       // raw_lba != base + 0x100
    CHECK(bad(0x50, 1));
    CHECK(bad(0x54, 24));           // raw_blocks != sectors * 5
    CHECK(bad(0x58, 1));
    CHECK(bad(0x5C, 0));            // sectors 0
    CHECK(bad(0x60, 0, 1));         // track_count 0
    CHECK(bad(0x60, 100, 1));       // track_count 100
    CHECK(good(0x60, 99, 1));
    CHECK(bad(0x62, 0x100, 2));     // art id above 0xFF
    CHECK(good(0x62, 0xFFFF, 2));
    CHECK(bad(0x64, 0x2906));       // cheat_lba not base + 5
    CHECK(bad(0x6C, 4));            // cheat_size without cheat_lba
    CHECK(bad(0x68, 1));
    CHECK(bad(0x78, 4));            // extra_size without extra_lba
    CHECK(bad(0x74, 1));
    CHECK(bad(0x94, 1, 1));         // padding after the hashes
    CHECK(bad(0xFB, 1, 1));
    {  // cheat: 0x1400 max without EXT, 0x2800 with
        use(base);
        CHECK(mount_ok());
        put32(at(0x32, 0x100 + 0x64), 0x2905);
        put32(at(0x32, 0x100 + 0x6C), 0x1400);
        seal_disc(1);
        CHECK(disc_by_id(1, &di));
        CHECK_EQ(di.cheat_lba, 0x2905u);
        put32(at(0x32, 0x100 + 0x6C), 0x1401);
        seal_disc(1);
        CHECK(!disc_by_id(1, &di));
        put32(at(0x32, 0x100 + 8), 2);
        put32(at(0x32, 0x100 + 0x6C), 0x2800);
        seal_disc(1);
        CHECK(disc_by_id(1, &di));
        put32(at(0x32, 0x100 + 0x6C), 0x2801);
        seal_disc(1);
        CHECK(!disc_by_id(1, &di));
        // extra slot: base + 0xF, or base + 0x19 with EXT
        put32(at(0x32, 0x100 + 0x6C), 0);
        put32(at(0x32, 0x100 + 0x64), 0);
        put32(at(0x32, 0x100 + 0x70), 0x2919);
        put32(at(0x32, 0x100 + 0x78), 0xA00);
        seal_disc(1);
        CHECK(disc_by_id(1, &di));
        put32(at(0x32, 0x100 + 0x78), 0xA01);
        seal_disc(1);
        CHECK(!disc_by_id(1, &di));
        put32(at(0x32, 0x100 + 0x78), 0xA00);
        put32(at(0x32, 0x100 + 8), 0);
        seal_disc(1);
        CHECK(!disc_by_id(1, &di));  // 0x2919 is wrong without EXT
        put32(at(0x32, 0x100 + 0x70), 0x290F);
        seal_disc(1);
        CHECK(disc_by_id(1, &di));
    }
    // an unused record must be entirely zero
    uint8_t rec[0x100] = {0};
    CHECK_EQ(validate_disc(rec, 3, 0x2800, nullptr), 0);
    for (uint32_t off = 0; off < 0x100; ++off) {
        if (off == 4) continue;
        rec[off] = 1;
        CHECK_EQ(validate_disc(rec, 3, 0x2800, nullptr), -1);
        rec[off] = 0;
    }
    // disc_of_case follows the selected Case and checks the back references
    use(base);
    CHECK(mount_ok());
    CHECK(!disc_of_case(0, &di));  // no Case selected
    state().sel_case = 1;
    CHECK(disc_of_case(0, &di));
    CHECK_EQ(di.id, 1);
    CHECK(disc_of_case(1, &di));
    CHECK_EQ(di.id, 2);
    CHECK(!disc_of_case(2, &di));
    *at(0x32, 0x100 * 2 + 7) = 0;  // slot 0 on both discs: Disc 2 no longer matches slot 1 (CRC also breaks)
    seal_disc(2);
    CHECK(!disc_of_case(1, &di));
    state().sel_case = 0x100;
    CHECK(!disc_of_case(0, &di));
}

void test_track_map() {
    const auto base = make_image("basic");
    use(base);
    CHECK(mount_ok());
    DiscInfo di;
    CHECK(disc_by_id(1, &di));  // two tracks: data 0..3, audio 3..5
    CardDisc out;
    std::memset(&out, 0xEE, sizeof out);
    CHECK(load_track_map(di, &out));
    CHECK_EQ(out.cue.track_count, 2u);
    CHECK_EQ(out.cue.tracks[0].indices[1], 0x122Au);
    CHECK_EQ(out.cue.tracks[1].size, 3u);
    CHECK_EQ(out.cue.tracks[1].file_offset, 0u);
    CHECK_EQ(out.cue.tracks[1].index_count, 1u);
    CHECK_EQ(out.cue.tracks[1].indices[0], 0x122Au);
    CHECK_EQ(out.cue.tracks[1].track_type, 2u);
    CHECK_EQ(out.cue.tracks[2].size, 2u);
    CHECK_EQ(out.cue.tracks[2].file_offset, 3u);
    CHECK_EQ(out.cue.tracks[2].indices[0], 3u + 0x122A);
    CHECK_EQ(out.cue.tracks[2].track_type, 1u);  // audio
    CHECK_EQ(out.cue.tracks[3].file_offset, 5u);
    CHECK_EQ(out.cue.tracks[3].indices[0], 5u + 0x122A);
    CHECK_EQ(out.cue.tracks[3].indices[1], 5u + 0x122A);
    CHECK_EQ(out.has_data, 1);
    CHECK_EQ(out.current_track, 0);
    CHECK_EQ(out.sd_menu, 1);
    CHECK(load_track_map(di, nullptr));

    // a failed validation leaves the table untouched
    const uint32_t map_lba = di.track_lba;
    auto fails = [&](uint32_t off, uint32_t xor_val, bool reseal) {
        use(base);
        CHECK(mount_ok());
        DiscInfo d;
        CHECK(disc_by_id(1, &d));
        *at(map_lba, off) ^= static_cast<uint8_t>(xor_val);
        if (reseal) seal_map(map_lba);
        CardDisc t;
        std::memset(&t, 0xEE, sizeof t);
        bool r = load_track_map(d, &t);
        CHECK_EQ(static_cast<uint8_t>(t.cue.track_count & 0xFF), 0xEE);
        return !r;
    };
    // one damaged byte in every region (CRC covers 0x930 bytes)
    for (uint32_t off : {0u, 7u, 8u, 9u, 0xAu, 0xCu, 0xFu, 0x10u, 0x14u, 0x18u, 0x1Cu, 0x1Du, 0x1Fu, 0x20u, 0x2Cu, 0x4Fu, 0x6FFu % 0x930u, 0x929u})
        CHECK(fails(off, 1, false));
    // logical rules with a valid CRC
    CHECK(fails(8, 1, true));     // count byte != track_count
    CHECK(fails(9, 1, true));     // entry size
    CHECK(fails(0xA, 2, true));   // flags >= 2
    CHECK(fails(0x10, 1, true));  // entry 0 does not start at 0
    CHECK(fails(0x1C, 1, true));  // type 3
    CHECK(fails(0x1D, 0x10, true));  // kind > 0xF
    CHECK(fails(0x1F, 1, true));  // reserved byte
    CHECK(fails(0x20, 1, true));  // entry 1 start != running end
    CHECK(fails(0x28, 1, true));  // entry 1 length changes: end != sectors
    CHECK(fails(0x2C + 0x30, 1, true));  // entries after count must be zero (entry 5)
    // the other two accepted CRC forms
    {
        use(base);
        CHECK(mount_ok());
        DiscInfo d;
        CHECK(disc_by_id(1, &d));
        put32(at(map_lba, 0xC), 0);
        put32(at(map_lba, 0xC), crc32(at(map_lba), 0x640));
        CHECK(load_track_map(d, nullptr));
        put32(at(map_lba, 0xC), crc32(at(map_lba, 0x10), 0x630));
        CHECK(load_track_map(d, nullptr));
        put32(at(map_lba, 0xC), 0x1234);
        CHECK(!load_track_map(d, nullptr));
    }
    // entry rules: index1 inside the track, first track data, at least one data track, sizes
    auto set_entry = [&](int e, uint32_t start, uint32_t idx1, uint32_t len, uint8_t type) {
        uint8_t* p = at(map_lba, 0x10 + 16 * e);
        put32(p, start);
        put32(p + 4, idx1);
        put32(p + 8, len);
        p[0xC] = type;
    };
    struct Case { uint32_t s0, i0, l0; uint8_t t0; uint32_t s1, i1, l1; uint8_t t1; bool ok; };
    const Case cases[] = {
        {0, 0, 3, 2, 3, 3, 2, 0, true},    // data + audio
        {0, 2, 3, 1, 3, 3, 2, 0, true},    // MODE1 type 1 and index1 inside
        {0, 3, 3, 2, 3, 3, 2, 0, false},   // index1 == end of track 0
        {0, 0, 3, 0, 3, 3, 2, 2, false},   // first track audio
        {0, 0, 3, 2, 3, 2, 2, 0, false},   // index1 before the track start
        {0, 0, 3, 2, 3, 5, 2, 0, false},   // index1 == end of track 1
        {0, 0, 3, 2, 3, 3, 3, 0, false},   // last end != sectors
        {0, 0, 3, 2, 4, 4, 1, 0, false},   // gap
        {0, 0, 0, 2, 0, 0, 5, 0, false},   // zero length
        {0, 0, 3, 2, 3, 3, 2, 2, true},    // two data tracks
    };
    for (const Case& c : cases) {
        use(base);
        CHECK(mount_ok());
        DiscInfo d;
        CHECK(disc_by_id(1, &d));
        set_entry(0, c.s0, c.i0, c.l0, c.t0);
        set_entry(1, c.s1, c.i1, c.l1, c.t1);
        seal_map(map_lba);
        CHECK_EQ(load_track_map(d, nullptr), c.ok);
    }
    // a disc of audio only after a data first track is fine; all audio is refused (entry 0 must be data)
    {
        use(base);
        CHECK(mount_ok());
        DiscInfo d;
        CHECK(disc_by_id(3, &d));  // one track (0,0,2) data
        const uint32_t lba = d.track_lba;
        *at(lba, 0x1C) = 0;
        put32(at(lba, 0xC), 0);
        put32(at(lba, 0xC), crc32(at(lba), 0x930));
        CHECK(!load_track_map(d, nullptr));
    }
}

void test_listing() {
    use(make_image("basic"));
    CHECK(mount_ok());
    CardState& s = state();
    auto tail = [&](uint32_t pos, uint32_t err, uint32_t count) {
        const uint8_t* r = response();
        CHECK_EQ(r[pos], 0);
        CHECK_EQ(r[pos + 1], err);
        CHECK_EQ(r[pos + 2], count >> 8);
        CHECK_EQ(r[pos + 3], count & 0xFF);
        CHECK(all_zero(r + pos + 4, kRespSize - pos - 4));
    };
    // whole library
    library_listing();
    {
        uint32_t pos = 0;
        const uint8_t* r = response();
        CHECK_EQ(listing_entry(r, &pos), std::string("Alundra 2"));
        CHECK_EQ(r[1], 1);
        CHECK_EQ(listing_entry(r, &pos), std::string("Chrono Cross"));
        CHECK_EQ(listing_entry(r, &pos), std::string("CASE PS1"));
        CHECK_EQ(listing_entry(r, &pos), std::string(31, 'A') + "B");
        CHECK_EQ(listing_entry(r, &pos), std::string("Caf     ! ~"));
        tail(pos, 0, 5);
    }
    // paging: skip the first Case
    s.list_offset = 1;
    library_listing();
    {
        uint32_t pos = 0;
        CHECK_EQ(listing_entry(response(), &pos), std::string("Chrono Cross"));
        listing_entry(response(), &pos);
        listing_entry(response(), &pos);
        listing_entry(response(), &pos);
        tail(pos, 0, 4);
    }
    s.list_offset = 0;
    // one Case: its discs
    s.sel_case = 1;
    library_listing();
    {
        uint32_t pos = 0;
        CHECK_EQ(listing_entry(response(), &pos), std::string("Chrono Cross (Disc 1) - DISCO 1"));
        CHECK_EQ(listing_entry(response(), &pos), std::string("Chrono Cross (Disc 2) - DISCO 2"));
        tail(pos, 0, 2);
    }
    s.list_offset = 1;
    library_listing();
    {
        uint32_t pos = 0;
        CHECK_EQ(listing_entry(response(), &pos), std::string("Chrono Cross (Disc 2) - DISCO 2"));
        tail(pos, 0, 1);
    }
    s.list_offset = 0;
    s.sel_case = 0;  // a single disc Case has no suffix
    library_listing();
    {
        uint32_t pos = 0;
        CHECK_EQ(listing_entry(response(), &pos), std::string("Alundra 2"));
        tail(pos, 0, 1);
    }
    s.sel_case = 2;  // empty title
    library_listing();
    {
        uint32_t pos = 0;
        CHECK_EQ(listing_entry(response(), &pos), std::string("JOGO PS1"));
        tail(pos, 0, 1);
    }
    s.sel_case = 4;  // non ASCII title bytes
    library_listing();
    {
        uint32_t pos = 0;
        CHECK_EQ(listing_entry(response(), &pos), std::string(9, ' ') + " ok");
    }
    s.sel_case = 9;  // not a Case
    library_listing();
    {
        uint32_t pos = 0;
        CHECK_EQ(listing_entry(response(), &pos), std::string("CASE INVALIDA"));
        tail(pos, 0, 1);
    }
    s.valid = 0;
    library_listing();
    {
        uint32_t pos = 0;
        CHECK_EQ(listing_entry(response(), &pos), std::string("ERRO RAW INVALIDO"));
        tail(pos, 0, 1);
    }
    // a damaged Case is left out of the listing, the others stay
    s.valid = 1;
    s.sel_case = 0xFFFFFFFFu;
    *at(10, 0x50 + 0x39) ^= 1;  // Case 1: CRC breaks
    library_listing();
    {
        uint32_t pos = 0;
        CHECK_EQ(listing_entry(response(), &pos), std::string("Alundra 2"));
        CHECK_EQ(listing_entry(response(), &pos), std::string("CASE PS1"));
    }

    // append limits
    zero_fill(response(), kRespSize);
    uint32_t pos = 0;
    CHECK(!listing_append(&pos, "", 1));
    CHECK(listing_append(&pos, "abc", 1));
    CHECK_EQ(pos, 5u);
    pos = kRespSize - 6 - 3;
    CHECK(listing_append(&pos, "abc", 1));   // 0x914 - 6 - 3 + 6 + 3 == 0x914
    pos = kRespSize - 6 - 3 + 1;
    CHECK(!listing_append(&pos, "abc", 1));
    const std::string longname(0x70, 'q');
    pos = 0;
    CHECK(listing_append(&pos, longname.c_str(), 1));  // no NUL in 0x60 bytes: cut at 0x60
    CHECK_EQ(response()[0], 0x60);

    // too many Cases for one sector: error flag and the count of the table
    use(make_image("many"));
    CHECK(mount_ok());
    library_listing();
    {
        uint32_t p = 0, n = 0;
        while (response()[p] != 0) {
            listing_entry(response(), &p);
            ++n;
        }
        CHECK_EQ(n, 70u);
        CHECK_EQ(response()[p + 1], 1);
        CHECK_EQ((response()[p + 2] << 8) | response()[p + 3], 80);
    }
}

void test_legacy() {
    const auto base = make_image("legacy");
    use(base);
    CardDisc d;
    std::memset(&d, 0, sizeof d);
    CHECK(mount(&d));
    CHECK_EQ(state().phase, 0u);
    CHECK_EQ(state().magic, kStateMagic);
    CHECK_EQ(d.cue.track_count, 1u);
    CHECK_EQ(d.cue.tracks[1].size, 4u);
    CHECK_EQ(d.cue.tracks[1].track_type, 2u);
    CHECK_EQ(d.cue.tracks[2].file_offset, 4u);
    CHECK_EQ(globals().raw_base, 0x45u);
    CHECK_EQ(globals().total_sectors, 4u + 2 * 0x122A);
    for (uint32_t off : {0u, 5u, 6u, 7u, 8u, 9u, 0xAu, 0x10u, 0x14u, 0x130u, 0x134u, 0x135u, 0x136u, 0x137u, 0x16Cu, 0x170u, 0x174u,
                         0x17Cu, 0x18Cu, 0x190u, 0x22Cu, 0x22Fu, 0x400u}) {
        use(base);
        *at(0, off) ^= 1;
        CHECK(!mount(&d));
    }
}

void test_stub_and_events() {
    use(make_image("empty"));
    CHECK(mount_ok());
    CardState& s = state();
    CHECK(sector_route(0x96) == SectorRoute::kCard);
    CHECK(sector_route(0xF2) == SectorRoute::kCard);
    CHECK(sector_route(0xF3) == SectorRoute::kBuiltinThenCard);
    s.menu_sectors = 0;
    CHECK(sector_route(0x96) == SectorRoute::kBuiltin);
    s.menu_sectors = 1;
    s.phase = kPhaseGame;
    CHECK(sector_route(0x41A) == SectorRoute::kBuiltinThenCard);
    CHECK(sector_route(0x500) == SectorRoute::kCard);
    s.phase = kPhaseSwitch;
    CHECK(sector_route(0x41A) == SectorRoute::kBuiltinThenCard);
    CHECK_EQ(s.phase, 4u);
    response_cache().slot[3] = 7;
    CHECK(sector_route(0x400) == SectorRoute::kCard);
    CHECK_EQ(s.phase, 3u);
    CHECK_EQ(response_cache().slot[3], 0xFFFFFFFEu);
    CHECK_EQ(response_cache().last, 0xFFFFFFFFu);
    s.phase = kPhaseNone;
    CHECK(sector_route(0x41A) == SectorRoute::kCard);
    CHECK_EQ(sector_block(0x14B4, 0x96), 0x14B4u);
    CHECK_EQ(sector_block(0x14B4, 0x97), 0x14B9u);
    CHECK_EQ(sector_block(100, 0x96 + 10), 150u);

    // events: sequence, wrap, flags, CRC, cache reset
    s.event.seq = 0xFFFFFFFFu;
    s.cache_key = 5;
    post_event(3, 4, 0x55);
    CHECK_EQ(s.event.seq, 1u);
    CHECK_EQ(s.event.code, 3);
    CHECK_EQ(s.event.arg, 4);
    CHECK_EQ(s.event.extra, 0x55u);
    CHECK_EQ(s.event.crc, crc32(&s.event, 0x24));
    CHECK_EQ(s.cache_key, 0xFFFFFFFFu);
    post_event_ack(10, 77);
    CHECK_EQ(s.event.seq, 2u);
    CHECK_EQ(s.event.code, 8);
    CHECK_EQ(s.event.arg, 10);
    CHECK_EQ(s.event.extra, 77u);

    // disc_info_request failure path
    DiscInfo di;
    CHECK(!disc_info_request(0x0102, &di));
    CHECK_EQ(get32(response()), 0x314D4750u);
    CHECK_EQ(response()[4], 1);
    CHECK_EQ(response()[5], 3);
    CHECK_EQ(response()[6], 2);
    CHECK_EQ(response()[7], 1);
    CHECK_EQ(get32(response() + 0x100), crc32(response(), 0x100));
    CHECK_EQ(s.event.code, 8);
    CHECK_EQ(s.event.arg, 10);
    CHECK_EQ(s.event.extra, 0x0102u);
}

void test_sd_failures() {
    use(make_image("empty"));
    g_always_fail = 1;
    CHECK(!mount_ok());
    g_always_fail = 0;
    set_sd({nullptr, nullptr});
    CHECK(!mount_ok());
    set_sd({fake_init, nullptr});
    CHECK(!mount_ok());
    use(make_image("empty"));
    CHECK(mount_ok());
}

}  // namespace

int main() {
    test_helpers();
    test_block_io();
    test_mount_basic();
    test_header_ab_rule();
    test_header_damage();
    test_cases();
    test_discs();
    test_track_map();
    test_listing();
    test_legacy();
    test_stub_and_events();
    test_sd_failures();
    TEST_MAIN_END();
}
