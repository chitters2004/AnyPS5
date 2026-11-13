#include "prx/libkernel/Time/include/SleepWake.hpp"
#include "prx/libkernel/Time/include/TimedWait.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"

#include <atomic>
#include <cstdlib>

namespace SleepWake {

namespace {

// A thread's sleep run state, packed into one word so it lives in a HostThreadSlot (a TEB read on Windows x64,
// native thread_local elsewhere). The old thread_local struct cost one __emutls_get_address call (about 12 ns)
// per field access on MinGW, and Sleep touched it six or seven times.
//   bit 0       sleeping
//   bits 1..4   earlyWakes (at most EARLY_WAKES_PER_RUN, so 4 bits are enough)
//   bits 5..63  lastSleepEndNanos, a monotonic clock since boot, good for 2^59 ns (18 years)
struct ThreadState {
    unsigned earlyWakes = 0;
    std::uint64_t lastSleepEndNanos = 0;
    bool sleeping = false;

    static constexpr unsigned EARLY_SHIFT = 1;
    static constexpr unsigned NANOS_SHIFT = 5;
    static_assert(EARLY_WAKES_PER_RUN < (1u << (NANOS_SHIFT - EARLY_SHIFT)));

    static ThreadState Unpack(std::uint64_t bits) {
        ThreadState state;
        state.sleeping = (bits & 1u) != 0;
        state.earlyWakes = static_cast<unsigned>((bits >> EARLY_SHIFT) & ((1u << (NANOS_SHIFT - EARLY_SHIFT)) - 1u));
        state.lastSleepEndNanos = bits >> NANOS_SHIFT;
        return state;
    }

    std::uint64_t Pack() const {
        return (lastSleepEndNanos << NANOS_SHIFT) | (static_cast<std::uint64_t>(earlyWakes) << EARLY_SHIFT) | (sleeping ? 1u : 0u);
    }
};

struct StateTag {};
using StateSlot = HostThreadSlot<std::uint64_t, StateTag>;

ThreadState Load() {
    return ThreadState::Unpack(StateSlot::Get());
}

void Store(const ThreadState& state) {
    StateSlot::Set(state.Pack());
}

std::atomic<std::uint64_t> completions{0};

bool Enabled() {
    static const bool enabled = std::getenv("APS5_NO_SLEEP_WAKE") == nullptr;
    return enabled;
}

}

void Sleep(std::uint64_t nanos) {
    ThreadState state = Load();
    bool woken = false;
    if (nanos != 0 && nanos <= MAX_WAKEABLE_NANOS && !state.sleeping && Enabled()) {
        if (TimedWait::NowNanos() - state.lastSleepEndNanos >= nanos) state.earlyWakes = 0;
        if (state.earlyWakes < EARLY_WAKES_PER_RUN) {
            state.sleeping = true;
            Store(state);
            const bool early = TimedWait::SleepNanosUnlessChanged(nanos, completions, completions.load(std::memory_order_seq_cst));
            // A handler that ran on this thread during the sleep may have ended the run, so merge into the current word.
            state = Load();
            state.sleeping = false;
            if (early) ++state.earlyWakes;
            Store(state);
            woken = true;
        }
    }
    if (!woken) TimedWait::SleepNanos(nanos);
    state = Load();
    state.lastSleepEndNanos = TimedWait::NowNanos();
    Store(state);
}

}

extern "C" {

void KernelWakeSleepers_nid_no_patch() {
    SleepWake::completions.fetch_add(1, std::memory_order_seq_cst);
    TimedWait::WakeChangeSleepers();
}

void KernelNoteGpuSubmit_nid_no_patch() {
    SleepWake::ThreadState state = SleepWake::Load();
    state.earlyWakes = 0;
    SleepWake::Store(state);
}

}
