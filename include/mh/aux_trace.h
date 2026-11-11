// Event trace "BT01" of the V3 firmware (RAM 0x20040000..0x20041800): a fixed-size record of the timing and
// status around the two hooked entry points. V3 FUN_10024044 (init), FUN_10024000 (status word),
// FUN_10024090 (wraps FUN_10021624, the command hook chain), FUN_10024188 (wraps RAM 0x20000484).
// Host-testable model: the hardware reads (timer, UART flags, GPIO ctrl) are parameters.
#pragma once

#include "mh/platform.h"

namespace mh::aux {

constexpr uint32_t kTraceMagic = 0x31305442;  // "BT01"
constexpr uint32_t kTraceRingA = 0x80;        // first 128 events (0x20040000), later ones cycle in ring B
constexpr uint32_t kTraceRingB = 0x40;        // 64 events at 0x20041200
constexpr uint32_t kTraceIrq = 0x10;          // 16 events at 0x20041040

struct TraceEvent {  // 16 bytes
    uint32_t t_start = 0, counter_after = 0, flags_before = 0, flags_after = 0;
};
struct TraceIrqEvent {  // 24 bytes
    uint32_t t_start = 0, aux = 0, flags_before = 0, flags_after = 0, uart_flags = 0, gpio_ctrl = 0;
};
struct TraceBlock {
    TraceEvent ring_a[kTraceRingA];
    uint32_t magic = 0, version = 0, boot_timer = 0;
    uint32_t head = 0, done = 0;            // +0xC, +0x10
    uint32_t irq_head = 0, irq_done = 0;    // +0x14, +0x18
    uint32_t irq_overflow = 0, busy = 0;    // +0x1C, +0x20
    TraceIrqEvent irq[kTraceIrq];
    TraceEvent ring_b[kTraceRingB];
};

// Inputs of the status word (FUN_10024000).
struct TraceFlagsIn {
    bool global_93ec = false;   // byte at RAM 0x200093ec
    uint32_t global_8820 = 0;   // word at RAM 0x20008820, shifted to bit 16
    uint8_t byte08 = 0;         // arg[8]
    bool byte1a = false, byte15 = false, byte20 = false;
};

uint32_t trace_flags(const TraceFlagsIn& in);                    // FUN_10024000
void trace_init(TraceBlock& t, uint32_t timer);                  // FUN_10024044 (the chained init is glue)
TraceEvent& trace_begin(TraceBlock& t, uint32_t timer, uint32_t flags);   // first half of FUN_10024090
void trace_end(TraceBlock& t, TraceEvent& ev, uint32_t counter, uint32_t flags);  // second half
// FUN_10024188; returns nullptr when the 16 slot table is full (only the overflow count moves).
TraceIrqEvent* trace_irq_begin(TraceBlock& t, uint32_t timer, uint32_t aux, uint32_t flags, uint32_t uart,
                               uint32_t gpio);
void trace_irq_end(TraceBlock& t, TraceIrqEvent* ev, uint32_t flags);

}  // namespace mh::aux
