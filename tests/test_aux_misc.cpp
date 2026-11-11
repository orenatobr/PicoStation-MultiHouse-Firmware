// Si5351 boot sequence (V3 FUN_10025108) and event trace (V3 FUN_10024044/10024090/10024188).
#include <vector>

#include "check.h"
#include "mh/aux_clock.h"
#include "mh/aux_trace.h"

using namespace mh::aux;

namespace {

struct Bus {
    std::vector<std::pair<uint8_t, uint8_t>> writes;
    uint32_t now = 0;
    uint32_t reg0_polls_until_ready = 0;  // reads returning "busy" bits before the status clears
    uint8_t busy_bits = 0xFF;
    int reads = 0;
};

bool wr(void* c, uint8_t r, uint8_t v) {
    auto* b = static_cast<Bus*>(c);
    b->writes.push_back({r, v});
    return true;
}
bool rdreg(void* c, uint8_t, uint8_t* v) {
    auto* b = static_cast<Bus*>(c);
    ++b->reads;
    *v = b->reg0_polls_until_ready ? (--b->reg0_polls_until_ready, b->busy_bits) : 0;
    return true;
}
uint32_t nowf(void* c) { return static_cast<Bus*>(c)->now += 10; }

void test_clock() {
    CHECK_EQ(kClockTable[0][0], 0x03);
    CHECK_EQ(kClockTable[0][1], 0xFF);
    CHECK_EQ(kClockTable[48][0], 0x03);
    CHECK_EQ(kClockTable[48][1], 0xF9);  // CLK1 and CLK2 enabled, like (1<<1) | (1<<2) in upstream
    Bus b;
    ClockBus bus{wr, rdreg, nowf, &b};
    ClockResult r = clock_program(bus);
    CHECK_EQ(r.status, 2u);
    CHECK_EQ(r.writes, kClockTableLen);
    CHECK_EQ(b.writes.size(), size_t{kClockTableLen});
    CHECK_EQ(b.reads, 2);  // SYS_INIT wait, lock wait
    CHECK_EQ(r.last_reg, 3);
    CHECK_EQ(r.last_value, 0xF9);

    // lock wait: slow lock still succeeds; the last write only happens after it
    Bus slow;
    slow.reg0_polls_until_ready = 3;  // SYS_INIT clears after 3 reads
    ClockBus bus2{wr, rdreg, nowf, &slow};
    CHECK_EQ(clock_program(bus2).status, 2u);
    CHECK_EQ(slow.reads, 4 + 1);

    // never ready: SYS_INIT timeout, nothing is written (error 7)
    Bus dead;
    dead.reg0_polls_until_ready = 1u << 30;
    ClockBus bus3{wr, rdreg, nowf, &dead};
    r = clock_program(bus3);
    CHECK_EQ(r.status, 3u);
    CHECK_EQ(r.error, 7u);
    CHECK(dead.writes.empty());
    CHECK(dead.now >= kClockStatusTimeoutUs);

    // lock never arrives: 48 entries written, then error 8 and no output enable
    struct Lock {
        Bus b;
    } l;
    l.b.busy_bits = 0x20;
    ClockBus bus4{
        wr,
        [](void* c, uint8_t, uint8_t* v) {
            auto* bb = static_cast<Bus*>(c);
            *v = bb->writes.size() >= 48 ? 0x20 : 0;  // LOL_A stays set after the PLL reset
            return true;
        },
        nowf, &l.b};
    r = clock_program(bus4);
    CHECK_EQ(r.status, 3u);
    CHECK_EQ(r.error, 8u);
    CHECK_EQ(l.b.writes.size(), size_t{48});
    CHECK(l.b.writes.back().first != 0x03 || l.b.writes.back().second != 0xF9);
}

void test_trace() {
    TraceFlagsIn f;
    CHECK_EQ(trace_flags(f), 0u);
    f.global_93ec = true;
    f.global_8820 = 3;
    f.byte08 = 0x7E;
    f.byte1a = f.byte15 = f.byte20 = true;
    CHECK_EQ(trace_flags(f), 0x100u | 3u << 16 | 0x7Eu | 0x200u | 0x400u | 0x800u);

    static TraceBlock t;
    trace_init(t, 1234);
    CHECK_EQ(t.magic, 0x31305442u);
    CHECK_EQ(t.version, 1u);
    CHECK_EQ(t.boot_timer, 1234u);
    TraceEvent& e = trace_begin(t, 50, 0x11);
    CHECK_EQ(t.head, 1u);
    CHECK_EQ(t.busy, 1u);
    CHECK(&e == &t.ring_a[0]);
    CHECK_EQ(e.counter_after, 0xFFFFFFFFu);
    trace_end(t, e, 99, 0x22);
    CHECK_EQ(t.done, 1u);
    CHECK_EQ(t.busy, 0u);
    CHECK_EQ(e.counter_after, 99u);
    CHECK_EQ(e.flags_after, 0x22u);
    for (int i = 1; i < 128; ++i) trace_end(t, trace_begin(t, i, 0), 0, 0);
    CHECK(&trace_begin(t, 0, 0) == &t.ring_b[0]);  // 129th event switches to the 64 entry ring
    trace_end(t, t.ring_b[0], 0, 0);
    for (int i = 129; i < 192; ++i) trace_end(t, trace_begin(t, i, 0), 0, 0);
    CHECK(&trace_begin(t, 0, 0) == &t.ring_b[0]);  // wraps at 64

    // IRQ table: 16 slots, then only the overflow counter moves
    TraceIrqEvent* last = nullptr;
    for (uint32_t i = 0; i < 16; ++i) {
        last = trace_irq_begin(t, i, 7, 1, 2, 3);
        CHECK(last == &t.irq[i]);
        trace_irq_end(t, last, 5);
    }
    CHECK_EQ(t.irq[15].flags_after, 5u);
    CHECK_EQ(t.irq_done, 16u);
    CHECK_EQ(t.irq_overflow, 0u);
    CHECK(trace_irq_begin(t, 0, 0, 0, 0, 0) == nullptr);
    CHECK_EQ(t.irq_overflow, 1u);
    trace_irq_end(t, nullptr, 0);
    CHECK(trace_irq_begin(t, 0, 0, 0, 0, 0) == nullptr);
    CHECK_EQ(t.irq_overflow, 2u);
    CHECK_EQ(t.irq[3].uart_flags, 2u);
    CHECK_EQ(t.irq[3].gpio_ctrl, 3u);
}

}  // namespace

int main() {
    test_clock();
    test_trace();
    TEST_MAIN_END();
}
