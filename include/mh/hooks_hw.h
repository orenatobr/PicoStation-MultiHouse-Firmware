// Hardware operations behind the Multi House hooks (src/mh/hooks_impl.cpp). The firmware binds the real
// implementations in src/mh/hooks_pico.cpp; the host test (tests/test_integration.cpp) binds fakes, so the
// glue logic runs on the host without pico headers.
#pragma once

#include <stdint.h>

namespace picostation {
class DiscImage;
}

namespace mh::hooks {

struct Hw {
    // SD driver (third_party/SD-fatfs/SD/sd_spi.c): 0 = ok. `dst` has room for blocks * 512 bytes.
    int (*sd_init)();
    int (*sd_read)(uint32_t lba, uint32_t blocks, void* dst);
    // DiscImage::buildSector(sector, buffer, user data, scrambling, pregap = false): the 2324 byte response is
    // wrapped in a mode 2 form 2 sector (null `user` = zero data).
    void (*build_sector)(picostation::DiscImage& disc, int sector, uint32_t* buffer, const uint16_t* user,
                         const uint16_t* scrambling);
    // scramble_data (ff.c): 16 bit words -> 32 bit PIO words; null table = no scrambling.
    void (*scramble)(uint32_t* dst, const uint16_t* src, const uint16_t* scrambling, uint32_t words);
    void (*set_sector_max)(uint32_t total_sectors);       // c_sectorMax
    void (*set_card_mode)(picostation::DiscImage& disc, bool on);  // DiscImage +0x147A
    uint32_t (*now_us)();
    // Si5351 on I2C0 (address 0x60).
    void (*i2c_open)();
    bool (*i2c_write)(uint8_t reg, uint8_t value);
    bool (*i2c_read)(uint8_t reg, uint8_t* value);
    // Built-in fallback menu image (binary/picostation-menu.bin).
    const uint8_t* menu_image;
    uint32_t menu_image_size;
};

// Binds the operations; the pointed struct must stay valid. Called by the pico glue before main(), by the
// host test before each scenario.
void set_hw(const Hw* hw);

}  // namespace mh::hooks
