#include "prx/libSceAgcDriver/Execution/include/HandoffSpin.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using AgcDriver::HandoffAdaptiveFor;
using AgcDriver::HandoffSite;
using AgcDriver::HandoffSpinWindowFor;
using AgcDriver::LockSpinning;
using AgcDriver::SpinUntil;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void CheckWindowPolicy() {
    Require(HandoffSpinWindowFor(nullptr, 20) == 150us, "20 usable processors do not spin for 150 us");
    Require(HandoffSpinWindowFor(nullptr, 9) == 150us, "9 usable processors do not spin for 150 us");
    Require(HandoffSpinWindowFor(nullptr, 8) == 0ns, "8 usable processors spin");
    Require(HandoffSpinWindowFor(nullptr, 4) == 0ns, "4 usable processors spin");
    Require(HandoffSpinWindowFor(nullptr, 0) == 0ns, "an unknown processor count spins");
    Require(HandoffSpinWindowFor("0", 20) == 0ns, "APS5_HANDOFF_SPIN_US=0 still spins");
    Require(HandoffSpinWindowFor("400", 2) == 400us, "APS5_HANDOFF_SPIN_US=400 does not set 400 us on 2 logical processors");
    Require(HandoffSpinWindowFor("-5", 20) == 0ns, "a negative window spins");
    Require(HandoffSpinWindowFor("text", 20) == 0ns, "a window that is no number spins");
    Require(HandoffSpinWindowFor("99999999999", 20) == 10000us, "a huge window is not limited to 10000 us");
}

void CheckUsableProcessors() {
    const unsigned usable = AgcDriver::UsableProcessors();
    Require(usable >= 1, "no usable processor was counted");
    Require(usable <= (std::max)(std::thread::hardware_concurrency(), 64u), "more usable processors than the machine has");
}

void CheckSpinUntil() {
    int calls = 0;
    Require(SpinUntil(0ns, [&] { ++calls; return true; }) && calls == 1, "a ready condition was evaluated more than once");
    calls = 0;
    Require(!SpinUntil(0ns, [&] { ++calls; return false; }) && calls == 1, "a zero window evaluated the condition more than once");
    calls = 0;
    Require(SpinUntil(1s, [&] { return ++calls == 5; }) && calls == 5, "a condition that holds on its fifth evaluation was not seen");
    const auto start = std::chrono::steady_clock::now();
    Require(!SpinUntil(3ms, [] { return false; }), "a condition that never holds was reported ready");
    Require(std::chrono::steady_clock::now() - start >= 3ms, "the spin returned before its window ended");
    std::atomic<bool> flag{false};
    std::thread setter([&] {
        std::this_thread::sleep_for(500us);
        flag.store(true, std::memory_order_release);
    });
    const bool seen = SpinUntil(5s, [&] { return flag.load(std::memory_order_acquire); });
    setter.join();
    Require(seen, "a flag set by another thread was not seen within the window");
}

template<typename TMutex>
void Contend(std::chrono::nanoseconds window, const char* what) {
    constexpr int Workers = 4;
    constexpr int Rounds = 20000;
    TMutex mutex;
    std::atomic<int> inside{0};
    std::atomic<bool> overlapped{false};
    std::uint64_t counter = 0;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < Workers; ++worker) {
        workers.emplace_back([&] {
            for (int round = 0; round < Rounds; ++round) {
                LockSpinning(mutex, window);
                if (inside.fetch_add(1) != 0) overlapped.store(true);
                ++counter;
                inside.fetch_sub(1);
                mutex.unlock();
            }
        });
    }
    for (auto& worker : workers) worker.join();
    Require(!overlapped.load(), std::string(what) + ": two threads held the mutex at once");
    Require(counter == static_cast<std::uint64_t>(Workers) * Rounds, std::string(what) + ": increments were lost");
}

