#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_THREADTALLY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_THREADTALLY_HPP

#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libSceAgcDriver/Execution/include/ThreadScratch.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace AgcDriver {

template<std::size_t N, typename Tag>
class ThreadTally {
public:
    using Counts = std::array<std::uint64_t, N>;

    static void Add(std::size_t index, std::uint64_t amount = 1) {
        auto& slot = ThreadScratch<Thread, Tag>().counts[index];
        slot.store(slot.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
    }

    static Counts Fold() {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        Counts totals = registry.retired;
        for (const auto* thread : registry.threads) {
            for (std::size_t i = 0; i < N; ++i) totals[i] += thread->counts[i].load(std::memory_order_relaxed);
        }
        return totals;
    }

private:
    struct Thread;

    struct Shared {
        AgcDriver::Mutex mutex;
        std::vector<const Thread*> threads;
        Counts retired{};
    };

    static Shared& Registry() {
        static Shared& shared = *new Shared();
        return shared;
    }

    struct Thread {
        std::array<std::atomic<std::uint64_t>, N> counts{};

        Thread() {
            auto& registry = Registry();
            std::lock_guard lock(registry.mutex);
            registry.threads.push_back(this);
        }
        ~Thread() {
            auto& registry = Registry();
            std::lock_guard lock(registry.mutex);
            registry.threads.erase(std::remove(registry.threads.begin(), registry.threads.end(), this), registry.threads.end());
            for (std::size_t i = 0; i < N; ++i) registry.retired[i] += counts[i].load(std::memory_order_relaxed);
        }
        Thread(const Thread&) = delete;
        Thread& operator=(const Thread&) = delete;
    };
};

}

#endif
