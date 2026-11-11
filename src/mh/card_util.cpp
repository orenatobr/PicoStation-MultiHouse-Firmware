// Leaf helpers and buffers of the V2 card layer (V3 FUN_100216d8 .. FUN_10021788, FUN_10021924,
// FUN_100218ac .. FUN_100218f4). Kept as small non-inlined leaves like the originals.
// The original card code is size optimised; keep the same shape on the device.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("Os")
#endif
#include "mh/card.h"

#include <cstring>

// The byte loops must stay loops (not memset/memcpy calls) like the originals.
#if defined(__GNUC__) && !defined(__clang__)
#define MH_NO_LIBCALL [[gnu::optimize("Os", "no-tree-loop-distribute-patterns")]]
#else
#define MH_NO_LIBCALL
#endif

namespace mh::card {

namespace {
CardState g_state;
CardGlobals g_globals;
alignas(4) uint8_t g_block[0xA00];
alignas(4) uint8_t g_resp[0x1000];
TextBuf g_text;
ResponseCache g_cache;
SdIo g_sd;
}  // namespace

void set_sd(const SdIo& io) { g_sd = io; }
const SdIo& sd() { return g_sd; }
CardState& state() { return g_state; }
CardGlobals& globals() { return g_globals; }
uint8_t* block_buffer() { return g_block; }
uint8_t* response() { return g_resp; }
TextBuf& text() { return g_text; }
ResponseCache& response_cache() { return g_cache; }

// V3 FUN_100216d8
MH_NO_LIBCALL [[gnu::noinline]] void zero_fill(void* dst, uint32_t n) {
    auto* p = static_cast<uint8_t*>(dst);
    auto* end = p + n;
    do {
        *p++ = 0;
    } while (p != end);
}

// V3 FUN_100216e6
MH_NO_LIBCALL [[gnu::noinline]] void copy(void* dst, const void* src, uint32_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    for (uint32_t i = 0; i != n; ++i) d[i] = s[i];
}

// V3 FUN_100216f8
[[gnu::noinline]] bool equal(const void* a, const void* b, uint32_t n) {
    auto* pa = static_cast<const uint8_t*>(a);
    auto* pb = static_cast<const uint8_t*>(b);
    uint32_t i = 0;
    do {
        if (pb[i] != pa[i]) return false;
        ++i;
    } while (i != n);
    return true;
}

// V3 FUN_10021714
[[gnu::noinline]] bool all_zero(const void* p, uint32_t n) {
    auto* b = static_cast<const uint8_t*>(p);
    auto* end = b + n;
    while (b != end) {
        if (*b++ != 0) return false;
    }
    return true;
}

// V3 FUN_1002172c (the original reads through a word pointer; here alignment safe)
[[gnu::noinline]] uint32_t read_u32(const void* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// V3 FUN_10021742
[[gnu::noinline]] void store_u32(void* p, uint32_t v) { std::memcpy(p, &v, 4); }

// V3 FUN_10021748: reflected CRC-32, polynomial 0xEDB88320, bitwise
[[gnu::noinline]] uint32_t crc32_update(uint32_t crc, const void* p, uint32_t n) {
    auto* b = static_cast<const uint8_t*>(p);
    auto* end = b + n;
    for (; b != end; ++b) {
        crc ^= *b;
        for (int i = 8; i != 0; --i) crc = (-(crc & 1) & 0xEDB88320u) ^ (crc >> 1);
    }
    return crc;
}

// V3 FUN_10021774
[[gnu::noinline]] uint32_t crc32(const void* p, uint32_t n) { return ~crc32_update(0xFFFFFFFFu, p, n); }

// V3 FUN_10022040: CRC-32 of the first `len` bytes of the block buffer with the four bytes at
// [zero_from, zero_from + 4) read as zero. The original derives the window end from zero_from
// (0xC -> 0x10, anything else -> 0x14), which is the same for its only two callers (0xC and 0x10).
uint32_t crc32_window(uint32_t len, uint32_t zero_from) {
    const uint32_t zero_to = zero_from == 0xC ? 0x10 : 0x14;
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i = 0;
    do {
        uint8_t b = 0;
        if (i < zero_from || i >= zero_to) b = g_block[i];
        crc = crc32_update(crc, &b, 1);
        ++i;
    } while (i != len);
    return ~crc;
}

// V3 FUN_10021788: busy wait between SD retries, (attempt * 0x1770 + 0x2EE0) iterations
void retry_delay(uint32_t attempt) {
    volatile int32_t n = static_cast<int32_t>(attempt * 0x1770 + 0x2EE0);
    int32_t cur;
    do {
        cur = n;
        n = cur - 1;
    } while (cur != 0);
}

// V3 FUN_10021924
void copy_printable(char* dst, uint32_t kind, const uint8_t* src, uint32_t src_limit) {
    const uint32_t last = kind == 0x10 ? 0xE : 0x1F;
    uint32_t i = 0;
    for (;;) {
        uint8_t b = src[i];
        if (b == 0) break;
        if (static_cast<uint32_t>(b) - 0x20 > 0x5E) b = 0x20;
        dst[i] = static_cast<char>(b);
        ++i;
        if (i - 1 == last || src_limit == i) break;
    }
    dst[i] = 0;
}

// V3 FUN_100218ac
bool text_append_char(TextBuf& t, char c) {
    const uint32_t len = t.len;
    if (len > 0x2FFE) return false;
    t.len = len + 1;
    t.data[len] = c;
    t.data[t.len] = 0;
    return true;
}

// V3 FUN_100218d4
bool text_append_str(TextBuf& t, const char* s) {
    while (*s) {
        if (!text_append_char(t, *s++)) return false;
    }
    return true;
}

// V3 FUN_100218f4
bool text_append_hex(TextBuf& t, uint32_t v, uint32_t nibbles) {
    static const char kHex[] = "0123456789ABCDEF";
    while (nibbles != 0) {
        --nibbles;
        const uint32_t shift = nibbles * 4;  // the original shifts by (n * 4) & 0xFF: zero digits past bit 31
        if (!text_append_char(t, kHex[shift < 32 ? (v >> shift) & 0xF : 0])) return false;
    }
    return true;
}

}  // namespace mh::card
