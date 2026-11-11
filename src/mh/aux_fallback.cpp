// V3 FUN_10030000 / FUN_100300b8: built-in fallback menu.
#include "mh/aux_fallback.h"

namespace mh::aux {

// V3 FUN_10030000 (table part)
void fallback_table(DiscTable& t) {
    const TocTrack data{kFallbackSectors, 0, 0, true};
    toc_build(t, &data, 1, kFallbackSectors, 0);
}

// V3 FUN_100300b8
void fallback_convert(const uint8_t* image_sector, const uint8_t* scrambler, uint32_t* out) {
    for (uint32_t i = 0; i < kSectorBytes; i += 2) {
        uint32_t in = image_sector ? static_cast<uint32_t>(le16(image_sector + i)) : 0u;
        uint32_t v = (in ^ le16(scrambler + i)) << 8;
        if (v & 0x100) v |= 0xFF;
        out[i / 2] = v;
    }
}

}  // namespace mh::aux
