// Cheat blob validators and renderers (V3 FUN_100228c8 for version 4, FUN_1002e100 for version 6).
#include "mh/handler_cheat.h"

namespace mh::handler {

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr char kBlobMagic[8] = {'P', 'S', 'T', 'C', 'H', 'T', '1', '\0'};  // flash 0x100235dd / 0x1002e942

inline uint32_t le16(const uint8_t* p) { return static_cast<uint32_t>(p[0] | p[1] << 8); }
inline uint32_t le32(const uint8_t* p) { return card::read_u32(p); }
inline uint32_t be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}
inline uint32_t be16(const uint8_t* p) { return static_cast<uint32_t>(p[0] << 8 | p[1]); }

bool magic_ok(const uint8_t* b) {
    for (int i = 0; i < 8; ++i) {
        if (b[i] != static_cast<uint8_t>(kBlobMagic[i])) return false;
    }
    return true;
}

// Fields shared by the v4 and v6 header (+0x28 total size is checked per version).
struct Header {
    uint32_t count;       // +0x10 entries, 1..0x100
    uint32_t flag0_count; // +0x14 entries with flag bit0 set
    uint32_t lines;       // +0x18 total code lines
    uint32_t title;       // +0x2c title index
    uint32_t variant;     // +0x30
    uint32_t db_crc;      // +0x34
    uint8_t scope;        // +0x38
    uint8_t game;         // +0x39
};

Header read_header(const uint8_t* b) {
    return Header{le32(b + 0x10), le32(b + 0x14), le32(b + 0x18), le32(b + 0x2c),
                  le32(b + 0x30), le32(b + 0x34), b[0x38],        b[0x39]};
}

// CRC-32 of the blob with the 4 bytes at +0x3c read as zero (FUN_100228c8 inline loop, FUN_1002e100 loop).
uint32_t blob_crc(const uint8_t* b, uint32_t total) {
    const uint8_t zero[4] = {0, 0, 0, 0};
    uint32_t c = card::crc32_update(0xFFFFFFFFu, b, 0x3C);
    c = card::crc32_update(c, zero, 4);
    c = card::crc32_update(c, b + 0x40, total - 0x40);
    return ~c;
}

// Name rules shared by v4 and v6 differ (v4 allows every printable except '[' and ']').
bool v4_name_ok(const uint8_t* name, uint32_t* len_out) {
    uint32_t i = 0;
    while (name[i] != 0) {
        uint32_t c = name[i];
        if (c - 0x20 > 0x5E || ((c - 0x5B) & 0xFD) == 0) return false;
        if (++i == 0x40) return false;
    }
    *len_out = i;
    return i != 0 && card::all_zero(name + i + 1, 0x3F - i);
}

// v6 name alphabet: space & ' - . 0-9 A-Z (mask 0x3ff60c1 over 0x20..0x39, flash 0x1002e77c).
bool v6_char_ok(uint32_t c) {
    if (c < 0x3A) return c >= 0x20 && ((0x3FF60C1u >> (c - 0x20)) & 1u) != 0;
    return c - 0x41 <= 0x19;
}

// v6 line opcode byte (first byte of the 6 byte line): 0x00 0x80 0x20 0x21 0x30 0x31 0x70 0x90 0xC1.
bool v6_opcode_ok(uint32_t op) {
    if ((op & 0x7F) == 0) return true;
    if ((op & 0xEE) == 0x20) return true;
    if (((op - 0x70) & ~0x20u) == 0) return true;
    return op == 0xC1;
}

// Sink for the v6 chunk renderer (FUN_1002e038): characters before `start` are counted but not stored.
struct ChunkSink {
    uint32_t start;
    uint32_t pos;
    uint32_t len;
    uint8_t* out;
    void put(uint8_t c) {
        if (start <= pos && len < kChunkBytes) out[len++] = c;
        ++pos;
    }
    void str(const char* s) {
        for (; *s; ++s) put(static_cast<uint8_t>(*s));
    }
    void hex(uint32_t v, uint32_t nibbles) {
        for (uint32_t n = nibbles * 4; n != 0; n -= 4) put(static_cast<uint8_t>("0123456789ABCDEF"[(v >> (n - 4)) & 0xF]));
    }
};

const char kEntryTail[] = "]\nType=V5Native1\nMHFlags=";  // flash 0x100235a4 / 0x1002e917
const char kMultiTag[] = " (M)";                           // flash 0x100235be

}  // namespace

