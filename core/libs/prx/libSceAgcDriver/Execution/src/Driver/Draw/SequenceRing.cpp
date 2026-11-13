#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/SequenceRing.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::uint64_t NotVisiting = std::numeric_limits<std::uint64_t>::max();

thread_local std::uint64_t runningSequence = 0;
thread_local std::uint64_t runningIndex = 0;
thread_local const SequenceRing* runningRing = nullptr;
thread_local std::size_t runningPreparer = 0;

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

std::uint64_t nanosSince(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

}

SequenceRing::SequenceRing(Options configured) : options(std::move(configured)) {
    if (options.capacity < 2) throw std::invalid_argument("a sequence ring needs room for at least two items");
    if (options.preparers == 0) throw std::invalid_argument("a sequence ring needs at least one preparer");
    if (options.pause == nullptr) options.pause = &yieldPause;
    slots.resize(std::bit_ceil(options.capacity));
    mask = slots.size() - 1;
    if (options.preparers > 1) {
        visitFrom = std::make_unique<std::atomic<std::uint64_t>[]>(options.preparers);
        for (std::size_t preparer = 0; preparer < options.preparers; ++preparer) visitFrom[preparer].store(NotVisiting, std::memory_order_relaxed);
        doneMarks = std::make_unique<std::atomic<std::uint64_t>[]>(slots.size());
        for (std::size_t slot = 0; slot < slots.size(); ++slot) doneMarks[slot].store(0, std::memory_order_relaxed);
    }
}

SequenceRing::~SequenceRing() {
    Stop();
}

std::uint64_t SequenceRing::Running() {
    return runningSequence;
}

bool SequenceRing::OnPreparer() const {
    return runningRing == this;
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
    auto done = prepared.load(std::memory_order_acquire);
    if (options.preparers > 1) {
        announced.store(done, std::memory_order_seq_cst);
        for (std::size_t preparer = 0; preparer < options.preparers; ++preparer) done = std::min(done, visitFrom[preparer].load(std::memory_order_seq_cst));
    }
    for (; reclaimed < done; ++reclaimed) {
        auto& slot = slots[reclaimed & mask];
        slot.work = nullptr;
        slot.held = {};
    }
}

std::pair<std::uint64_t, std::uint64_t> SequenceRing::beginOlderVisit() const {
    if (!OnPreparer()) return {0, 0};
    const auto index = runningIndex;
    if (options.preparers == 1) return {index, index};
    const auto first = prepared.load(std::memory_order_acquire);
    if (first >= index) return {index, index};
    visitFrom[runningPreparer].store(first, std::memory_order_seq_cst);
    const auto start = std::max(first, announced.load(std::memory_order_seq_cst));
    if (start >= index) {
        visitFrom[runningPreparer].store(NotVisiting, std::memory_order_release);
        return {index, index};
    }
    return {start, index};
}

