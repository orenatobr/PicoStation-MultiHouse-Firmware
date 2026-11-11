// Device side of the Multi House hooks: binds mh::hooks::Hw (include/mh/hooks_hw.h) to the SD driver, the
// DiscImage sector builder, the sector limit and the Si5351 I2C bus. Excluded from the host tests.
#include "disc_image.h"
#include "ff.h"
#include "hardware/i2c.h"
#include "hardware/timer.h"
#include "mh/hooks_hw.h"
#include "mh/platform.h"
#include "picostation_pinout.h"
#include "values.h"

// SD driver (third_party/SD-fatfs/SD/sd_spi.c), C linkage, not declared in a header.
extern "C" int sd_init(void);
extern "C" int sd_read_blocks(uint32_t block, size_t count, uint8_t* buf, const uint16_t* sc, uint8_t dt);

// binary/picostation-menu.bin, embedded by addBinaryFileWithSize in CMakeLists.txt.
extern "C" const uint8_t loaderImage[];
extern "C" const uint32_t loaderImageSize;

namespace {

constexpr uint8_t kSi5351Address = 0x60;
constexpr uint32_t kI2cTimeoutUs = 20000;

// The card layer addresses the disc by raw offsets (mh::card::CardDisc); keep the field positions honest.
static_assert(offsetof(CueDisc, tracks) == 4, "CardCue layout");
static_assert(sizeof(CueTrack) == 0x34, "CardTrack layout");
static_assert(offsetof(CueTrack, size) == 4 && offsetof(CueTrack, fileOffset) == 8 &&
                  offsetof(CueTrack, indexCount) == 0xC && offsetof(CueTrack, indices) == 0x10 &&
                  offsetof(CueTrack, trackType) == 0x1C,
              "CardTrack fields");
static_assert(sizeof(CueDisc) == 0x1470, "CardCue size");

int __not_in_flash_func(sd_read)(uint32_t lba, uint32_t blocks, void* dst) {
    return sd_read_blocks(lba, blocks, static_cast<uint8_t*>(dst), nullptr, 0);
}

void __time_critical_func(build_sector)(picostation::DiscImage& disc, int sector, uint32_t* buffer,
                                        const uint16_t* user, const uint16_t* scrambling) {
    disc.buildSector(sector, buffer, const_cast<uint16_t*>(user), scrambling);
}

void __time_critical_func(scramble)(uint32_t* dst, const uint16_t* src, const uint16_t* scrambling, uint32_t words) {
    scramble_data(dst, const_cast<uint16_t*>(src), scrambling, words);
}

void set_sector_max(uint32_t total) { c_sectorMax = static_cast<int>(total); }
void set_card_mode(picostation::DiscImage& disc, bool on) { disc.set_card_mode(on); }
uint32_t now_us() { return time_us_32(); }

void i2c_open() {
    i2c_init(i2c0, 400 * 1000);
    gpio_set_function(GPIO_EXP_I2C0_SDA, GPIO_FUNC_I2C);
    gpio_set_function(GPIO_EXP_I2C0_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(GPIO_EXP_I2C0_SDA);
    gpio_pull_up(GPIO_EXP_I2C0_SCL);
}

bool i2c_write(uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = {reg, value};
    return i2c_write_timeout_us(i2c0, kSi5351Address, buf, 2, false, kI2cTimeoutUs) == 2;
}

bool i2c_read(uint8_t reg, uint8_t* value) {
    return i2c_write_timeout_us(i2c0, kSi5351Address, &reg, 1, true, kI2cTimeoutUs) == 1 &&
           i2c_read_timeout_us(i2c0, kSi5351Address, value, 1, false, kI2cTimeoutUs) == 1;
}

const mh::hooks::Hw kHw = {
    sd_init, sd_read, build_sector, scramble, set_sector_max, set_card_mode, now_us,
    i2c_open, i2c_write, i2c_read, loaderImage, 0,
};

// Bound before main(): the hooks are first used by main() (boot_trace).
struct Binder {
    Binder() {
        static mh::hooks::Hw hw = kHw;
        hw.menu_image_size = loaderImageSize;
        mh::hooks::set_hw(&hw);
    }
} g_binder;

}  // namespace
