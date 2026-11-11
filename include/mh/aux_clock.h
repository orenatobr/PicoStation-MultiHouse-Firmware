// Si5351 boot programming of the V3 firmware: a fixed register table written over I2C0 (replaces the
// computed si5351_Init / SetupCLK1 / SetupCLK2 / EnableOutputs sequence of upstream) with a diagnostic
// block "PSI1". V3 FUN_10025108 (sequence), FUN_10025000/FUN_1002505c/FUN_10025074 (I2C polls),
// FUN_10025094 (status wait), FUN_100252dc (entry: runs the sequence, then continues into upstream init).
// The I2C0 controller/pad setup of FUN_10025108 stays in the pico-only glue; this file is the host-testable part.
#pragma once

#include "mh/platform.h"

namespace mh::aux {

constexpr uint32_t kClockTableLen = 49;
extern const uint8_t kClockTable[kClockTableLen][2];  // {register, value}, flash 0x100252ec
constexpr uint32_t kClockLockWaitIndex = 0x30;         // lock wait runs before this table entry
constexpr uint32_t kClockStatusTimeoutUs = 100000;     // 0x1869F
constexpr uint8_t kClockInitMask = 0x80;               // reg 0 SYS_INIT
constexpr uint8_t kClockLockMask = 0xE8;               // reg 0 SYS_INIT | LOL_B | LOL_A | LOS_XTAL

// Diagnostic values stored by V3 in the "PSI1" block (not used for programming).
constexpr uint32_t kClockClk1Hz = 53203425;
constexpr uint32_t kClockClk2Hz = 53693175;
constexpr uint32_t kClockI2cHz = 400000;
constexpr uint32_t kClockVcoHz = 271200000;

struct ClockBus {
    bool (*write_reg)(void* ctx, uint8_t reg, uint8_t value);
    bool (*read_reg)(void* ctx, uint8_t reg, uint8_t* value);
    uint32_t (*now_us)(void* ctx);
    void* ctx;
};

// PSI1 status (block word 2): 1 running, 2 done, 3 failed.
struct ClockResult {
    uint32_t status = 1;
    uint32_t error = 0;   // block word 3 / 0xC: 4 TX empty, 5 RX, 6 STOP, 7 SYS_INIT wait, 8 lock wait
    uint32_t writes = 0;  // block word 7
    uint8_t last_reg = 0, last_value = 0;
};

// FUN_10025094: poll register 0 until (value & mask) == 0 or 100000 us elapse.
bool clock_wait_status(const ClockBus& bus, uint8_t mask);

// Sequence after the controller is enabled (second half of FUN_10025108).
ClockResult clock_program(const ClockBus& bus);

}  // namespace mh::aux
