#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_SEQUENCERING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_SEQUENCERING_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

class SequenceRing {
public:
    using Work = std::function<void()>;
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    struct Held {
        std::uint64_t key = 0;
        std::uint64_t shape = 0;
        bool unknown = true;
        bool store = false;
        std::vector<Range> writes;
        std::uint64_t labelAddress = 0;
        std::vector<std::byte> labelBytes;
    };
    struct Options {
        std::size_t capacity = 32;
        std::chrono::nanoseconds spin{};
        void (*pause)() = nullptr;
        std::function<void()> preparerStart;
    };
    struct Counters {
        std::uint64_t pushed = 0;
        std::uint64_t bypassed = 0;
        std::uint64_t fullWaits = 0;
        std::uint64_t fullWaitNs = 0;
        std::uint64_t preparerSleeps = 0;
    };

    explicit SequenceRing(Options options);
    ~SequenceRing();
    SequenceRing(const SequenceRing&) = delete;
    SequenceRing& operator=(const SequenceRing&) = delete;

    std::uint64_t Push(Work work, Held held, bool holdsGpuLock = false);
    std::uint64_t Bypass();
    void Drain();
    void Settle();
    void RethrowFailure();
    void Stop();
    void Reclaim();

    bool Idle() const { return prepared.load(std::memory_order_acquire) == pushed.load(std::memory_order_acquire); }
    std::size_t Waiting() const { return static_cast<std::size_t>(pushed.load(std::memory_order_acquire) - prepared.load(std::memory_order_acquire)); }
    std::size_t Capacity() const { return slots.size(); }
    std::uint64_t Issued() const { return issued.load(std::memory_order_acquire); }
    std::uint64_t PreparedSequence() const { return preparedSequence.load(std::memory_order_acquire); }
    bool OnPreparer() const { return preparerId.load(std::memory_order_acquire) == std::this_thread::get_id(); }
    bool Producing() const { return producerId.load(std::memory_order_acquire) == std::this_thread::get_id(); }
    Counters TakeCounters();
    static std::uint64_t Running();

    template<typename TVisit>
    bool AnyNewestFirst(TVisit&& visit) const {
        const auto first = prepared.load(std::memory_order_acquire);
        for (auto index = pushed.load(std::memory_order_relaxed); index != first; --index) {
            if (visit(static_cast<const Held&>(slots[(index - 1) & mask].held))) return true;
        }
        return false;
    }

private:
    struct Slot {
        std::uint64_t sequence = 0;
        Work work;
        Held held;
    };
    void claimProducer();
    void run();
    void waitPrepared(std::uint64_t count);

    std::vector<Slot> slots;
    std::uint64_t mask = 0;
    Options options;
    alignas(64) std::atomic<std::uint64_t> pushed{0};
    std::atomic<std::uint64_t> issued{0};
    std::uint64_t reclaimed = 0;
    alignas(64) std::atomic<std::uint64_t> prepared{0};
    std::atomic<std::uint64_t> preparedSequence{0};
    alignas(64) std::atomic<bool> preparerSleeping{false};
    std::atomic<std::uint32_t> wakeSignal{0};
    std::atomic<std::uint32_t> preparedWaiters{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> started{false};
    std::atomic<std::thread::id> producerId{};
    std::atomic<std::thread::id> preparerId{};
    std::mutex failureMutex;
    std::exception_ptr failure;
    std::mutex threadMutex;
    std::thread thread;
    std::atomic<std::uint64_t> bypassed{0};
    std::atomic<std::uint64_t> fullWaits{0};
    std::atomic<std::uint64_t> fullWaitNs{0};
    std::atomic<std::uint64_t> preparerSleeps{0};
};

}

#endif
