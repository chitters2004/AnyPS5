#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_SEQUENCERING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_SEQUENCERING_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

class SequenceRing {
public:
    using Work = std::function<void()>;
    using Publish = std::function<void(bool)>;
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
        std::function<void(std::exception_ptr)> failed;
        bool timed = false;
        std::uint64_t (*threadCpu)() = nullptr;
        std::size_t preparers = 1;
        bool (*single)() = nullptr;
        std::function<Publish()> outbox;
    };
    class Cancelled : public std::runtime_error {
    public:
        Cancelled() : std::runtime_error("a sequence ring item was cancelled by an earlier item's failure") {}
    };
    static constexpr std::size_t Tags = 32;
    struct Residency {
        std::uint64_t items = 0;
        std::uint64_t totalNs = 0;
        std::uint64_t longestNs = 0;
    };
    struct Counters {
        std::uint64_t pushed = 0;
        std::uint64_t bypassed = 0;
        std::uint64_t fullWaits = 0;
        std::uint64_t fullWaitNs = 0;
        std::uint64_t preparerSleeps = 0;
        std::uint64_t worked = 0;
        std::uint64_t busyCpuNs = 0;
        std::uint64_t busyWallNs = 0;
        std::uint64_t idleNs = 0;
        std::uint64_t turnWaits = 0;
        std::uint64_t turnWaitNs = 0;
        std::uint64_t publishedForOthers = 0;
        std::array<Residency, Tags> residency{};
    };

    struct Totals {
        std::uint64_t issued = 0;
        std::uint64_t pushed = 0;
        std::uint64_t worked = 0;
        std::uint64_t busyCpuNs = 0;
        std::uint64_t busyWallNs = 0;
        std::uint64_t turnWaits = 0;
        std::uint64_t turnWaitNs = 0;
        std::uint64_t publishedForOthers = 0;
    };

    explicit SequenceRing(Options options);
    ~SequenceRing();
    SequenceRing(const SequenceRing&) = delete;
    SequenceRing& operator=(const SequenceRing&) = delete;

    std::uint64_t Push(Work work, Held held, bool holdsGpuLock = false, std::uint8_t tag = 0);
    std::uint64_t Bypass();
    void Drain();
    void Settle();
    void RethrowFailure();
    void Stop();
    void Reclaim();
    void AwaitTurn();

    bool Idle() const { return prepared.load(std::memory_order_acquire) == pushed.load(std::memory_order_acquire); }
    std::size_t Waiting() const { return static_cast<std::size_t>(pushed.load(std::memory_order_acquire) - prepared.load(std::memory_order_acquire)); }
    std::size_t Capacity() const { return slots.size(); }
    std::size_t Preparers() const { return options.preparers; }
    std::uint64_t Issued() const { return issued.load(std::memory_order_acquire); }
    std::uint64_t PreparedSequence() const { return preparedSequence.load(std::memory_order_acquire); }
    bool OnPreparer() const;
    bool Producing() const { return producerId.load(std::memory_order_acquire) == std::this_thread::get_id(); }
    Counters TakeCounters();
    Totals CurrentTotals() const;
    static std::uint64_t Running();

    template<typename TVisit>
    bool AnyNewestFirst(TVisit&& visit) const {
        const auto first = prepared.load(std::memory_order_acquire);
        for (auto index = pushed.load(std::memory_order_relaxed); index != first; --index) {
            if (visit(static_cast<const Held&>(slots[(index - 1) & mask].held))) return true;
        }
        return false;
    }

    template<typename TVisit>
    bool AnyOlderNewestFirst(TVisit&& visit) const {
        const auto window = beginOlderVisit();
        if (window.first == window.second) return false;
        struct EndVisit {
            const SequenceRing& ring;
            ~EndVisit() { ring.endOlderVisit(); }
        } end{*this};
        for (auto index = window.second; index != window.first; --index) {
            if (visit(static_cast<const Held&>(slots[(index - 1) & mask].held))) return true;
        }
        return false;
    }

private:
    struct Slot {
        std::uint64_t sequence = 0;
        Work work;
        Held held;
        std::uint8_t tag = 0;
        std::chrono::steady_clock::time_point pushedAt{};
        Publish publish;
        std::exception_ptr error;
    };
    struct ResidencyCounters {
        std::atomic<std::uint64_t> items{0};
        std::atomic<std::uint64_t> totalNs{0};
        std::atomic<std::uint64_t> longestNs{0};
    };
    void claimProducer();
    void run(std::size_t preparer);
    void waitPrepared(std::uint64_t count);
    void waitTurn(std::uint64_t index);
    void publishReady();
    void finishItem(std::uint64_t index);
    bool claimable() const;
    std::pair<std::uint64_t, std::uint64_t> beginOlderVisit() const;
    void endOlderVisit() const;

    std::vector<Slot> slots;
    std::uint64_t mask = 0;
    Options options;
    alignas(64) std::atomic<std::uint64_t> pushed{0};
    std::atomic<std::uint64_t> issued{0};
    std::uint64_t reclaimed = 0;
    alignas(64) std::atomic<std::uint64_t> claimed{0};
    alignas(64) std::atomic<std::uint64_t> prepared{0};
    std::atomic<std::uint64_t> preparedSequence{0};
    alignas(64) std::atomic<std::uint32_t> preparerSleepers{0};
    std::atomic<std::uint32_t> wakeSignal{0};
    std::atomic<std::uint32_t> preparedWaiters{0};
    std::atomic<bool> failed{false};
    std::atomic<std::uint64_t> cancelBelow{0};
    std::atomic<bool> stopping{false};
    std::atomic<bool> started{false};
    std::atomic<std::thread::id> producerId{};
    std::atomic<std::uint64_t> announced{0};
    std::unique_ptr<std::atomic<std::uint64_t>[]> visitFrom;
    std::unique_ptr<std::atomic<std::uint64_t>[]> doneMarks;
    alignas(64) std::atomic<bool> publishing{false};
    std::mutex failureMutex;
    std::exception_ptr failure;
    std::mutex threadMutex;
    std::vector<std::thread> threads;
    std::atomic<std::uint64_t> bypassed{0};
    std::atomic<std::uint64_t> fullWaits{0};
    std::atomic<std::uint64_t> fullWaitNs{0};
    std::atomic<std::uint64_t> preparerSleeps{0};
    std::atomic<std::uint64_t> worked{0};
    std::atomic<std::uint64_t> busyCpuNs{0};
    std::atomic<std::uint64_t> busyWallNs{0};
    std::atomic<std::uint64_t> idleNs{0};
    std::atomic<std::uint64_t> turnWaits{0};
    std::atomic<std::uint64_t> turnWaitNs{0};
    std::atomic<std::uint64_t> publishedForOthers{0};
    std::array<ResidencyCounters, Tags> residency{};
    std::mutex takeMutex;
    Totals taken;
};

}

#endif
