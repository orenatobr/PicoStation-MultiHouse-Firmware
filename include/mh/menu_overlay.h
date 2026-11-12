// Overlay of the Manager 0.5.5 menu patch (Game ID wait, see docs/MEMCARD_FAVORITES.md) for SD cards that still
// hold the original menu. The menu is a PS1 program stored on the SD card; the Pico serves its raw sectors.
// When a served menu sector is byte for byte one of the original sectors the patch changes, it is replaced by
// the patched sector. Anything else (a menu already patched by the Manager, another menu) passes through.
#pragma once

#include <stdint.h>

namespace mh::menu_overlay {

constexpr uint32_t kSectorBytes = 2352;

struct Entry {
    uint32_t sector;         // sector index inside the menu image
    uint32_t original_hash;  // fnv1a32 over the 588 little endian words of the original sector
    const uint8_t* patched;  // kSectorBytes bytes
};

extern const uint32_t kCount;
extern const Entry kEntries[];

uint32_t fnv1a(const uint8_t* data, uint32_t bytes);  // bytes must be a multiple of 4

// Replaces `sector` (kSectorBytes bytes) when menu sector `k` is a known original. True when it did.
bool apply(uint32_t k, uint8_t* sector);
bool apply(uint32_t k, uint8_t* sector, const Entry* entries, uint32_t count);  // explicit table (tests)

}  // namespace mh::menu_overlay
