#include "mh/menu_overlay.h"

#include <string.h>

namespace mh::menu_overlay {

uint32_t fnv1a(const uint8_t* data, uint32_t bytes) {
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < bytes; i += 4) {
        uint32_t w;
        memcpy(&w, data + i, 4);
        h = (h ^ w) * 0x01000193u;
    }
    return h;
}

bool apply(uint32_t k, uint8_t* sector, const Entry* entries, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (entries[i].sector != k) continue;
        if (fnv1a(sector, kSectorBytes) != entries[i].original_hash) return false;
        memcpy(sector, entries[i].patched, kSectorBytes);
        return true;
    }
    return false;
}

bool apply(uint32_t k, uint8_t* sector) { return apply(k, sector, kEntries, kCount); }

}  // namespace mh::menu_overlay
