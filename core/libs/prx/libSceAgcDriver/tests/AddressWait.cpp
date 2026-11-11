#include "prx/libSceAgcDriver/Execution/include/AddressWait.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace {

using namespace std::chrono_literals;
using AgcDriver::AddressCondition;
using AgcDriver::AddressWait;
using AgcDriver::AddressWaitFor;
using AgcDriver::AddressWaitUntil;
using AgcDriver::AddressWakeAll;
using AgcDriver::AddressWakeOne;
using Clock = std::chrono::steady_clock;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class Watchdog {
public:
    explicit Watchdog(std::chrono::seconds limit) : thread([this, limit](std::stop_token token) {
        std::unique_lock lock(mutex);
        if (wake.wait_for(lock, token, limit, [] { return false; }) || token.stop_requested()) return;
        std::fprintf(stderr, "address wait tests: '%s' made no progress for %lld s, a wakeup was lost\n", phase.c_str(), static_cast<long long>(limit.count()));
        std::fflush(stderr);
        std::_Exit(1);
    }) {}

    void Phase(std::string name) {
        std::lock_guard lock(mutex);
        phase = std::move(name);
    }

private:
    std::mutex mutex;
    std::condition_variable_any wake;
    std::string phase = "start";
    std::jthread thread;
};

static_assert(AgcDriver::AddressWord<std::uint32_t> && AgcDriver::AddressWord<std::int32_t> && AgcDriver::AddressWord<float>);
static_assert(!AgcDriver::AddressWord<std::uint64_t> && !AgcDriver::AddressWord<std::uint16_t>, "a futex compares 32 bits, so a 64-bit word that changed only in its upper half would sleep through the change");

void CheckSemantics() {
    std::atomic<std::uint32_t> word32{7};
    std::atomic<std::uint32_t> other{0x7007};
    AddressWait(word32, 6u);
    AddressWait(other, 7u);
    AddressWakeOne(word32);
    AddressWakeAll(word32);
    AddressWakeOne(other);
    AddressWakeAll(other);
    Require(AddressWaitFor(word32, 6u, 1s), "a timed wait on a value that differs reported a timeout");
    const auto start = Clock::now();
    Require(!AddressWaitFor(word32, 7u, 20ms) || Clock::now() - start < 20ms, "a timed wait that returned after its deadline did not report the timeout");
    for (int round = 0; round < 50 && Clock::now() - start < 20ms; ++round) AddressWaitUntil(word32, 7u, start + 20ms);
    Require(Clock::now() - start >= 20ms, "timed waits on an unchanged value returned for good before the deadline");
    Require(!AddressWaitUntil(other, 0x7007u, Clock::now() - 1ms), "a wait with a past deadline did not report the timeout");
    std::thread setter([&] {
        std::this_thread::sleep_for(5ms);
        other.store(0x7008u, std::memory_order_release);
        AddressWakeAll(other);
    });
    while (other.load(std::memory_order_acquire) == 0x7007u) AddressWait(other, 0x7007u);
    setter.join();
}

template<typename TWord>
void PingPong(Watchdog& watchdog, const char* what) {
    watchdog.Phase(std::string("ping-pong ") + what);
    constexpr TWord Rounds = 20000;
    std::atomic<TWord> turn{0};
    std::thread other([&] {
        for (TWord expected = 1; expected < 2 * Rounds; expected += 2) {
            for (auto seen = turn.load(std::memory_order_acquire); seen != expected; seen = turn.load(std::memory_order_acquire)) AddressWait(turn, seen);
            turn.store(expected + 1, std::memory_order_release);
            AddressWakeOne(turn);
        }
    });
    for (TWord expected = 0; expected < 2 * Rounds; expected += 2) {
        for (auto seen = turn.load(std::memory_order_acquire); seen != expected; seen = turn.load(std::memory_order_acquire)) AddressWait(turn, seen);
        turn.store(expected + 1, std::memory_order_release);
        AddressWakeOne(turn);
    }
    other.join();
    Require(turn.load() == 2 * Rounds, std::string(what) + ": the ping-pong ended on the wrong turn");
}

