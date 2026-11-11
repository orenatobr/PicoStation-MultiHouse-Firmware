// Built-in fallback menu (shown when neither a V2 nor a legacy card layout is valid).
// V3 FUN_10030000 (mount chain + disc table), FUN_100300b8 (sector provider).
#pragma once

#include "mh/aux_legacy.h"

namespace mh::aux {

constexpr uint32_t kFallbackSectors = 0x5D;       // length of the single data track (93)
constexpr uint32_t kFallbackImageSectors = 0x1D;  // sectors present in the embedded image (29)
constexpr uint32_t kSectorBytes = 0x930;
constexpr uint32_t kFirstSectorLba = 0x96;        // request LBA of sector 0

// Disc table of the fallback menu (one data track of 0x5D sectors, raw base 0).
void fallback_table(DiscTable& t);

// Values FUN_10030000 stores in the card state block: state = 1, raw base (+0x24) = 0, menu_sectors
// (+0x28) = 0. menu_sectors == 0 is what makes the sector provider serve the embedded menu.
struct FallbackState {
    uint32_t phase = 1;
    uint32_t raw_base = 0;
    uint32_t menu_sectors = 0;
};

// FUN_100300b8 decision: true when the request is answered from the embedded image.
inline bool fallback_serves(bool state_ok, uint32_t phase, uint32_t menu_sectors, uint32_t count, uint32_t lba) {
    return state_ok && phase == 1 && menu_sectors == 0 && count == 1 && lba >= kFirstSectorLba &&
           lba - kFirstSectorLba < kFallbackSectors;
}

// FUN_100300b8 conversion: 0x930 image bytes (nullptr = zeros) XOR the scrambler, one 32 bit word per
// 16 bit input word (0x1260 output bytes): word = (in ^ scr) << 8, low byte 0xFF when bit 8 is set.
void fallback_convert(const uint8_t* image_sector, const uint8_t* scrambler, uint32_t* out);

// Locates sector `index` (lba - 0x96, < 0x5D) in the image: nullptr past the 29 embedded sectors.
inline const uint8_t* fallback_sector(const uint8_t* image, uint32_t index) {
    return index < kFallbackImageSectors ? image + index * kSectorBytes : nullptr;
}

}  // namespace mh::aux
