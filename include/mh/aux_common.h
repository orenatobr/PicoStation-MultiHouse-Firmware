// Small byte helpers shared by the aux_* modules (config/music, legacy loaders, fallback menu).
// They mirror the tiny leaf helpers of V3: FUN_10024412/FUN_10027012 (load32), FUN_10024400/FUN_10027000
// (copy), FUN_10027016 (compare), FUN_10027032 (zero test), FUN_10027098 (store32), FUN_1002441a and
// FUN_1002704a (CRC-32).
#pragma once

#include "mh/platform.h"

namespace mh::aux {

constexpr uint32_t kBlockBytes = 0xA00;  // 5 blocks, size of every card record read

inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

inline void put_le32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

inline void put_le16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

// FUN_10027032: true when all n bytes are zero.
inline bool all_zero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

// FUN_10027016: true when the first n bytes are equal.
inline bool bytes_equal(const uint8_t* a, const uint8_t* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// FUN_1002441a: reflected CRC-32 (poly 0xEDB88320, init and final xor 0xFFFFFFFF), bit by bit.
inline uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// FUN_1002704a: CRC-32 of n bytes where the bytes in [hole, hole + hole_len) are read as zero.
inline uint32_t crc32_hole(const uint8_t* p, size_t n, size_t hole, size_t hole_len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        uint32_t byte = (i < hole || i - hole >= hole_len) ? p[i] : 0u;
        crc ^= byte;
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// Block reader used by the loaders: read `blocks` 512 byte blocks starting at `lba` into `out`.
// Returns true on success (the firmware routine at RAM 0x20003415 returns 0 on success).
struct BlockReader {
    bool (*read)(void* ctx, uint32_t lba, uint32_t blocks, uint8_t* out);
    void* ctx;
    bool operator()(uint32_t lba, uint32_t blocks, uint8_t* out) const { return read(ctx, lba, blocks, out); }
};

}  // namespace mh::aux
