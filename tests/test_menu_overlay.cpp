// Host test of the menu overlay (src/mh/menu_overlay.cpp). With MH_MENU_ORIG and MH_MENU_PATCHED set to the
// Manager menu blobs before and after tools/patch_menu.py, it also checks the generated table sector by sector.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "check.h"
#include "mh/menu_overlay.h"

using namespace mh::menu_overlay;
using Bytes = std::vector<uint8_t>;

static Bytes load(const char* path) {
    Bytes b;
    FILE* f = std::fopen(path, "rb");
    if (!f) return b;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    return b;
}

static void test_synthetic() {
    Bytes orig(kSectorBytes), patched(kSectorBytes, 0x5A);
    for (uint32_t i = 0; i < kSectorBytes; ++i) orig[i] = static_cast<uint8_t>(i * 7 + 1);
    const Entry e{9, fnv1a(orig.data(), kSectorBytes), patched.data()};

    Bytes s = orig;
    CHECK(apply(9, s.data(), &e, 1));
    CHECK(s == patched);

    s = orig;
    CHECK(!apply(8, s.data(), &e, 1));  // other sector index: untouched
    CHECK(s == orig);

    s = orig;
    s[100] ^= 1;
    const Bytes changed = s;
    CHECK(!apply(9, s.data(), &e, 1));  // not the original bytes (already patched, other menu): untouched
    CHECK(s == changed);
}

// Fixture: FNV-1a of the patched sectors the Manager 0.5.5 menu patch produces. The Manager pins the same values in
// core/tests/test_menu.c, so the two code paths cannot drift apart without a CI failure on one side.
static const struct { uint32_t sector; uint32_t patched_hash; } kFixture[] = {
    {33, 0x6c896883u},
    {34, 0x8bf3870bu},
    {35, 0x4ccc5389u},
    {42, 0x366974d3u},
    {54, 0x769e36ddu},
};

static void test_fixture() {
    CHECK_EQ(kCount, sizeof kFixture / sizeof kFixture[0]);
    for (uint32_t i = 0; i < kCount; ++i) {
        CHECK_EQ(kEntries[i].sector, kFixture[i].sector);
        CHECK_EQ(fnv1a(kEntries[i].patched, kSectorBytes), kFixture[i].patched_hash);
    }
}

static void test_generated() {
    CHECK_EQ(kCount, 5u);
    Bytes junk(kSectorBytes, 0x11);
    for (uint32_t i = 0; i < kCount; ++i) {
        Bytes s = junk;
        CHECK(!apply(kEntries[i].sector, s.data()));
        CHECK(s == junk);
    }
    const char* o = std::getenv("MH_MENU_ORIG");
    const char* p = std::getenv("MH_MENU_PATCHED");
    if (!o || !p) return;
    const Bytes a = load(o), b = load(p);
    CHECK_EQ(a.size(), 93u * kSectorBytes);
    CHECK_EQ(b.size(), a.size());
    for (uint32_t k = 0; k < 93; ++k) {
        Bytes s(a.begin() + k * kSectorBytes, a.begin() + (k + 1) * kSectorBytes);
        const Bytes want(b.begin() + k * kSectorBytes, b.begin() + (k + 1) * kSectorBytes);
        const bool hit = apply(k, s.data());
        CHECK_EQ(hit, s == want && s != Bytes(a.begin() + k * kSectorBytes, a.begin() + (k + 1) * kSectorBytes));
        CHECK(s == want);
        Bytes again = want;  // a menu already patched by the Manager passes through
        CHECK(!apply(k, again.data()) || again == want);
    }
}

int main() {
    test_synthetic();
    test_fixture();
    test_generated();
    TEST_MAIN_END();
}
