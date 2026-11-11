#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "edc.h"
#include "mh/hooks.h"
#include "picostation.h"
#include "pseudo_atomics.h"
#include <cstdio>
#include <csignal>
#include <cstring>

#include "disc_image.h"
#include "hardware/i2c.h"
#include "si5351.h"
#include "picostation_pinout.h"

uint32_t c_sectorMax = 333000;  // 74:00:00

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_15);  // Increase the VREG voltage to 1.15V for the faster clock speed
    sleep_ms(100); // Wait for the voltage to stabilize

    set_sys_clock_khz(271200, true);

    initPseudoAtomics();
    // V3 main: FUN_10024564 (trace init, which chains to eccedc_init, then clears the command hook state),
    // then FUN_100252dc (Si5351 table, then initHW) instead of the direct eccedc_init() / initHW() calls.
    mh::hooks::boot_trace();
    eccedc_init();
    mh::hooks::boot_clock();

    picostation::initHW();
    multicore_launch_core1(picostation::core1Entry);  // I2S Thread

    picostation::core0Entry();  // Reset, playback speed, Sled, soct, subq
    __builtin_unreachable();
}

// Weak defaults of the Multi House hooks (include/mh/hooks.h): the upstream behaviour. The integration code
// provides the strong definitions.
namespace mh::hooks {

__attribute__((weak)) bool mount_default_disc(picostation::DiscImage& disc) {
    disc.buildPlaceholderCue();
    return true;
}

__attribute__((weak)) void card_read_sector(picostation::DiscImage&, void* buffer, int, const uint16_t*) {
    memset(buffer, 0, 1176 * 4);
}

__attribute__((weak)) void on_mech_command(picostation::MechCommand&, uint32_t) {}
__attribute__((weak)) void on_door() {}
__attribute__((weak)) void poll() {}
__attribute__((weak)) void reset_card_state() {}
__attribute__((weak)) void boot_trace() {}
__attribute__((weak)) void boot_clock() {
    // Upstream clock bring-up (I2C0 + computed Si5351 setup) when the table based sequence is not linked.
    i2c_init(i2c0, 400 * 1000);
    gpio_set_function(GPIO_EXP_I2C0_SDA, GPIO_FUNC_I2C);
    gpio_set_function(GPIO_EXP_I2C0_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(GPIO_EXP_I2C0_SDA);
    gpio_pull_up(GPIO_EXP_I2C0_SCL);
    if (si5351_Init(0)) {
        si5351_SetupCLK1(53203425, SI5351_DRIVE_STRENGTH_8MA);
        si5351_SetupCLK2(53693175, SI5351_DRIVE_STRENGTH_8MA);
        si5351_EnableOutputs((1 << 1) | (1 << 2));
    }
}

}  // namespace mh::hooks
