// V3 FUN_10025108 / FUN_10025094: Si5351 register table sequence.
#include "mh/aux_clock.h"

namespace mh::aux {

// Flash 0x100252ec. 0x00-0x0F: outputs off, drivers powered down, crystal load; PLL A (0x1A..0x21) and
// MS0/MS1 (0x22..0x29, 0x32..0x39) parameters, PLL reset (0xB1); then MS2 parameters and the output enable.
const uint8_t kClockTable[kClockTableLen][2] = {
    {0x03, 0xff}, {0x10, 0x80}, {0x11, 0x80}, {0x12, 0x80}, {0x13, 0x80}, {0x14, 0x80}, {0x15, 0x80}, {0x16, 0x80},
    {0x17, 0x80}, {0xb7, 0xc0}, {0x1a, 0x00}, {0x1b, 0x01}, {0x1c, 0x00}, {0x1d, 0x10}, {0x1e, 0x00}, {0x1f, 0x00},
    {0x20, 0x00}, {0x21, 0x00}, {0xb1, 0xa0}, {0x11, 0x0f}, {0x32, 0xeb}, {0x33, 0x04}, {0x34, 0x00}, {0x35, 0x06},
    {0x36, 0x75}, {0x37, 0xf4}, {0x38, 0x5d}, {0x39, 0x2c}, {0xa6, 0x00}, {0x22, 0x00}, {0x23, 0x01}, {0x24, 0x00},
    {0x25, 0x10}, {0x26, 0x00}, {0x27, 0x00}, {0x28, 0x00}, {0x29, 0x00}, {0xb1, 0xa0}, {0x12, 0x2f}, {0x3a, 0xc1},
    {0x3b, 0x71}, {0x3c, 0x00}, {0x3d, 0x06}, {0x3e, 0x61}, {0x3f, 0xf8}, {0x40, 0x41}, {0x41, 0xaf}, {0xa7, 0x00},
    {0x03, 0xf9},
};

// V3 FUN_10025094
bool clock_wait_status(const ClockBus& bus, uint8_t mask) {
    const uint32_t t0 = bus.now_us(bus.ctx);
    for (;;) {
        uint8_t v = 0;
        if (!bus.read_reg(bus.ctx, 0, &v)) return false;
        if ((v & mask) == 0) return true;
        if (bus.now_us(bus.ctx) - t0 > kClockStatusTimeoutUs - 1) return false;
    }
}

// V3 FUN_10025108 (register part)
ClockResult clock_program(const ClockBus& bus) {
    ClockResult r;
    if (!clock_wait_status(bus, kClockInitMask)) {
        r.status = 3;
        r.error = 7;
        return r;
    }
    for (uint32_t i = 0; i < kClockTableLen; ++i) {
        if (i == kClockLockWaitIndex && !clock_wait_status(bus, kClockLockMask)) {
            r.status = 3;
            r.error = 8;
            return r;
        }
        r.last_reg = kClockTable[i][0];
        r.last_value = kClockTable[i][1];
        if (!bus.write_reg(bus.ctx, r.last_reg, r.last_value)) {
            r.status = 3;
            r.error = 4;
            return r;
        }
        ++r.writes;
    }
    r.status = 2;
    return r;
}

}  // namespace mh::aux