void CheckBlockingFallback() {
    std::mutex mutex;
    std::atomic<bool> held{false};
    std::atomic<bool> released{false};
    std::thread holder([&] {
        mutex.lock();
        held.store(true);
        std::this_thread::sleep_for(20ms);
        released.store(true);
        mutex.unlock();
    });
    while (!held.load()) std::this_thread::yield();
    LockSpinning(mutex, 100us);
    const bool afterRelease = released.load();
    mutex.unlock();
    holder.join();
    Require(afterRelease, "a spin that outlasted its window did not wait for the holder");
}

void CheckHandoffSite() {
    HandoffSite site{"test", 3ms, false};
    Require(std::string(site.Name()) == "test" && site.Window() == 3ms, "a site did not keep its name and window");
    const auto immediate = site.Spin([] { return true; });
    Require(!immediate.waited && immediate.ready, "a condition that already held was counted as a wait");
    int calls = 0;
    const auto caught = site.Spin([&] { return ++calls == 4; });
    Require(caught.waited && caught.ready && calls == 4 && caught.end >= caught.start, "a condition that held within the window was not reported as caught");
    const auto missed = site.Spin([] { return false; });
    Require(missed.waited && !missed.ready && missed.end - missed.start >= 3ms, "a condition that never held was not reported as missed after the window");
    HandoffSite off{"off", 0ns, false};
    calls = 0;
    const auto blocked = off.Spin([&] { return ++calls > 1; });
    Require(blocked.waited && !blocked.ready && calls == 1 && blocked.start == blocked.end, "a zero window spun or evaluated the condition more than once");
    std::mutex mutex;
    std::atomic<bool> held{false};
    std::thread holder([&] {
        mutex.lock();
        held.store(true);
        std::this_thread::sleep_for(20ms);
        mutex.unlock();
    });
    while (!held.load()) std::this_thread::yield();
    HandoffSite lockSite{"lock", 100us, false};
    const auto locked = LockSpinning(mutex, lockSite);
    mutex.unlock();
    holder.join();
    Require(locked.waited && !locked.ready, "a lock held past the window was not reported as a miss");
    const auto free = LockSpinning(mutex, lockSite);
    mutex.unlock();
    Require(!free.waited && free.ready, "a free lock was reported as a wait");
}

void CheckAdaptation() {
    Require(HandoffAdaptiveFor(nullptr), "adaptation is off by default");
    Require(!HandoffAdaptiveFor("0"), "APS5_HANDOFF_ADAPTIVE=0 does not turn adaptation off");
    Require(HandoffAdaptiveFor("1"), "APS5_HANDOFF_ADAPTIVE=1 turns adaptation off");
    HandoffSite fixed{"fixed", 150us, false};
    fixed.Learn(false, 150us);
    fixed.Learn(true, 1us);
    Require(fixed.Window() == 150us, "a fixed site changed its window");
    HandoffSite site{"adaptive", 150us, true};
    Require(site.Window() == 150us, "an adaptive site does not start at its cap");
    site.Learn(true, 2us);
    Require(site.Window() == 8us, "a hand-off caught after 2 us did not set a window of four times that");
    for (int round = 0; round < 50; ++round) site.Learn(true, 10us);
    Require(site.Window() > 32us && site.Window() <= 40us, "the window did not follow hand-offs that settled at 10 us");
    site.Learn(true, 1ms);
    Require(site.Window() <= 150us, "a slow hand-off raised the window above the cap");
    HandoffSite shrinking{"shrinking", 150us, true};
    shrinking.Learn(false, 150us);
    Require(shrinking.Window() == 75us, "a miss did not halve the window");
    for (int round = 0; round < 10; ++round) shrinking.Learn(false, 1us);
    Require(shrinking.Window() == 0ns, "repeated misses did not stop the spin");
    shrinking.Learn(true, 100ns);
    Require(shrinking.Window() == HandoffSite::Floor, "a very short hand-off set a window below the floor");
    HandoffSite off{"off", 0ns, true};
    off.Learn(true, 5us);
    Require(off.Window() == 0ns, "a zero cap learned a window");
    int spun = 0;
    for (std::uint32_t call = 0; call < 2 * HandoffSite::ProbeEvery; ++call) {
        const auto result = off.Spin([] { return false; });
        if (result.end != result.start) ++spun;
    }
    Require(spun == 0, "a zero cap probed");
    HandoffSite probing{"probing", 150us, true};
    probing.Learn(true, 20us);
    for (int round = 0; round < 10; ++round) probing.Learn(false, 1us);
    Require(probing.Window() == 0ns, "the probing site did not stop spinning");
    spun = 0;
    for (std::uint32_t call = 0; call < 2 * HandoffSite::ProbeEvery; ++call) {
        const auto result = probing.Spin([] { return false; });
        if (result.end != result.start) {
            ++spun;
            Require(result.end - result.start >= 80us, "a probe did not spin for four times the typical hand-off");
        }
    }
    Require(spun == 2, "a site that stopped spinning did not probe once every " + std::to_string(HandoffSite::ProbeEvery) + " calls");
    std::atomic<bool> polling{false};
    std::atomic<bool> released{false};
    std::thread releaser([&] {
        while (!polling.load(std::memory_order_acquire)) std::this_thread::yield();
        released.store(true, std::memory_order_release);
    });
    HandoffSite catching{"catching", 1s, true};
    int polls = 0;
    const auto caught = catching.Spin([&] {
        if (++polls == 2) polling.store(true, std::memory_order_release);
        return released.load(std::memory_order_acquire);
    });
    releaser.join();
    Require(caught.ready && catching.Window() < 1s && catching.Window() >= HandoffSite::Floor, "a caught hand-off did not shrink the window from the cap");
}