void SequenceRing::endOlderVisit() const {
    visitFrom[runningPreparer].store(NotVisiting, std::memory_order_release);
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
        fullWaitNs.fetch_add(nanosSince(start), std::memory_order_relaxed);
    }
    Reclaim();
    while (index >= slots.size() && reclaimed <= index - slots.size()) {
        options.pause();
        Reclaim();
    }
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
            threads.reserve(options.preparers);
            for (std::size_t preparer = 0; preparer < options.preparers; ++preparer) threads.emplace_back([this, preparer] { run(preparer); });
            started.store(true, std::memory_order_release);
        }
    }
    if (preparerSleepers.load(std::memory_order_seq_cst) != 0) {
        wakeSignal.fetch_add(1, std::memory_order_seq_cst);
        if (options.preparers == 1) wakeSignal.notify_one();
        else wakeSignal.notify_all();
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

void SequenceRing::waitTurn(std::uint64_t index) {
    if (prepared.load(std::memory_order_acquire) == index) return;
    const auto start = options.timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    waitPrepared(index);
    if (options.timed) {
        turnWaits.fetch_add(1, std::memory_order_relaxed);
        turnWaitNs.fetch_add(nanosSince(start), std::memory_order_relaxed);
    }
}

void SequenceRing::AwaitTurn() {
    if (!OnPreparer()) throw std::logic_error("only a sequence ring preparer can wait for its item's turn");
    waitTurn(runningIndex);
    if (failed.load(std::memory_order_acquire) || runningIndex < cancelBelow.load(std::memory_order_acquire)) throw Cancelled();
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
    std::vector<std::thread> preparers;
    {
        std::lock_guard lock(threadMutex);
        stopping.store(true, std::memory_order_seq_cst);
        preparers = std::move(threads);
    }
    wakeSignal.fetch_add(1, std::memory_order_seq_cst);
    wakeSignal.notify_all();
    for (auto& preparer : preparers) {
        if (!preparer.joinable()) continue;
        if (preparer.get_id() == std::this_thread::get_id()) preparer.detach();
        else preparer.join();
    }
}

SequenceRing::Counters SequenceRing::TakeCounters() {
    Counters counters;
    counters.pushed = issued.load(std::memory_order_relaxed);
    counters.bypassed = bypassed.exchange(0, std::memory_order_relaxed);
    counters.fullWaits = fullWaits.exchange(0, std::memory_order_relaxed);
    counters.fullWaitNs = fullWaitNs.exchange(0, std::memory_order_relaxed);
    counters.preparerSleeps = preparerSleeps.exchange(0, std::memory_order_relaxed);
    counters.idleNs = idleNs.exchange(0, std::memory_order_relaxed);
    const auto totals = CurrentTotals();
    {
        std::lock_guard lock(takeMutex);
        counters.worked = totals.worked - taken.worked;
        counters.busyCpuNs = totals.busyCpuNs - taken.busyCpuNs;
        counters.busyWallNs = totals.busyWallNs - taken.busyWallNs;
        counters.turnWaits = totals.turnWaits - taken.turnWaits;
        counters.turnWaitNs = totals.turnWaitNs - taken.turnWaitNs;
        counters.publishedForOthers = totals.publishedForOthers - taken.publishedForOthers;
        taken = totals;
    }
    for (std::size_t tag = 0; tag < Tags; ++tag) {
        counters.residency[tag].items = residency[tag].items.exchange(0, std::memory_order_relaxed);
        counters.residency[tag].totalNs = residency[tag].totalNs.exchange(0, std::memory_order_relaxed);
        counters.residency[tag].longestNs = residency[tag].longestNs.exchange(0, std::memory_order_relaxed);
    }
    return counters;
}

SequenceRing::Totals SequenceRing::CurrentTotals() const {
    Totals totals;
    totals.issued = issued.load(std::memory_order_acquire);
    totals.pushed = pushed.load(std::memory_order_acquire);
    totals.worked = worked.load(std::memory_order_relaxed);
    totals.busyCpuNs = busyCpuNs.load(std::memory_order_relaxed);
    totals.busyWallNs = busyWallNs.load(std::memory_order_relaxed);
    totals.turnWaits = turnWaits.load(std::memory_order_relaxed);
    totals.turnWaitNs = turnWaitNs.load(std::memory_order_relaxed);
    totals.publishedForOthers = publishedForOthers.load(std::memory_order_relaxed);
    return totals;
}

void SequenceRing::publishReady() {
    for (;;) {
        if (publishing.exchange(true, std::memory_order_seq_cst)) return;
        for (;;) {
            const auto index = prepared.load(std::memory_order_relaxed);
            if (doneMarks[index & mask].load(std::memory_order_acquire) != index + 1) break;
            if (index != runningIndex) publishedForOthers.fetch_add(1, std::memory_order_relaxed);
            finishItem(index);
        }
        publishing.store(false, std::memory_order_seq_cst);
        const auto index = prepared.load(std::memory_order_seq_cst);
        if (doneMarks[index & mask].load(std::memory_order_seq_cst) != index + 1) return;
    }
}

void SequenceRing::finishItem(std::uint64_t index) {
    auto& slot = slots[index & mask];
    const auto sequence = slot.sequence;
    auto error = std::exchange(slot.error, nullptr);
    const bool cancelled = failed.load(std::memory_order_acquire) || index < cancelBelow.load(std::memory_order_acquire);
    if (slot.publish) {
        const auto running = std::exchange(runningSequence, sequence);
        try {
            slot.publish(!cancelled);
        } catch (...) {
            if (!cancelled && error == nullptr) error = std::current_exception();
        }
        runningSequence = running;
        slot.publish = nullptr;
    }
    if (error != nullptr && !cancelled) {
        bool cancellation = false;
        try {
            std::rethrow_exception(error);
        } catch (const Cancelled&) {
            cancellation = true;
        } catch (...) {
        }
        if (!cancellation) {
            bool first = false;
            {
                std::lock_guard lock(failureMutex);
                first = failure == nullptr;
                if (first) failure = error;
                failed.store(true, std::memory_order_seq_cst);
                cancelBelow.store(std::max(cancelBelow.load(std::memory_order_relaxed), claimed.load(std::memory_order_seq_cst)), std::memory_order_release);
            }
            if (first && options.failed) options.failed(error);
        }
    }
    if (options.timed) {
        const auto resident = nanosSince(slot.pushedAt);
        auto& counters = residency[slot.tag];
        counters.items.fetch_add(1, std::memory_order_relaxed);
        counters.totalNs.fetch_add(resident, std::memory_order_relaxed);
        if (resident > counters.longestNs.load(std::memory_order_relaxed)) counters.longestNs.store(resident, std::memory_order_relaxed);
    }
    preparedSequence.store(sequence, std::memory_order_release);
    prepared.store(index + 1, std::memory_order_seq_cst);
    if (preparedWaiters.load(std::memory_order_seq_cst) != 0) prepared.notify_all();
}

bool SequenceRing::claimable() const {
    return pushed.load(std::memory_order_acquire) != claimed.load(std::memory_order_acquire);
}

void SequenceRing::run(std::size_t preparer) {
    runningRing = this;
    runningPreparer = preparer;
    if (options.preparerStart) options.preparerStart();
    for (;;) {
        if (preparer != 0 && options.single != nullptr && options.single()) {
            if (stopping.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const auto idleSince = options.timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (!spinFor(options.spin, options.pause, [&] { return claimable(); })) {
            for (;;) {
                const auto seen = wakeSignal.load(std::memory_order_seq_cst);
                preparerSleepers.fetch_add(1, std::memory_order_seq_cst);
                if (claimable() || stopping.load(std::memory_order_seq_cst)) {
                    preparerSleepers.fetch_sub(1, std::memory_order_seq_cst);
                    break;
                }
                preparerSleeps.fetch_add(1, std::memory_order_relaxed);
                wakeSignal.wait(seen, std::memory_order_seq_cst);
                preparerSleepers.fetch_sub(1, std::memory_order_seq_cst);
                if (preparer != 0 && options.single != nullptr && options.single()) break;
            }
            if (preparer != 0 && options.single != nullptr && options.single()) {
                wakeSignal.fetch_add(1, std::memory_order_seq_cst);
                wakeSignal.notify_all();
                continue;
            }
            if (!claimable()) {
                if (stopping.load(std::memory_order_acquire)) break;
                if (options.timed) idleNs.fetch_add(nanosSince(idleSince), std::memory_order_relaxed);
                continue;
            }
        }
        auto index = claimed.load(std::memory_order_acquire);
        if (index == pushed.load(std::memory_order_acquire) || !claimed.compare_exchange_strong(index, index + 1, std::memory_order_seq_cst)) continue;
        if (options.timed) idleNs.fetch_add(nanosSince(idleSince), std::memory_order_relaxed);
        if (options.preparers > 1 && claimable() && preparerSleepers.load(std::memory_order_seq_cst) != 0) {
            wakeSignal.fetch_add(1, std::memory_order_seq_cst);
            wakeSignal.notify_all();
        }
        auto& slot = slots[index & mask];
        runningIndex = index;
        runningSequence = slot.sequence;
        std::exception_ptr error;
        if (!failed.load(std::memory_order_seq_cst) && index >= cancelBelow.load(std::memory_order_acquire)) {
            const auto busySince = options.timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const auto cpuSince = options.timed && options.threadCpu != nullptr ? options.threadCpu() : 0;
            try {
                slot.work();
            } catch (...) {
                error = std::current_exception();
            }
            if (options.timed) {
                worked.fetch_add(1, std::memory_order_relaxed);
                busyWallNs.fetch_add(nanosSince(busySince), std::memory_order_relaxed);
                if (options.threadCpu != nullptr) busyCpuNs.fetch_add(options.threadCpu() - cpuSince, std::memory_order_relaxed);
            }
        }
        runningSequence = 0;
        slot.error = std::move(error);
        if (options.outbox) slot.publish = options.outbox();
        if (doneMarks == nullptr) {
            finishItem(index);
            continue;
        }
        doneMarks[index & mask].store(index + 1, std::memory_order_seq_cst);
        publishReady();
    }
    runningRing = nullptr;
}

}