const char kTextHeader[] = "MHDialect=V5ORIG_BANK_V01\n";  // flash 0x10023589 / 0x1002e8fc

uint32_t blob_version(const uint8_t* blob, uint32_t size) {
    if (size < kBlobHeader || !magic_ok(blob)) return 0;
    return le16(blob + 8);
}

// ---------------------------------------------------------------------------------------------
// Version 4
// ---------------------------------------------------------------------------------------------
bool v4_validate(const uint8_t* b, uint32_t size) {
    if (size < kBlobHeader) return false;
    if (!magic_ok(b) || le16(b + 8) != 4 || le16(b + 0xA) != 0x40 || le32(b + 0xC) != 0) return false;
    const Header h = read_header(b);
    if (h.count - 1 > 0xFF) return false;
    if (le32(b + 0x1C) != 0x60 || le32(b + 0x20) != 0x40) return false;
    const uint32_t table_end = h.count * 0x60 + 0x40;  // first code line
    if (le32(b + 0x24) != table_end) return false;
    const uint32_t total = le32(b + 0x28);
    if (total < table_end || size < total) return false;
    if (h.lines > 0x100) return false;  // stricter than the profile limit 510, see CARD_LAYOUT_CROSSCHECK
    if (h.scope != 2 || h.game > 1 || b[0x3A] != 0) return false;
    if (h.game != 0) {
        if (h.title == kNone || h.variant == kNone) return false;
        if (h.db_crc != kCheatDbCrc) return false;  // cave 0x1002b000
    } else if (h.title != kNone || h.variant != kNone || h.db_crc != 0) {
        return false;
    }
    if (le32(b + 0x3C) != blob_crc(b, total)) return false;
    if (!card::all_zero(b + total, size - total)) return false;

    uint32_t cursor = table_end, lines_sum = 0, flag0 = 0, audited = 0;
    for (uint32_t i = 0; i < h.count; ++i) {
        const uint8_t* e = b + 0x40 + i * 0x60;
        const uint32_t lines = le16(e + 0x4C);
        const uint32_t egame = e[0x4E];
        const uint32_t flags = e[0x4F];
        const uint32_t bytes = lines * 6;
        if (lines == 0 || le32(e + 0x48) != cursor || total < cursor || total - cursor < bytes) return false;
        if (flags > 0x3F) return false;
        if (((flags & 8) >> 3) == ((flags & 4) >> 2)) return false;  // audited xor custom
        if ((flags & 0x10) && !(flags & 8)) return false;
        bool needs_audit_fields = false;
        if ((flags & 2) == 0) {
            if (flags & 8) {
                needs_audit_fields = true;
            } else if (flags & 4) {  // custom entry: no game, no ids
                if (egame != 0 || le32(e + 0x40) != kNone || le32(e + 0x44) != kNone) return false;
            }
        } else {
            if ((flags & 9) != 9) return false;
            needs_audit_fields = true;
        }
        if (needs_audit_fields) {
            if (h.game == 0 || egame != h.game) return false;
            if (le32(e + 0x40) != h.variant) return false;
            if (le32(e + 0x44) == kNone || (flags & 4)) return false;
        }
        if (!card::all_zero(e + 0x54, 0xC)) return false;
        if (card::crc32(b + cursor, bytes) != le32(e + 0x50)) return false;
        uint32_t name_len;
        if (!v4_name_ok(e, &name_len)) return false;
        cursor += bytes;
        lines_sum += lines;
        flag0 += flags & 1;
        audited += (flags & 8) != 0;
    }
    if (cursor != total || lines_sum != h.lines || flag0 != h.flag0_count) return false;
    if (h.game != 0 && audited == 0) return false;
    return true;
}

