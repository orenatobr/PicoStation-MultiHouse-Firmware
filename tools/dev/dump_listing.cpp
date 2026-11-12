// host: mount an image, set sel_case/list_offset, run library_listing, print the response as hex
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "mh/card.h"
using namespace mh::card;
static std::vector<uint8_t> g_img;
static int fi() { return 0; }
static int fr(uint32_t lba, uint32_t blocks, void* dst) {
    if ((uint64_t(lba) + blocks) * 512 > g_img.size()) return 1;
    memcpy(dst, g_img.data() + size_t(lba) * 512, size_t(blocks) * 512);
    return 0;
}
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb"); uint8_t b[65536]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) g_img.insert(g_img.end(), b, b + n);
    fclose(f);
    set_sd({fi, fr});
    static CardDisc d;
    if (!mount(&d)) { puts("MOUNT FAIL"); return 1; }
    state().sel_case = strtoul(argv[2], 0, 0);
    state().list_offset = strtoul(argv[3], 0, 0);
    library_listing();
    const uint8_t* r = response();
    for (unsigned i = 0; i < kRespSize; ++i) printf("%02x", r[i]);
    puts("");
}
