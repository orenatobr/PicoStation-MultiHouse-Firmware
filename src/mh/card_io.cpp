// Block IO of the V2 card layer (V3 FUN_10021c14, FUN_10021ce4, FUN_1002200c).
// The original card code is size optimised; keep the same shape on the device.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("Os")
#endif
#include "mh/card.h"

namespace mh::card {

// V3 FUN_10021c14: up to 3 tries, retry_delay after every failed try
bool read_blocks(uint32_t lba, uint32_t blocks) {
    if (blocks == 0 || blocks > 5 || !sd().read) return false;
    uint32_t tries = 0;
    do {
        if (sd().read(lba, blocks, block_buffer()) == 0) return true;
        const uint32_t next = tries + 1;
        retry_delay(tries);
        tries = next;
    } while (tries != 3);
    return false;
}

// V3 FUN_10021ce4: copies `len` bytes starting `byte_off` bytes after block `lba`. Aligned runs of
// at least one block are read up to 5 blocks at a time, everything else one block at a time.
bool read_bytes(uint32_t lba, uint32_t byte_off, void* dst, uint32_t len) {
    auto* out = static_cast<uint8_t*>(dst);
    for (;;) {
        uint32_t blocks, chunk;
        if ((byte_off & 0x1FF) == 0 && len > 0x1FF) {
            blocks = len >> 9;
            if (blocks > 5) blocks = 5;
            chunk = blocks << 9;
        } else {
            chunk = 0x200 - (byte_off & 0x1FF);
            if (len < chunk) chunk = len;
            blocks = 1;
        }
        if (!read_blocks((byte_off >> 9) + lba, blocks)) return false;
        copy(out, block_buffer() + (byte_off & 0x1FF), chunk);
        out += chunk;
        byte_off += chunk;
        len -= chunk;
        if (len == 0) return true;
    }
}

// V3 FUN_1002200c: a 5 block slot whose bytes 0x930..0xA00 are zero, `tries` attempts
bool read_slot(uint32_t lba, uint32_t tries) {
    uint32_t n = 0;
    while (!(read_blocks(lba, 5) && all_zero(block_buffer() + kSlotUsed, kSlot - kSlotUsed))) {
        const uint32_t next = n + 1;
        retry_delay(n);
        n = next;
        if (n >= tries) return false;
    }
    return true;
}

}  // namespace mh::card