bool v4_render(const uint8_t* b, card::TextBuf& out) {
    card::zero_fill(out.data, card::kTextSize);
    out.len = 0;
    if (!card::text_append_str(out, kTextHeader)) return false;
    const Header h = read_header(b);
    uint32_t cursor = h.count * 0x60 + 0x40;
    for (uint32_t i = 0; i < h.count; ++i) {
        const uint8_t* e = b + 0x40 + i * 0x60;
        const uint32_t lines = le16(e + 0x4C);
        const uint32_t flags = e[0x4F];
        if (!card::text_append_char(out, '[')) return false;
        for (const uint8_t* p = e; *p != 0; ++p) {
            if (!card::text_append_char(out, static_cast<char>(*p))) return false;
        }
        if ((flags & 2) && !card::text_append_str(out, kMultiTag)) return false;
        if (!card::text_append_str(out, kEntryTail)) return false;
        if (!card::text_append_hex(out, flags & 3, 2) || !card::text_append_char(out, '\n')) return false;
        for (uint32_t k = 0; k < lines; ++k) {
            const uint8_t* ln = b + cursor + k * 6;
            if (!card::text_append_hex(out, be32(ln), 8) || !card::text_append_char(out, ' ') ||
                !card::text_append_hex(out, be16(ln + 4), 4) || !card::text_append_char(out, '\n'))
                return false;
        }
        if (!card::text_append_char(out, '\n')) return false;
        cursor += lines * 6;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Version 6
// ---------------------------------------------------------------------------------------------
bool v6_validate(const uint8_t* b, uint32_t size) {
    if (size < kBlobHeader || size - kBlobHeader > 0x27C0) return false;
    if (!magic_ok(b) || le16(b + 8) != 6 || le16(b + 0xA) != 0x40 || le32(b + 0xC) != 0) return false;
    const Header h = read_header(b);
    if (h.count - 1 > 0xFF) return false;
    if (le32(b + 0x1C) != 8 || le32(b + 0x20) != 0x40) return false;
    const uint32_t names = (h.count + 8) * 8;  // first name
    if (le32(b + 0x24) != names || names > size || le32(b + 0x28) != size) return false;
    if (h.title >= 0x3EC || h.variant != h.title || h.db_crc != kCheatDbCrc) return false;
    if (static_cast<uint32_t>(h.scope) - 1u >= 2u || h.game != 1 || b[0x3A] != 0) return false;
    if (le32(b + 0x3C) != blob_crc(b, size)) return false;

    uint8_t seen[32] = {};  // bitmap of the 0..255 entry ids already used
    uint32_t pos = names, lines_sum = 0, flag0 = 0;
    for (uint32_t i = 0; i < h.count; ++i) {
        const uint8_t* e = b + 0x40 + i * 8;
        const uint32_t name_off = le16(e), lines_off = le16(e + 2), id = le16(e + 4);
        const uint32_t lines = e[6], flags = e[7];
        if (id > 0xFF || lines == 0 || (flags & 0xCC) != 8 || (flags & 3) == 2) return false;
        if ((seen[id >> 3] >> (id & 7)) & 1) return false;
        seen[id >> 3] |= static_cast<uint8_t>(1u << (id & 7));
        if (name_off != pos) return false;
        uint32_t p = pos;
        for (;;) {
            if (size <= p) return false;
            const uint32_t c = b[p];
            if (c == 0) break;
            ++p;
            if (pos + 0x30 == p) return false;  // 47 characters at most
            if (!v6_char_ok(c)) return false;
        }
        if (p == pos) return false;
        const uint32_t first_line = p + 1;
        if (lines_off != first_line || size - first_line < lines * 6) return false;
        for (uint32_t k = 0; k < lines; ++k) {
            if (!v6_opcode_ok(b[first_line + k * 6])) return false;
        }
        pos = first_line + lines * 6;
        lines_sum += lines;
        flag0 += flags & 1;
    }
    if (pos != size || le32(b + 0x14) != flag0 || le32(b + 0x18) != lines_sum) return false;
    return lines_sum < 0x1FF && h.scope == 2;
}

ChunkOut v6_render_chunk(const uint8_t* b, uint32_t size, uint32_t chunk, uint8_t* out) {
    ChunkSink s{chunk * kChunkBytes, 0, 0, out};
    s.str(kTextHeader);
    const uint32_t count = le32(b + 0x10);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = b + 0x40 + i * 8;
        const uint32_t name_off = le16(e), lines_off = le16(e + 2), lines = e[6], flags = e[7];
        // Length of this entry's text is known up front: skip it when it ends before the chunk starts.
        const uint32_t est_end = s.pos + lines * 0xE + 0x1D + (lines_off - name_off);
        if (s.start < est_end && s.len != kChunkBytes) {
            s.put('[');
            s.str(reinterpret_cast<const char*>(b + name_off));
            s.str(kEntryTail);
            s.hex(flags & 3, 2);
            s.put('\n');
            for (uint32_t k = 0; k < lines; ++k) {
                const uint8_t* ln = b + lines_off + k * 6;
                s.hex(be32(ln), 8);
                s.put(' ');
                s.hex(be16(ln + 4), 4);
                s.put('\n');
            }
            s.put('\n');
        }
        s.pos = est_end;
    }
    return ChunkOut{s.len, s.pos <= s.start + s.len};
}

}  // namespace mh::handler
