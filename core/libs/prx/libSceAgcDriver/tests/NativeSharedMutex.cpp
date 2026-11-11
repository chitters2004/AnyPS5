#include "prx/libc/include/NativeSharedMutex.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using namespace std::chrono_literals;

int failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

bool WaitFor(const std::atomic<bool>& flag, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

static_assert(!std::is_copy_constructible_v<NativeSharedMutex> && !std::is_copy_assignable_v<NativeSharedMutex>);
static_assert(std::is_default_constructible_v<NativeSharedMutex>);
static_assert(std::is_same_v<decltype(std::declval<NativeSharedMutex&>().try_lock()), bool>);
static_assert(std::is_same_v<decltype(std::declval<NativeSharedMutex&>().try_lock_shared()), bool>);

void ExclusiveCounts(int threads, int iterations) {
    NativeSharedMutex mutex;
    std::uint64_t counter = 0;
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < iterations; ++i) {
                std::unique_lock lock(mutex);
                const auto seen = counter;
                std::this_thread::yield();
                counter = seen + 1;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    Check(counter == static_cast<std::uint64_t>(threads) * static_cast<std::uint64_t>(iterations), "every increment under the exclusive lock counted");
}

void TryLocks() {
    NativeSharedMutex mutex;
    Check(mutex.try_lock(), "try_lock takes a free lock");
    bool exclusive = true;
    bool shared = true;
    std::thread other([&] {
        exclusive = mutex.try_lock();
        if (exclusive) mutex.unlock();
        shared = mutex.try_lock_shared();
        if (shared) mutex.unlock_shared();
    });
    other.join();
    Check(!exclusive, "try_lock fails while another thread holds the lock exclusively");
    Check(!shared, "try_lock_shared fails while another thread holds the lock exclusively");
    mutex.unlock();
    Check(mutex.try_lock_shared(), "try_lock_shared takes a free lock");
    other = std::thread([&] {
        exclusive = mutex.try_lock();
        if (exclusive) mutex.unlock();
        shared = mutex.try_lock_shared();
        if (shared) mutex.unlock_shared();
    });
    other.join();
    Check(!exclusive, "try_lock fails while another thread holds the lock shared");
    Check(shared, "try_lock_shared succeeds while another thread holds the lock shared");
    mutex.unlock_shared();
    other = std::thread([&] {
        exclusive = mutex.try_lock();
        if (exclusive) mutex.unlock();
    });
    other.join();
    Check(exclusive, "try_lock takes the lock once every reader left");
}

void ReadersOverlap() {
    NativeSharedMutex mutex;
    std::atomic<bool> firstInside{false};
    std::atomic<bool> secondInside{false};
    bool firstSaw = false;
    bool secondSaw = false;
    std::thread first([&] {
        std::shared_lock lock(mutex);
        firstInside.store(true, std::memory_order_release);
        firstSaw = WaitFor(secondInside, 5000ms);
    });
    std::thread second([&] {
        std::shared_lock lock(mutex);
        secondInside.store(true, std::memory_order_release);
        secondSaw = WaitFor(firstInside, 5000ms);
    });
    first.join();
    second.join();
    Check(firstSaw && secondSaw, "two readers hold the lock at the same time");
}

void WriterWaitsForReader() {
    NativeSharedMutex mutex;
    mutex.lock_shared();
    std::atomic<bool> started{false};
    std::atomic<bool> acquired{false};
    std::thread writer([&] {
        started.store(true, std::memory_order_release);
        std::unique_lock lock(mutex);
        acquired.store(true, std::memory_order_release);
    });
    WaitFor(started, 5000ms);
    std::this_thread::sleep_for(50ms);
    Check(!acquired.load(), "lock() waits while a reader holds the lock");
    mutex.unlock_shared();
    writer.join();
    Check(acquired.load(), "the waiting writer took the lock after the reader left");
}

void ReaderWaitsForWriter() {
    NativeSharedMutex mutex;
    mutex.lock();
    std::atomic<bool> started{false};
    std::atomic<bool> acquired{false};
    std::thread reader([&] {
        started.store(true, std::memory_order_release);
        std::shared_lock lock(mutex);
        acquired.store(true, std::memory_order_release);
    });
    WaitFor(started, 5000ms);
    std::this_thread::sleep_for(50ms);
    Check(!acquired.load(), "lock_shared() waits while a writer holds the lock");
    mutex.unlock();
    reader.join();
    Check(acquired.load(), "the waiting reader took the lock after the writer left");
}

void ReadersAndWriters(int readers, int writers, int iterations) {
    NativeSharedMutex mutex;
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::atomic<int> readersInside{0};
    std::atomic<int> writersInside{0};
    std::atomic<int> writersLeft{writers};
    std::atomic<bool> broken{false};
    std::atomic<std::uint64_t> reads{0};
    std::vector<std::thread> threads;
    for (int w = 0; w < writers; ++w) {
        threads.emplace_back([&] {
            for (int i = 0; i < iterations; ++i) {
                std::unique_lock lock(mutex);
                if (writersInside.fetch_add(1) != 0 || readersInside.load() != 0) broken.store(true);
                ++first;
                if ((i & 63) == 0) std::this_thread::yield();
                ++second;
                writersInside.fetch_sub(1);
            }
            writersLeft.fetch_sub(1);
        });
    }
    for (int r = 0; r < readers; ++r) {
        threads.emplace_back([&] {
            for (int i = 0; i < iterations && writersLeft.load() != 0; ++i) {
                {
                    std::shared_lock lock(mutex);
                    readersInside.fetch_add(1);
                    if (writersInside.load() != 0 || first != second) broken.store(true);
                    readersInside.fetch_sub(1);
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
                if ((i & 15) == 0) std::this_thread::yield();
            }
        });
    }
    for (auto& thread : threads) thread.join();
    Check(!broken.load(), "no reader saw a writer or a half-written pair, and writers ran alone");
    Check(first == static_cast<std::uint64_t>(writers) * static_cast<std::uint64_t>(iterations) && first == second, "every write under the exclusive lock counted");
    Check(reads.load() != 0, "readers got through between writers");
}

void TryLocksUnderContention(int threads, int iterations) {
    NativeSharedMutex mutex;
    std::uint64_t counter = 0;
    std::atomic<int> writersInside{0};
    std::atomic<bool> broken{false};
    std::atomic<std::uint64_t> taken{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < iterations; ++i) {
                if (((i + t) & 1) == 0) {
                    if (!mutex.try_lock()) continue;
                    if (writersInside.fetch_add(1) != 0) broken.store(true);
                    ++counter;
                    writersInside.fetch_sub(1);
                    mutex.unlock();
                    taken.fetch_add(1, std::memory_order_relaxed);
                } else {
                    if (!mutex.try_lock_shared()) continue;
                    if (writersInside.load() != 0) broken.store(true);
                    mutex.unlock_shared();
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    Check(!broken.load(), "a successful try_lock excludes every other holder");
    Check(counter == taken.load(), "every increment after a successful try_lock counted");
}

void ScopedLockPairs(int threads, int iterations) {
    NativeSharedMutex left;
    NativeSharedMutex right;
    std::uint64_t counter = 0;
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < iterations; ++i) {
                if ((t & 1) == 0) {
                    std::scoped_lock lock(left, right);
                    ++counter;
                } else {
                    std::scoped_lock lock(right, left);
                    ++counter;
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    Check(counter == static_cast<std::uint64_t>(threads) * static_cast<std::uint64_t>(iterations), "std::scoped_lock takes two locks in either order without deadlock");
}

void StripedLocks(int threads, int iterations) {
    struct alignas(64) Stripe {
        NativeSharedMutex mutex;
        std::uint64_t value = 0;
    };
    constexpr int Stripes = 16;
    const auto stripes = std::make_unique<Stripe[]>(Stripes);
    std::atomic<bool> broken{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            std::uint32_t state = 0x9e3779b9u * static_cast<std::uint32_t>(t + 1);
            for (int i = 0; i < iterations; ++i) {
                state = state * 1664525u + 1013904223u;
                auto& stripe = stripes[(state >> 16) % Stripes];
                if ((state & 3u) == 0u) {
                    std::unique_lock lock(stripe.mutex);
                    ++stripe.value;
                } else {
                    std::shared_lock lock(stripe.mutex);
                    if (stripe.value > static_cast<std::uint64_t>(threads) * static_cast<std::uint64_t>(iterations)) broken.store(true);
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    std::uint64_t writes = 0;
    for (int s = 0; s < Stripes; ++s) writes += stripes[s].value;
    std::uint64_t expected = 0;
    for (int t = 0; t < threads; ++t) {
        std::uint32_t state = 0x9e3779b9u * static_cast<std::uint32_t>(t + 1);
        for (int i = 0; i < iterations; ++i) {
            state = state * 1664525u + 1013904223u;
            if ((state & 3u) == 0u) ++expected;
        }
    }
    Check(!broken.load() && writes == expected, "every write to a striped lock counted");
}

NativeSharedMutex& StaticMutex() {
    static NativeSharedMutex mutex;
    return mutex;
}

void StaticLock() {
    std::vector<std::thread> workers;
    std::uint64_t counter = 0;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < 10000; ++i) {
                std::unique_lock lock(StaticMutex());
                ++counter;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    Check(counter == 40000, "a function-local static lock serialises its writers");
}

template<typename TMutex>
double NanosecondsPerOperation(int threads, int iterations, bool shared) {
    struct alignas(64) Slot {
        TMutex mutex;
        std::uint64_t value = 0;
    };
    const auto slots = std::make_unique<Slot[]>(static_cast<std::size_t>(threads));
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            auto& slot = slots[static_cast<std::size_t>(t)];
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < iterations; ++i) {
                if (shared) {
                    slot.mutex.lock_shared();
                    slot.value += 1;
                    slot.mutex.unlock_shared();
                } else {
                    slot.mutex.lock();
                    slot.value += 1;
                    slot.mutex.unlock();
                }
            }
        });
    }
    while (ready.load() != threads) std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    std::uint64_t total = 0;
    for (int t = 0; t < threads; ++t) total += slots[static_cast<std::size_t>(t)].value;
    Check(total == static_cast<std::uint64_t>(threads) * static_cast<std::uint64_t>(iterations), "every benchmark increment counted");
    return std::chrono::duration<double, std::nano>(elapsed).count() / iterations;
}

enum class Contention { Shared, Exclusive, Mixed };

bool Writes(Contention contention, int thread, int iteration) {
    return contention == Contention::Exclusive || (contention == Contention::Mixed && (iteration + thread) % 10 == 0);
}

template<typename TMutex>
double NanosecondsPerContendedOperation(int threads, int iterations, Contention contention) {
    TMutex mutex;
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> broken{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < iterations; ++i) {
                if (Writes(contention, t, i)) {
                    mutex.lock();
                    ++first;
                    ++second;
                    mutex.unlock();
                } else {
                    mutex.lock_shared();
                    if (first != second) broken.store(true, std::memory_order_relaxed);
                    mutex.unlock_shared();
                }
            }
        });
    }
    while (ready.load() != threads) std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    std::uint64_t expected = 0;
    for (int t = 0; t < threads; ++t) {
        for (int i = 0; i < iterations; ++i) expected += Writes(contention, t, i) ? 1u : 0u;
    }
    Check(!broken.load() && first == expected && second == expected, std::is_same_v<TMutex, std::shared_mutex> ? "std::shared_mutex lost a contended write or let a reader see a half-written pair" : "NativeSharedMutex lost a contended write or let a reader see a half-written pair");
    return std::chrono::duration<double, std::nano>(elapsed).count() / (static_cast<double>(threads) * iterations);
}

std::vector<int> ThreadCounts(int maximumThreads) {
    std::vector<int> counts;
    for (int threads = 1; threads < maximumThreads; threads *= 2) counts.push_back(threads);
    counts.push_back(maximumThreads);
    return counts;
}

void ContendedBenchmark(int maximumThreads, int iterations) {
    std::printf("all threads share one lock, %d operations each; ns per operation across all threads (wall time / (threads x operations))\n", iterations);
    std::printf("%8s %14s %14s %14s %14s %14s %14s\n", "threads", "std shared", "native shared", "std excl", "native excl", "std 10% w", "native 10% w");
    std::fflush(stdout);
    const auto cell = [](double nanoseconds) {
        std::printf(" %14.1f", nanoseconds);
        std::fflush(stdout);
    };
    for (const int threads : ThreadCounts(maximumThreads)) {
        std::printf("%8d", threads);
        std::fflush(stdout);
        cell(NanosecondsPerContendedOperation<std::shared_mutex>(threads, iterations, Contention::Shared));
        cell(NanosecondsPerContendedOperation<NativeSharedMutex>(threads, iterations, Contention::Shared));
        cell(NanosecondsPerContendedOperation<std::shared_mutex>(threads, iterations, Contention::Exclusive));
        cell(NanosecondsPerContendedOperation<NativeSharedMutex>(threads, iterations, Contention::Exclusive));
        cell(NanosecondsPerContendedOperation<std::shared_mutex>(threads, iterations, Contention::Mixed));
        cell(NanosecondsPerContendedOperation<NativeSharedMutex>(threads, iterations, Contention::Mixed));
        std::printf("\n");
    }
}

void Benchmark(int maximumThreads, int iterations) {
    std::printf("each thread locks and unlocks its own lock %d times; ns per operation per thread (wall time / iterations)\n", iterations);
    std::printf("NativeSharedMutex is %s on this platform\n", std::is_same_v<NativeSharedMutex, std::shared_mutex> ? "std::shared_mutex" : "a native lock");
    std::printf("%8s %22s %22s %22s %22s\n", "threads", "std shared", "native shared", "std exclusive", "native exclusive");
    for (const int threads : ThreadCounts(maximumThreads)) {
        const auto stdShared = NanosecondsPerOperation<std::shared_mutex>(threads, iterations, true);
        const auto nativeShared = NanosecondsPerOperation<NativeSharedMutex>(threads, iterations, true);
        const auto stdExclusive = NanosecondsPerOperation<std::shared_mutex>(threads, iterations, false);
        const auto nativeExclusive = NanosecondsPerOperation<NativeSharedMutex>(threads, iterations, false);
        std::printf("%8d %22.1f %22.1f %22.1f %22.1f\n", threads, stdShared, nativeShared, stdExclusive, nativeExclusive);
    }
}

}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--benchmark") == 0) {
        const int hardware = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        const int maximumThreads = argc >= 3 ? std::max(1, std::atoi(argv[2])) : hardware;
        const int iterations = argc >= 4 ? std::max(1, std::atoi(argv[3])) : 1000000;
        const int contendedIterations = argc >= 5 ? std::max(1, std::atoi(argv[4])) : 100000;
        Benchmark(maximumThreads, iterations);
        ContendedBenchmark(maximumThreads, contendedIterations);
        return failures == 0 ? 0 : 1;
    }
    ExclusiveCounts(8, 20000);
    TryLocks();
    ReadersOverlap();
    WriterWaitsForReader();
    ReaderWaitsForWriter();
    ReadersAndWriters(6, 2, 20000);
    ReadersAndWriters(2, 6, 5000);
    TryLocksUnderContention(8, 50000);
    ScopedLockPairs(4, 20000);
    StripedLocks(8, 50000);
    StaticLock();
    NanosecondsPerOperation<NativeSharedMutex>(4, 10000, true);
    NanosecondsPerOperation<NativeSharedMutex>(4, 10000, false);
    NanosecondsPerContendedOperation<NativeSharedMutex>(4, 10000, Contention::Mixed);
    if (failures != 0) {
        std::fprintf(stderr, "%d shared mutex check(s) failed\n", failures);
        return 1;
    }
    std::printf("shared mutex tests passed\n");
    return 0;
}