void EncoderHandoff(Watchdog& watchdog) {
    watchdog.Phase("posted and executed counters with sleeping flags");
    constexpr std::uint32_t Chunks = 60000;
    std::atomic<std::uint32_t> posted{0};
    std::atomic<std::uint32_t> executed{0};
    std::atomic<bool> sleeping{false};
    std::atomic<std::uint32_t> awaiting{0};
    std::atomic<bool> stopping{false};
    std::thread worker([&] {
        std::uint32_t taken = 0;
        for (;;) {
            if (posted.load(std::memory_order_acquire) == taken) {
                sleeping.store(true, std::memory_order_seq_cst);
                if (posted.load(std::memory_order_seq_cst) == taken) AddressWait(posted, taken);
                sleeping.store(false, std::memory_order_relaxed);
                continue;
            }
            ++taken;
            if (stopping.load(std::memory_order_acquire) && taken > Chunks) return;
            executed.fetch_add(1, std::memory_order_seq_cst);
            if (awaiting.load(std::memory_order_seq_cst) != 0) AddressWakeAll(executed);
        }
    });
    const auto await = [&](std::uint32_t through) {
        if (executed.load(std::memory_order_acquire) >= through) return;
        awaiting.fetch_add(1, std::memory_order_seq_cst);
        for (auto seen = executed.load(std::memory_order_seq_cst); seen < through; seen = executed.load(std::memory_order_seq_cst)) AddressWait(executed, seen);
        awaiting.fetch_sub(1, std::memory_order_relaxed);
    };
    std::atomic<bool> settling{true};
    std::vector<std::thread> settlers;
    for (int index = 0; index < 3; ++index) {
        settlers.emplace_back([&] {
            while (settling.load(std::memory_order_acquire)) await(posted.load(std::memory_order_acquire));
        });
    }
    for (std::uint32_t chunk = 1; chunk <= Chunks; ++chunk) {
        posted.fetch_add(1, std::memory_order_seq_cst);
        if (sleeping.load(std::memory_order_seq_cst)) AddressWakeOne(posted);
        if (chunk % 3 == 0) await(chunk);
    }
    await(Chunks);
    settling.store(false, std::memory_order_release);
    for (auto& settler : settlers) settler.join();
    Require(executed.load() == Chunks, "the worker did not execute every posted chunk");
    stopping.store(true, std::memory_order_seq_cst);
    posted.fetch_add(1, std::memory_order_seq_cst);
    AddressWakeAll(posted);
    worker.join();
}

