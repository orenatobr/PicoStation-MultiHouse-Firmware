// Cheat blob validators and text renderers of the V3 PS1 command handler.
// Host safe: no pico headers. Wire format and constants: docs/re/handler.md.
//
// Two blob versions share the "PSTCHT1\0" header (0x40 bytes):
//   version 4 "wire" blob: 0x60 byte entries, validated and rendered by the main handler FUN_100228c8,
//   version 6 "compact" blob: 8 byte entries, validated and rendered by FUN_1002e100 (and the unreferenced
//   twin FUN_1002c0a0).
#pragma once

#include "mh/card.h"
#include "mh/platform.h"

namespace mh::handler {

// CRC-32 of the cheat database the blobs must have been built from (FUN_1002b000 for v4, FUN_1002e100 for v6).
constexpr uint32_t kCheatDbCrc = 0xA06CFC31;

constexpr uint32_t kBlobHeader = 0x40;        // header size, also the offset of the entry table
constexpr uint32_t kBlobMaxV4Plain = 0x1400;  // largest v4 blob of a disc without the EXT layout
constexpr uint32_t kBlobMaxExt = 0x2800;      // largest blob of a disc with the EXT layout (flags bit1)
constexpr uint32_t kTextMax = 0x2FFF;         // longest rendered text (FUN_100218ac refuses beyond)
constexpr uint32_t kChunkBytes = 0x800;       // text bytes per PCH2 chunk

// Text rendered for the PS1 (the header line and the per entry blocks), see docs/re/handler.md section 5.
extern const char kTextHeader[];              // "MHDialect=V5ORIG_BANK_V01\n"

// Version 4 (wire) blob. `size` is the Disc record cheat_size, the number of valid bytes in `blob`.
// Reproduces every check of FUN_100228c8 (header, entry table, per entry CRC, name set, zero padding).
bool v4_validate(const uint8_t* blob, uint32_t size);
// Renders the whole blob as text into `out` (appends). False when the text would not fit (original: the
// blob is refused). Call only after v4_validate succeeded.
bool v4_render(const uint8_t* blob, card::TextBuf& out);

// Version 6 (compact) blob. `size` is the Disc record cheat_size.
bool v6_validate(const uint8_t* blob, uint32_t size);
struct ChunkOut {
    uint32_t len;   // bytes written to `out` (0..0x800)
    bool last;      // true when this chunk reaches the end of the text
};
// Renders the text bytes [chunk * 0x800, chunk * 0x800 + 0x800) of a validated v6 blob into `out`
// (at least 0x800 bytes). Entries entirely before the chunk are skipped without being formatted.
ChunkOut v6_render_chunk(const uint8_t* blob, uint32_t size, uint32_t chunk, uint8_t* out);

// Blob header peek: u16 version at +8, or 0 when the magic does not match.
uint32_t blob_version(const uint8_t* blob, uint32_t size);

}  // namespace mh::handler
