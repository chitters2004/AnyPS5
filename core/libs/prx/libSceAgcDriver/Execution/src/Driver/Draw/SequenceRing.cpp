#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/SequenceRing.hpp"
#include <bit>
#include <stdexcept>

namespace AgcDriver::DriverDetail {

namespace {

thread_local std::uint64_t runningSequence = 0;

void yieldPause() {
    std::this_thread::yield();
}

template<typename TReady>
bool spinFor(std::chrono::nanoseconds window, void (*pause)(), TReady&& ready) {
    if (ready()) return true;
    if (window.count() <= 0) return false;
    const auto until = std::chrono::steady_clock::now() + window;
    do {
        pause();
        if (ready()) return true;
    } while (std::chrono::steady_clock::now() < until);
    return false;
}

}

SequenceRing::SequenceRing(Options configured) : options(std::move(configured)) {
    if (options.capacity < 2) throw std::invalid_argument("a sequence ring needs room for at least two items");
    if (options.pause == nullptr) options.pause = &yieldPause;
    slots.resize(std::bit_ceil(options.capacity));
    mask = slots.size() - 1;
}

SequenceRing::~SequenceRing() {
    Stop();
}

std::uint64_t SequenceRing::Running() {
    return runningSequence;
}

void SequenceRing::claimProducer() {
    const auto self = std::this_thread::get_id();
    auto current = producerId.load(std::memory_order_acquire);
    if (current == self) return;
    if (OnPreparer()) throw std::logic_error("the sequence ring's preparer produced into its own ring");
    if (current != std::thread::id{} && !Idle()) throw std::logic_error("a second thread produced into the sequence ring while the first thread's items were still waiting");
    if (!producerId.compare_exchange_strong(current, self, std::memory_order_acq_rel)) throw std::logic_error("two threads claimed the sequence ring at once");
}

void SequenceRing::RethrowFailure() {
    if (!failed.load(std::memory_order_acquire)) return;
    std::exception_ptr error;
    {
        std::lock_guard lock(failureMutex);
        error = std::exchange(failure, nullptr);
        failed.store(false, std::memory_order_release);
    }
    if (error != nullptr) std::rethrow_exception(error);
}

void SequenceRing::Reclaim() {
    const auto done = prepared.load(std::memory_order_acquire);
    for (; reclaimed < done; ++reclaimed) {
        auto& slot = slots[reclaimed & mask];
        slot.work = nullptr;
        slot.held = {};
    }
}

std::uint64_t SequenceRing::Push(Work work, Held held, bool holdsGpuLock, std::uint8_t tag) {
    if (tag >= Tags) throw std::invalid_argument("a sequence ring item tag is out of range");
    claimProducer();
    if (stopping.load(std::memory_order_acquire)) throw std::logic_error("an item was pushed into a stopped sequence ring");
    Reclaim();
    RethrowFailure();
    const auto index = pushed.load(std::memory_order_relaxed);
    if (index - prepared.load(std::memory_order_acquire) >= slots.size()) {
        if (holdsGpuLock) throw std::runtime_error("sequence ring full while the producer holds the GPU lock");
        const auto start = std::chrono::steady_clock::now();
        waitPrepared(index - slots.size() + 1);
        fullWaits.fetch_add(1, std::memory_order_relaxed);
        fullWaitNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
    }
    Reclaim();
    auto& slot = slots[index & mask];
    const auto sequence = issued.load(std::memory_order_relaxed) + 1;
    slot.sequence = sequence;
    slot.work = std::move(work);
    slot.held = std::move(held);
    slot.tag = tag;
    if (options.timed) slot.pushedAt = std::chrono::steady_clock::now();
    issued.store(sequence, std::memory_order_release);
    pushed.store(index + 1, std::memory_order_seq_cst);
    if (!started.load(std::memory_order_acquire)) {
        std::lock_guard lock(threadMutex);
        if (!started.load(std::memory_order_relaxed)) {
            thread = std::thread([this] { run(); });
            started.store(true, std::memory_order_release);
        }
    }
    if (preparerSleeping.load(std::memory_order_seq_cst)) {
        wakeSignal.fetch_add(1, std::memory_order_seq_cst);
        wakeSignal.notify_one();
    }
    return sequence;
}

std::uint64_t SequenceRing::Bypass() {
    claimProducer();
    if (!Idle()) throw std::logic_error("an item bypassed the sequence ring while earlier items were still waiting in it");
    const auto sequence = issued.load(std::memory_order_relaxed) + 1;
    issued.store(sequence, std::memory_order_release);
    bypassed.fetch_add(1, std::memory_order_relaxed);
    return sequence;
}

void SequenceRing::waitPrepared(std::uint64_t count) {
    if (spinFor(options.spin, options.pause, [&] { return prepared.load(std::memory_order_acquire) >= count; })) return;
    for (;;) {
        preparedWaiters.fetch_add(1, std::memory_order_seq_cst);
        const auto value = prepared.load(std::memory_order_seq_cst);
        if (value >= count) {
            preparedWaiters.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        prepared.wait(value, std::memory_order_acquire);
        preparedWaiters.fetch_sub(1, std::memory_order_relaxed);
    }
}

void SequenceRing::Drain() {
    if (OnPreparer()) return;
    waitPrepared(pushed.load(std::memory_order_acquire));
    if (Producing()) Reclaim();
    RethrowFailure();
}

void SequenceRing::Settle() {
    if (OnPreparer()) return;
    waitPrepared(pushed.load(std::memory_order_acquire));
}

void SequenceRing::Stop() {
    std::thread preparer;
    {
        std::lock_guard lock(threadMutex);
        stopping.store(true, std::memory_order_seq_cst);
        preparer = std::move(thread);
    }
    wakeSignal.fetch_add(1, std::memory_order_seq_cst);
    wakeSignal.notify_all();
    if (!preparer.joinable()) return;
    if (preparer.get_id() == std::this_thread::get_id()) preparer.detach();
    else preparer.join();
}

SequenceRing::Counters SequenceRing::TakeCounters() {
    Counters counters;
    counters.pushed = issued.load(std::memory_order_relaxed);
    counters.bypassed = bypassed.exchange(0, std::memory_order_relaxed);
    counters.fullWaits = fullWaits.exchange(0, std::memory_order_relaxed);
    counters.fullWaitNs = fullWaitNs.exchange(0, std::memory_order_relaxed);
    counters.preparerSleeps = preparerSleeps.exchange(0, std::memory_order_relaxed);
    counters.worked = worked.exchange(0, std::memory_order_relaxed);
    counters.busyCpuNs = busyCpuNs.exchange(0, std::memory_order_relaxed);
    counters.busyWallNs = busyWallNs.exchange(0, std::memory_order_relaxed);
    counters.idleNs = idleNs.exchange(0, std::memory_order_relaxed);
    for (std::size_t tag = 0; tag < Tags; ++tag) {
        counters.residency[tag].items = residency[tag].items.exchange(0, std::memory_order_relaxed);
        counters.residency[tag].totalNs = residency[tag].totalNs.exchange(0, std::memory_order_relaxed);
        counters.residency[tag].longestNs = residency[tag].longestNs.exchange(0, std::memory_order_relaxed);
    }
    return counters;
}

void SequenceRing::run() {
    preparerId.store(std::this_thread::get_id(), std::memory_order_release);
    if (options.preparerStart) options.preparerStart();
    const auto nanosSince = [](std::chrono::steady_clock::time_point since) { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count()); };
    for (;;) {
        const auto index = prepared.load(std::memory_order_relaxed);
        const auto ready = [&] { return pushed.load(std::memory_order_acquire) != index; };
        const auto idleSince = options.timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (!spinFor(options.spin, options.pause, ready)) {
            for (;;) {
                const auto seen = wakeSignal.load(std::memory_order_seq_cst);
                preparerSleeping.store(true, std::memory_order_seq_cst);
                if (pushed.load(std::memory_order_seq_cst) != index || stopping.load(std::memory_order_seq_cst)) break;
                preparerSleeps.fetch_add(1, std::memory_order_relaxed);
                wakeSignal.wait(seen, std::memory_order_seq_cst);
            }
            preparerSleeping.store(false, std::memory_order_relaxed);
            if (!ready()) {
                if (stopping.load(std::memory_order_acquire)) break;
                if (options.timed) idleNs.fetch_add(nanosSince(idleSince), std::memory_order_relaxed);
                continue;
            }
        }
        if (options.timed) idleNs.fetch_add(nanosSince(idleSince), std::memory_order_relaxed);
        auto& slot = slots[index & mask];
        const auto sequence = slot.sequence;
        if (!failed.load(std::memory_order_acquire)) {
            const auto busySince = options.timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const auto cpuSince = options.timed && options.threadCpu != nullptr ? options.threadCpu() : 0;
            runningSequence = sequence;
            try {
                slot.work();
            } catch (...) {
                const auto error = std::current_exception();
                bool first = false;
                {
                    std::lock_guard lock(failureMutex);
                    first = failure == nullptr;
                    if (first) failure = error;
                    failed.store(true, std::memory_order_release);
                }
                if (first && options.failed) options.failed(error);
            }
            runningSequence = 0;
            if (options.timed) {
                worked.fetch_add(1, std::memory_order_relaxed);
                busyWallNs.fetch_add(nanosSince(busySince), std::memory_order_relaxed);
                if (options.threadCpu != nullptr) busyCpuNs.fetch_add(options.threadCpu() - cpuSince, std::memory_order_relaxed);
            }
        }
        if (options.timed) {
            const auto resident = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - slot.pushedAt).count());
            auto& counters = residency[slot.tag];
            counters.items.fetch_add(1, std::memory_order_relaxed);
            counters.totalNs.fetch_add(resident, std::memory_order_relaxed);
            if (resident > counters.longestNs.load(std::memory_order_relaxed)) counters.longestNs.store(resident, std::memory_order_relaxed);
        }
        preparedSequence.store(sequence, std::memory_order_release);
        prepared.store(index + 1, std::memory_order_seq_cst);
        if (preparedWaiters.load(std::memory_order_seq_cst) != 0) prepared.notify_all();
    }
}

}
