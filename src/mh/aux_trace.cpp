// V3 FUN_10024000 / FUN_10024044 / FUN_10024090 / FUN_10024188: "BT01" event trace.
#include "mh/aux_trace.h"

namespace mh::aux {

// V3 FUN_10024000
uint32_t trace_flags(const TraceFlagsIn& in) {
    return (in.global_93ec ? 0x100u : 0u) | in.global_8820 << 16 | in.byte08 | (in.byte1a ? 0x200u : 0u) |
           (in.byte15 ? 0x400u : 0u) | (in.byte20 ? 0x800u : 0u);
}

// V3 FUN_10024044
void trace_init(TraceBlock& t, uint32_t timer) {
    t = TraceBlock{};
    t.version = 1;
    t.boot_timer = timer;
    t.magic = kTraceMagic;  // written last
}

// V3 FUN_10024090, before the wrapped call
TraceEvent& trace_begin(TraceBlock& t, uint32_t timer, uint32_t flags) {
    const uint32_t n = t.head;
    TraceEvent& ev = n < kTraceRingA ? t.ring_a[n] : t.ring_b[n & (kTraceRingB - 1)];
    t.busy = n + 1;
    t.head = n + 1;
    ev.t_start = timer;
    ev.counter_after = 0xFFFFFFFFu;
    ev.flags_before = flags;
    ev.flags_after = 0xFFFFFFFFu;
    return ev;
}

// V3 FUN_10024090, after the wrapped call
void trace_end(TraceBlock& t, TraceEvent& ev, uint32_t counter, uint32_t flags) {
    ev.counter_after = counter;
    ev.flags_after = flags;
    t.done = t.head;
    t.busy = 0;
}

// V3 FUN_10024188, before the wrapped call
TraceIrqEvent* trace_irq_begin(TraceBlock& t, uint32_t timer, uint32_t aux, uint32_t flags, uint32_t uart,
                               uint32_t gpio) {
    const uint32_t n = t.irq_head;
    TraceIrqEvent* ev = nullptr;
    if (n < kTraceIrq) {
        ev = &t.irq[n];
        *ev = TraceIrqEvent{timer, aux, flags, 0xFFFFFFFFu, uart, gpio};
    } else {
        t.irq_overflow = n - 0xF;
    }
    t.irq_head = n + 1;
    return ev;
}

// V3 FUN_10024188, after the wrapped call
void trace_irq_end(TraceBlock& t, TraceIrqEvent* ev, uint32_t flags) {
    if (ev) ev->flags_after = flags;
    t.irq_done = t.irq_head;
}

}  // namespace mh::aux
