/* Long names for the ORIGINAL PicoStation_MH_V3 firmware: code that tools/patch_original.py appends after the
 * original image (at 0x10041000) and calls from three patched call sites of the original library listing
 * (FUN_100222e0). Semantics follow docs/LONG_NAMES.md and card.cpp of the rebuild (join_long, disc_long).
 * Everything the original owns is reached by address (see the defines); nothing here is linked to it. */
typedef unsigned char u8;
typedef unsigned int u32;

#define STATE ((const volatile u32*)0x20033400u)   /* CardState; +0x1C = disc_lba */
#define TEXT_BUF ((char*)0x2003C000u)                /* 80 bytes of RAM the original never touches */
#define SD_READ ((int (*)(u32, u32, void*))0x20003415u)             /* FUN_20003414, 0 = ok */
#define LISTING_APPEND ((int (*)(u32*, const char*, u32))0x10022285u) /* FUN_10022284 */
#define CRC32 ((u32 (*)(const void*, u32))0x10021775u)               /* FUN_10021774 */

static u32 rd32(const u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

/* The Disc record `id` (0x100 bytes inside a 512 byte block read into `buf`) when it is a valid record that
 * carries a long names extension; fills *tl / *cl (title_ext / case_ext lengths) and returns the extension. */
static const u8* disc_ext(u32 id, u8* buf, u32* tl, u32* cl, const u8** rec) {
    u32 lba = STATE[0x1C / 4] + (id >> 1);
    int ok = 0;
    for (int t = 0; t < 3 && !ok; ++t) ok = SD_READ(lba, 1, buf) == 0;
    if (!ok) return 0;
    const u8* r = buf + ((id & 1) << 8);
    if (r[0] != 'D' || r[1] != 'I' || r[2] != 'S' || r[3] != 'C' || r[5] != id) return 0;
    if (CRC32(r, 0xFC) != rd32(r + 0xFC)) return 0;
    const u8* x = r + 0x94;
    if (x[0] != 'L' || x[1] != 'N' || x[2] != 'X' || x[3] != '1') return 0;
    *tl = x[4];
    *cl = x[5];
    if (*tl > 40 || *cl > 40) return 0;
    for (u32 i = 0; i < *tl; ++i) if (x[6 + i] < 0x20 || x[6 + i] > 0x7E) return 0;
    for (u32 i = 0; i < *cl; ++i) if (x[0x2E + i] < 0x20 || x[0x2E + i] > 0x7E) return 0;
    *rec = r;
    return x;
}

/* base (at most `cap` characters) followed by len extension characters */
static void join(char* out, const char* base, u32 cap, const u8* ext, u32 len) {
    u32 n = 0;
    while (base[n] != 0 && n != cap) {
        out[n] = base[n];
        ++n;
    }
    for (u32 i = 0; i != len; ++i) out[n++] = (char)ext[i];
    out[n] = 0;
}

/* Case row of the whole library listing: FUN_10022284(pos, name, flag), with the Case record `rec` of the row. */
int case_row_c(u32* pos, const char* name, u32 flag, const u8* rec) {
    if (rec[0x3F] == 1) {
        u8 buf[512];
        u32 tl, cl;
        const u8* r;
        const u8* x = disc_ext(rec[0x3A], buf, &tl, &cl, &r);
        if (x && r[6] == rec[6] && r[7] == 0 && cl != 0) {
            char out[80];
            join(out, name, 31, x + 0x2E, cl);
            return LISTING_APPEND(pos, out, flag);
        }
    }
    return LISTING_APPEND(pos, name, flag);
}

/* Title of a Disc row: `di` is the validated DiscInfo of the original (id at +0, title at +0x40). */
const char* disc_title_c(const u8* di) {
    const char* base = (const char*)(di + 0x40);
    u8 buf[512];
    u32 tl, cl;
    const u8* r;
    const u8* x = disc_ext(di[0], buf, &tl, &cl, &r);
    if (!x || tl == 0) return base;
    join(TEXT_BUF, base, 31, x + 6, tl);
    return TEXT_BUF;
}

__attribute__((naked)) void case_row_tramp(void) {
    __asm__ volatile(
        "push {r4, lr}\n"
        "ldr r3, [sp, #8]\n"      /* [sp,#0] of the listing frame: pointer to the current Case record */
        "bl case_row_c\n"
        "pop {r4, pc}\n");
}

__attribute__((naked)) void disc_title_tramp(void) {
    __asm__ volatile(
        "push {r4, lr}\n"
        "movs r0, r5\n"           /* r5 = DiscInfo of the row */
        "bl disc_title_c\n"
        "movs r3, r0\n"
        "ldrb r2, [r3, #0]\n"
        "pop {r4, pc}\n");
}
