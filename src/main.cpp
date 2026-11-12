#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "edc.h"
#include "mh/hooks.h"
#include "picostation.h"
#include "pseudo_atomics.h"
#include <cstdio>
#include <csignal>

#include "disc_image.h"

uint32_t c_sectorMax = 333000;  // 74:00:00

// The original V3 reads the flash unique ID first thing in main (found by comparing the boot of both images in an emulator).
static pico_unique_board_id_t g_boardId;

int main() {
    pico_get_unique_board_id(&g_boardId);
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