void BoundedQueue(Watchdog& watchdog) {
    watchdog.Phase("bounded queue with many producers and consumers");
    constexpr int Producers = 4;
    constexpr int Consumers = 4;
    constexpr int Items = 40000;
    constexpr std::size_t Depth = 8;
    std::mutex mutex;
    AddressCondition notEmpty;
    AddressCondition notFull;
    AddressCondition drained;
    std::deque<int> queue;
    int producersLeft = Producers;
    std::atomic<std::uint64_t> sum{0};
    std::atomic<int> consumed{0};
    std::vector<std::thread> threads;
    for (int producer = 0; producer < Producers; ++producer) {
        threads.emplace_back([&, producer] {
            for (int item = 1; item <= Items; ++item) {
                std::unique_lock lock(mutex);
                notFull.Wait(lock, [&] { return queue.size() < Depth; });
                queue.push_back(producer * Items + item);
                lock.unlock();
                notEmpty.NotifyOne();
            }
            std::lock_guard lock(mutex);
            if (--producersLeft == 0) {
                notEmpty.NotifyAll();
                drained.NotifyAll();
            }
        });
    }
    for (int consumer = 0; consumer < Consumers; ++consumer) {
        threads.emplace_back([&, consumer] {
            for (;;) {
                std::unique_lock lock(mutex);
                if (consumer % 2 == 0) notEmpty.Wait(lock, [&] { return !queue.empty() || producersLeft == 0; });
                else while (!notEmpty.WaitFor(lock, 50us, [&] { return !queue.empty() || producersLeft == 0; })) {}
                if (queue.empty()) return;
                const auto item = queue.front();
                queue.pop_front();
                const bool empty = queue.empty();
                lock.unlock();
                notFull.NotifyAll();
                if (empty) drained.NotifyAll();
                sum.fetch_add(static_cast<std::uint64_t>(item), std::memory_order_relaxed);
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    {
        std::unique_lock lock(mutex);
        drained.Wait(lock, [&] { return producersLeft == 0 && queue.empty(); });
    }
    for (auto& thread : threads) thread.join();
    std::uint64_t expected = 0;
    for (int producer = 0; producer < Producers; ++producer) {
        for (int item = 1; item <= Items; ++item) expected += static_cast<std::uint64_t>(producer * Items + item);
    }
    Require(consumed.load() == Producers * Items, "the consumers lost or duplicated items");
    Require(sum.load() == expected, "the consumers took items that were not produced");
}

void FlagOutsideTheLock(Watchdog& watchdog) {
    watchdog.Phase("flag set outside the lock");
    constexpr int Rounds = 20000;
    std::mutex mutex;
    AddressCondition changed;
    std::atomic<int> round{0};
    std::atomic<int> answered{0};
    std::thread answerer([&] {
        for (int expected = 1; expected <= Rounds; ++expected) {
            std::unique_lock lock(mutex);
            changed.Wait(lock, [&] { return round.load(std::memory_order_acquire) >= expected; });
            lock.unlock();
            answered.store(expected, std::memory_order_release);
            changed.NotifyAll();
        }
    });
    for (int next = 1; next <= Rounds; ++next) {
        round.store(next, std::memory_order_release);
        changed.NotifyAll();
        std::unique_lock lock(mutex);
        changed.Wait(lock, [&] { return answered.load(std::memory_order_acquire) >= next; });
    }
    answerer.join();
}

void NotifyBetweenCheckAndSleep(Watchdog& watchdog) {
    watchdog.Phase("notify between the predicate and the sleep");
    std::mutex mutex;
    AddressCondition changed;
    std::atomic<bool> flag{false};
    for (int round = 0; round < 1000; ++round) {
        flag.store(false, std::memory_order_relaxed);
        int checks = 0;
        std::unique_lock lock(mutex);
        changed.Wait(lock, [&] {
            if (flag.load(std::memory_order_acquire)) return true;
            if (++checks == 2) {
                flag.store(true, std::memory_order_release);
                changed.NotifyAll();
            }
            return false;
        });
        Require(flag.load(std::memory_order_acquire), "a wait returned before its predicate held");
    }
}

void BroadcastWakesEveryWaiter(Watchdog& watchdog) {
    watchdog.Phase("broadcast to every sleeping waiter");
    constexpr int Waiters = 8;
    for (int round = 0; round < 20; ++round) {
        std::mutex mutex;
        AddressCondition changed;
        bool open = false;
        std::atomic<int> waiting{0};
        std::atomic<int> woken{0};
        std::vector<std::thread> threads;
        for (int index = 0; index < Waiters; ++index) {
            threads.emplace_back([&] {
                std::unique_lock lock(mutex);
                waiting.fetch_add(1, std::memory_order_relaxed);
                changed.Wait(lock, [&] { return open; });
                woken.fetch_add(1, std::memory_order_relaxed);
            });
        }
        while (waiting.load(std::memory_order_relaxed) != Waiters) std::this_thread::yield();
        std::this_thread::sleep_for(5ms);
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        changed.NotifyAll();
        for (auto& thread : threads) thread.join();
        Require(woken.load() == Waiters, "a broadcast did not wake every waiter");
    }
}

void MutexContention(Watchdog& watchdog) {
    watchdog.Phase("driver mutex under contention");
    constexpr int Workers = 6;
    constexpr int Rounds = 30000;
    AgcDriver::Mutex mutex;
    std::uint64_t counter = 0;
    std::atomic<int> inside{0};
    std::atomic<bool> overlapped{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < Workers; ++worker) {
        workers.emplace_back([&] {
            for (int round = 0; round < Rounds; ++round) {
                mutex.lock();
                if (inside.fetch_add(1) != 0) overlapped.store(true);
                ++counter;
                if (round % 64 == 0) std::this_thread::yield();
                inside.fetch_sub(1);
                mutex.unlock();
            }
        });
    }
    for (auto& worker : workers) worker.join();
    Require(!overlapped.load(), "two threads held the driver mutex at once");
    Require(counter == static_cast<std::uint64_t>(Workers) * Rounds, "increments under the driver mutex were lost");
}

void TimedWaits(Watchdog& watchdog) {
    watchdog.Phase("timed waits");
    std::mutex mutex;
    AddressCondition changed;
    bool ready = false;
    {
        std::unique_lock lock(mutex);
        const auto start = Clock::now();
        Require(!changed.WaitFor(lock, 10ms, [&] { return ready; }), "a timed wait on a predicate that never held reported success");
        Require(Clock::now() - start >= 10ms, "a timed wait gave up before its timeout");
        Require(lock.owns_lock(), "a timed wait returned without the lock");
        changed.WaitFor(lock, 1ms);
        Require(lock.owns_lock(), "an unconditional timed wait returned without the lock");
    }
    std::thread setter([&] {
        std::this_thread::sleep_for(5ms);
        {
            std::lock_guard lock(mutex);
            ready = true;
        }
        changed.NotifyOne();
    });
    {
        std::unique_lock lock(mutex);
        Require(changed.WaitFor(lock, 10s, [&] { return ready; }), "a timed wait missed the notification");
    }
    setter.join();
    constexpr int Waiters = 8;
    constexpr int Rounds = 3000;
    std::atomic<int> generation{0};
    std::atomic<std::uint64_t> timeouts{0};
    std::vector<std::thread> waiters;
    for (int index = 0; index < Waiters; ++index) {
        waiters.emplace_back([&, index] {
            for (int expected = 1; expected <= Rounds; ++expected) {
                std::unique_lock lock(mutex);
                while (!changed.WaitFor(lock, std::chrono::microseconds(10 + 37 * index), [&] { return generation.load(std::memory_order_acquire) >= expected; })) timeouts.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int next = 1; next <= Rounds; ++next) {
        {
            std::lock_guard lock(mutex);
            generation.store(next, std::memory_order_release);
        }
        changed.NotifyAll();
        if (next % 100 == 0) std::this_thread::sleep_for(50us);
    }
    for (auto& waiter : waiters) waiter.join();
}

void StopTokenWait(Watchdog& watchdog) {
    watchdog.Phase("stop token wait");
    std::mutex mutex;
    AddressCondition changed;
    for (int round = 0; round < 200; ++round) {
        std::atomic<bool> result{true};
        std::jthread waiter([&](std::stop_token token) {
            std::unique_lock lock(mutex);
            result.store(changed.Wait(lock, token, [] { return false; }));
        });
        if (round % 2 == 0) std::this_thread::sleep_for(100us);
        waiter.request_stop();
        waiter.join();
        Require(!result.load(), "a wait ended by its stop token reported the predicate");
    }
}

std::uint64_t Stamp() {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
#endif
}

double NsPerStamp() {
    static const double ratio = [] {
        const auto start = Clock::now();
        const auto first = Stamp();
        std::this_thread::sleep_for(50ms);
        const auto last = Stamp();
        const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        return elapsed / static_cast<double>(last - first);
    }();
    return ratio;
}

double ThreadsCpuSeconds() {
#ifdef _WIN32
    ULONG64 cycles = 0;
    if (!QueryProcessCycleTime(GetCurrentProcess(), &cycles)) return 0;
    return static_cast<double>(cycles) * NsPerStamp() / 1e9;
#else
    timespec time{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time);
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) / 1e9;
#endif
}

struct StandardWord {
    template<typename TWord>
    static void Wait(std::atomic<TWord>& word, TWord old) { word.wait(old, std::memory_order_acquire); }
    template<typename TWord>
    static void Wake(std::atomic<TWord>& word) { word.notify_all(); }
};

struct NativeWord {
    template<typename TWord>
    static void Wait(std::atomic<TWord>& word, TWord old) { AddressWait(word, old); }
    template<typename TWord>
    static void Wake(std::atomic<TWord>& word) { AddressWakeAll(word); }
};

struct SleepingWord {
    template<typename TWord>
    static void Wait(std::atomic<TWord>& word, TWord old) {
        if (word.load(std::memory_order_relaxed) == old) AgcDriver::AddressWaitDetail::Wait(word, old);
    }
    template<typename TWord>
    static void Wake(std::atomic<TWord>& word) { AddressWakeAll(word); }
};

struct Measurement {
    double handoffNs;
    double cpuNsPerHandoff;
    std::uint64_t rescues = 0;
};

template<typename TProgress, typename TRescue>
class Rescuer {
public:
    Rescuer(TProgress progress, TRescue rescue) : thread([this, progress, rescue](std::stop_token token) {
        auto last = progress();
        while (!token.stop_requested()) {
            {
                std::unique_lock lock(mutex);
                if (wake.wait_for(lock, token, 100ms, [] { return false; }) || token.stop_requested()) return;
            }
            const auto now = progress();
            if (now == last && !token.stop_requested()) {
                rescue();
                count.fetch_add(1, std::memory_order_relaxed);
            }
            last = now;
        }
    }) {}

    std::uint64_t Stop() {
        thread.request_stop();
        thread.join();
        return count.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> count{0};
    std::mutex mutex;
    std::condition_variable_any wake;
    std::jthread thread;
};

template<typename TWait, typename TWord>
Measurement HandoffWords(int rounds, int bystanders) {
    std::atomic<TWord> turn{0};
    std::vector<std::atomic<TWord>> idle(static_cast<std::size_t>(std::max(bystanders, 1)));
    std::atomic<bool> stop{false};
    std::vector<std::thread> sleepers;
    for (int index = 0; index < bystanders; ++index) {
        sleepers.emplace_back([&, index] {
            auto& word = idle[static_cast<std::size_t>(index)];
            while (!stop.load(std::memory_order_acquire)) TWait::Wait(word, TWord{0});
        });
    }
    std::this_thread::sleep_for(20ms);
    const auto cpuStart = ThreadsCpuSeconds();
    const auto start = Clock::now();
    Rescuer rescuer([&] { return turn.load(std::memory_order_acquire); }, [&] { TWait::Wake(turn); });
    std::thread other([&] {
        for (TWord expected = 1; expected < static_cast<TWord>(2 * rounds); expected += 2) {
            for (auto seen = turn.load(std::memory_order_acquire); seen != expected; seen = turn.load(std::memory_order_acquire)) TWait::Wait(turn, seen);
            turn.store(expected + 1, std::memory_order_release);
            TWait::Wake(turn);
        }
    });
    for (TWord expected = 0; expected < static_cast<TWord>(2 * rounds); expected += 2) {
        for (auto seen = turn.load(std::memory_order_acquire); seen != expected; seen = turn.load(std::memory_order_acquire)) TWait::Wait(turn, seen);
        turn.store(expected + 1, std::memory_order_release);
        TWait::Wake(turn);
    }
    other.join();
    const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    const auto cpu = (ThreadsCpuSeconds() - cpuStart) * 1e9;
    const auto rescues = rescuer.Stop();
    stop.store(true, std::memory_order_release);
    for (auto& word : idle) {
        word.store(1, std::memory_order_release);
        TWait::Wake(word);
    }
    for (auto& sleeper : sleepers) sleeper.join();
    return {elapsed / (2.0 * rounds), cpu / (2.0 * rounds), rescues};
}

template<typename TCondition, typename TMutex>
Measurement HandoffConditions(int rounds) {
    TMutex mutex;
    TCondition changed;
    int turn = 0;
    const auto notify = [&] {
        if constexpr (std::is_same_v<TCondition, std::condition_variable>) changed.notify_all();
        else changed.NotifyAll();
    };
    const auto wait = [&](std::unique_lock<TMutex>& lock, int expected) {
        if constexpr (std::is_same_v<TCondition, std::condition_variable>) changed.wait(lock, [&] { return turn == expected; });
        else changed.Wait(lock, [&] { return turn == expected; });
    };
    std::atomic<int> progress{0};
    const auto cpuStart = ThreadsCpuSeconds();
    const auto start = Clock::now();
    Rescuer rescuer([&] { return progress.load(std::memory_order_relaxed); }, notify);
    std::thread other([&] {
        for (int expected = 1; expected < 2 * rounds; expected += 2) {
            std::unique_lock lock(mutex);
            wait(lock, expected);
            ++turn;
            progress.store(turn, std::memory_order_relaxed);
            lock.unlock();
            notify();
        }
    });
    for (int expected = 0; expected < 2 * rounds; expected += 2) {
        std::unique_lock lock(mutex);
        wait(lock, expected);
        ++turn;
        progress.store(turn, std::memory_order_relaxed);
        lock.unlock();
        notify();
    }
    other.join();
    const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    const auto cpu = (ThreadsCpuSeconds() - cpuStart) * 1e9;
    const auto rescues = rescuer.Stop();
    return {elapsed / (2.0 * rounds), cpu / (2.0 * rounds), rescues};
}

template<typename TNotify>
Measurement IdleNotifies(int rounds, TNotify notify) {
    const auto cpuStart = ThreadsCpuSeconds();
    const auto start = Clock::now();
    for (int round = 0; round < rounds; ++round) notify();
    const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    const auto cpu = (ThreadsCpuSeconds() - cpuStart) * 1e9;
    return {elapsed / rounds, cpu / rounds};
}

struct ColdWake {
    double medianNs;
    double p90Ns;
    double notifyNs;
    double cpuNs;
    std::uint64_t rescues;
};

template<typename TWait, typename TNotify>
ColdWake ColdWakes(int rounds, TWait wait, TNotify notify) {
    std::atomic<int> armed{0};
    std::atomic<std::uint64_t> sentAt{0};
    std::vector<double> latencies;
    std::vector<double> notifies;
    std::uint64_t rescues = 0;
    latencies.reserve(static_cast<std::size_t>(rounds));
    notifies.reserve(static_cast<std::size_t>(rounds));
    const auto cpuStart = ThreadsCpuSeconds();
    std::thread waiter([&] {
        for (int round = 1; round <= rounds; ++round) {
            armed.store(round, std::memory_order_release);
            wait(round);
            const auto now = Stamp();
            latencies.push_back(static_cast<double>(static_cast<std::int64_t>(now - sentAt.load(std::memory_order_acquire))) * NsPerStamp());
        }
        armed.store(rounds + 1, std::memory_order_release);
    });
    for (int round = 1; round <= rounds; ++round) {
        for (auto stalled = Clock::now() + 100ms; armed.load(std::memory_order_acquire) != round; std::this_thread::yield()) {
            if (round == 1 || Clock::now() < stalled) continue;
            notify(round - 1);
            ++rescues;
            stalled = Clock::now() + 100ms;
        }
        std::this_thread::sleep_for(200us);
        const auto sent = Stamp();
        sentAt.store(sent, std::memory_order_release);
        notify(round);
        notifies.push_back(static_cast<double>(Stamp() - sent) * NsPerStamp());
    }
    for (auto stalled = Clock::now() + 100ms; armed.load(std::memory_order_acquire) != rounds + 1; std::this_thread::yield()) {
        if (Clock::now() < stalled) continue;
        notify(rounds);
        ++rescues;
        stalled = Clock::now() + 100ms;
    }
    waiter.join();
    const auto cpu = (ThreadsCpuSeconds() - cpuStart) * 1e9 / rounds;
    std::ranges::sort(latencies);
    std::ranges::sort(notifies);
    return {latencies[latencies.size() / 2], latencies[latencies.size() * 9 / 10], notifies[notifies.size() / 2], cpu, rescues};
}

template<typename TWord, bool Native>
ColdWake ColdWordWakes(int rounds) {
    std::atomic<TWord> word{0};
    return ColdWakes(rounds, [&](int round) {
        for (auto seen = word.load(std::memory_order_acquire); seen != static_cast<TWord>(round); seen = word.load(std::memory_order_acquire)) {
            if constexpr (Native) SleepingWord::Wait(word, seen);
            else word.wait(seen, std::memory_order_acquire);
        }
    }, [&](int round) {
        word.store(static_cast<TWord>(round), std::memory_order_release);
        if constexpr (Native) AddressWakeOne(word);
        else word.notify_one();
    });
}

template<typename TCondition>
ColdWake ColdConditionWakes(int rounds) {
    std::mutex mutex;
    TCondition changed;
    int turn = 0;
    return ColdWakes(rounds, [&](int round) {
        std::unique_lock lock(mutex);
        if constexpr (std::is_same_v<TCondition, std::condition_variable>) changed.wait(lock, [&] { return turn == round; });
        else changed.Wait(lock, [&] { return turn == round; });
    }, [&](int round) {
        {
            std::lock_guard lock(mutex);
            turn = round;
        }
        if constexpr (std::is_same_v<TCondition, std::condition_variable>) changed.notify_one();
        else changed.NotifyOne();
    });
}

void Print(const char* what, const std::function<Measurement()>& measure) {
    constexpr std::size_t Repeats = 7;
    std::vector<Measurement> runs;
    for (std::size_t run = 0; run < Repeats; ++run) runs.push_back(measure());
    std::uint64_t rescues = 0;
    for (const auto& run : runs) rescues += run.rescues;
    std::ranges::sort(runs, {}, &Measurement::handoffNs);
    const auto median = runs[Repeats / 2];
    std::printf("%-56s median %8.0f ns (min %7.0f, max %7.0f), %7.0f ns CPU, %llu lost wakeups\n", what, median.handoffNs, runs.front().handoffNs, runs.back().handoffNs, median.cpuNsPerHandoff, static_cast<unsigned long long>(rescues));
}

void PrintCold(const char* what, ColdWake value) {
    std::printf("%-56s median %8.0f ns, p90 %8.0f ns, notifier %6.0f ns, %8.0f ns CPU per round, %llu lost wakeups\n", what, value.medianNs, value.p90Ns, value.notifyNs, value.cpuNs, static_cast<unsigned long long>(value.rescues));
}

int Benchmark() {
    constexpr int Rounds = 20000;
    constexpr int ColdRounds = 2000;
    std::printf("native address waits: %s; timestamps %.3f ns per tick (TSC on x86-64), CPU from %s\n", AgcDriver::NativeAddressWait() ? "yes" : "no, std::atomic fallback", NsPerStamp(),
#ifdef _WIN32
        "QueryProcessCycleTime"
#else
        "CLOCK_PROCESS_CPUTIME_ID"
#endif
    );
    std::printf("cold wakes: the native rows sleep without the 64-pause spin; the waiter is blocked for 200 us, so a spin could not finish them anyway\n");
    std::printf("wake latency of a sleeping waiter (waiter blocked for 200 us before each wake):\n");
    PrintCold("  std::atomic<uint32_t> wait/notify_one", ColdWordWakes<std::uint32_t, false>(ColdRounds));
    PrintCold("  native 32-bit wait/wake one", ColdWordWakes<std::uint32_t, true>(ColdRounds));
    PrintCold("  std::atomic<uint64_t> wait/notify_one", ColdWordWakes<std::uint64_t, false>(ColdRounds));
    PrintCold("  std::condition_variable + std::mutex", ColdConditionWakes<std::condition_variable>(ColdRounds));
    PrintCold("  AddressCondition + std::mutex", ColdConditionWakes<AddressCondition>(ColdRounds));
    std::printf("ping-pong between two blocking threads, per hand-off:\n");
    Print("  std::atomic<uint32_t> wait/notify_all", [] { return HandoffWords<StandardWord, std::uint32_t>(Rounds, 0); });
    Print("  native 32-bit wait/wake all", [] { return HandoffWords<NativeWord, std::uint32_t>(Rounds, 0); });
    Print("  native 32-bit, sleep only (no 64-pause spin)", [] { return HandoffWords<SleepingWord, std::uint32_t>(Rounds, 0); });
    Print("  std::atomic<uint64_t> wait/notify_all", [] { return HandoffWords<StandardWord, std::uint64_t>(Rounds, 0); });
    Print("  std::atomic<uint32_t>, 32 idle waiters on other words", [] { return HandoffWords<StandardWord, std::uint32_t>(Rounds, 32); });
    Print("  std::atomic<uint64_t>, 32 idle waiters on other words", [] { return HandoffWords<StandardWord, std::uint64_t>(Rounds, 32); });
    Print("  native 32-bit, 32 idle waiters on other words", [] { return HandoffWords<NativeWord, std::uint32_t>(Rounds, 32); });
    Print("  native 32-bit sleep only, 32 idle waiters", [] { return HandoffWords<SleepingWord, std::uint32_t>(Rounds, 32); });
    Print("  std::condition_variable + std::mutex", [] { return HandoffConditions<std::condition_variable, std::mutex>(Rounds); });
    Print("  AddressCondition + std::mutex", [] { return HandoffConditions<AddressCondition, std::mutex>(Rounds); });
    std::printf("notify with no waiter, per call:\n");
    std::atomic<std::uint32_t> word{0};
    std::condition_variable condition;
    AddressCondition addressCondition;
    Print("  std::atomic<uint32_t>::notify_all", [&] { return IdleNotifies(Rounds * 10, [&] { word.notify_all(); }); });
    Print("  native wake all, unconditional", [&] { return IdleNotifies(Rounds * 10, [&] { AddressWakeAll(word); }); });
    Print("  std::condition_variable::notify_all", [&] { return IdleNotifies(Rounds * 10, [&] { condition.notify_all(); }); });
    Print("  AddressCondition::NotifyAll", [&] { return IdleNotifies(Rounds * 10, [&] { addressCondition.NotifyAll(); }); });
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::strcmp(argv[1], "--benchmark") == 0) return Benchmark();
        Watchdog watchdog(20s);
        CheckSemantics();
        PingPong<std::uint32_t>(watchdog, "32-bit");
        PingPong<std::int32_t>(watchdog, "signed 32-bit");
        EncoderHandoff(watchdog);
        BoundedQueue(watchdog);
        FlagOutsideTheLock(watchdog);
        NotifyBetweenCheckAndSleep(watchdog);
        BroadcastWakesEveryWaiter(watchdog);
        MutexContention(watchdog);
        TimedWaits(watchdog);
        StopTokenWait(watchdog);
        std::cout << "address wait tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