void CheckConvergence() {
    HandoffSite site{"converging", 1s, true};
    const auto shortHandoff = [&] {
        int polls = 0;
        return site.Spin([&] { return ++polls >= 2; });
    };
    const auto longHandoff = [&] { return site.Spin([] { return false; }); };
    const auto missesToStop = [](std::chrono::nanoseconds window) {
        int bound = 1;
        for (auto limit = window; limit >= HandoffSite::Floor; limit /= 2) ++bound;
        return bound;
    };
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int wait = 0; wait < 64; ++wait) Require(shortHandoff().ready, "a hand-off ready on the second poll was missed while hand-offs were short");
        const auto learned = site.Window();
        Require(learned >= HandoffSite::Floor && learned < 1s, "short hand-offs did not shrink the window from the cap");
        const auto bound = missesToStop(learned);
        int waits = 0;
        while (site.Window() > 0ns) {
            Require(!longHandoff().ready, "a hand-off that never arrived was reported as caught");
            Require(++waits <= bound, "the window did not reach zero within " + std::to_string(bound) + " waits after hand-offs became long");
        }
        int spun = 0;
        for (std::uint32_t wait = 0; wait < 2 * HandoffSite::ProbeEvery; ++wait) {
            const auto result = longHandoff();
            if (result.end != result.start) ++spun;
            Require(site.Window() == 0ns, "a probe that missed restarted the spin");
        }
        Require(spun >= 1 && spun <= 2, "long hand-offs were not probed once every " + std::to_string(HandoffSite::ProbeEvery) + " waits");
        waits = 0;
        for (;;) {
            ++waits;
            if (shortHandoff().ready) break;
            Require(waits <= static_cast<int>(HandoffSite::ProbeEvery), "a probe did not relearn short hand-offs within " + std::to_string(HandoffSite::ProbeEvery) + " waits");
        }
        Require(site.Window() >= HandoffSite::Floor, "a caught probe did not restart the spin");
    }
}

void CheckRecursion() {
    std::recursive_mutex mutex;
    LockSpinning(mutex, 100us);
    LockSpinning(mutex, 100us);
    mutex.unlock();
    mutex.unlock();
}

}

int main() {
    try {
        CheckWindowPolicy();
        CheckUsableProcessors();
        CheckSpinUntil();
        Contend<std::mutex>(0ns, "plain lock");
        Contend<std::mutex>(50us, "spin 50 us");
        Contend<std::mutex>(1ms, "spin 1 ms");
        Contend<std::recursive_mutex>(150us, "recursive mutex");
        CheckBlockingFallback();
        CheckRecursion();
        CheckHandoffSite();
        CheckAdaptation();
        CheckConvergence();
        std::cout << "handoff spin tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
