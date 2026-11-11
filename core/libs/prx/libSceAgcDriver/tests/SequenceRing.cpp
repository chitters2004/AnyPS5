#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/SequenceRing.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::DriverDetail::SequenceRing;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

SequenceRing::Options Small(std::size_t capacity, std::chrono::nanoseconds spin = {}) {
    SequenceRing::Options options;
    options.capacity = capacity;
    options.spin = spin;
    return options;
}

template<typename TCall>
bool Throws(TCall&& call) {
    try {
        call();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

struct OpenOnExit {
    std::atomic<bool>& open;
    ~OpenOnExit() { open.store(true, std::memory_order_release); }
};

void WaitUntil(const std::function<bool()>& ready, const char* what) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready()) {
        if (std::chrono::steady_clock::now() > until) throw std::runtime_error(std::string("timed out waiting for ") + what);
        std::this_thread::yield();
    }
}

void ItemsRunInSequenceOrder(std::chrono::nanoseconds spin) {
    SequenceRing ring(Small(8, spin));
    std::vector<std::uint64_t> ran;
    std::vector<std::uint64_t> running;
    std::vector<std::uint64_t> expected;
    for (std::uint64_t round = 0; round < 2000; ++round) {
        if (round % 7 == 0) {
            ring.Drain();
            expected.push_back(ring.Bypass());
            ran.push_back(expected.back());
            running.push_back(expected.back());
            continue;
        }
        const auto sequence = ring.Push([&ran, &running] {
            running.push_back(SequenceRing::Running());
            ran.push_back(SequenceRing::Running());
        }, {});
        expected.push_back(sequence);
    }
    ring.Drain();
    Require(ran.size() == expected.size(), "an item was lost or ran twice");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        Require(expected[i] == i + 1, "sequence numbers are not issued one after another");
        Require(ran[i] == expected[i], "item " + std::to_string(expected[i]) + " ran out of sequence order");
        Require(running[i] == expected[i], "an item saw the wrong running sequence");
    }
    Require(SequenceRing::Running() == 0, "the producer sees a running sequence");
    Require(ring.PreparedSequence() <= ring.Issued(), "the prepared sequence passed the issued one");
}

void MixedItemsKeepTheirOrder() {
    enum class Kind { Draw, Label, Dma, Flip, SubmissionEnd };
    SequenceRing ring(Small(4));
    std::vector<std::pair<Kind, std::uint64_t>> committed;
    std::mutex committedMutex;
    std::vector<std::pair<Kind, std::uint64_t>> expected;
    const auto push = [&](Kind kind, bool slow) {
        const auto sequence = ring.Push([&, kind, slow] {
            if (slow) std::this_thread::sleep_for(std::chrono::microseconds(200));
            std::lock_guard lock(committedMutex);
            committed.emplace_back(kind, SequenceRing::Running());
        }, {});
        expected.emplace_back(kind, sequence);
    };
    for (int frame = 0; frame < 20; ++frame) {
        push(Kind::Draw, true);
        push(Kind::Label, false);
        push(Kind::Draw, true);
        push(Kind::Dma, false);
        push(Kind::Draw, frame % 2 == 0);
        push(Kind::Label, false);
        push(Kind::SubmissionEnd, false);
        ring.Drain();
        const auto flip = ring.Bypass();
        {
            std::lock_guard lock(committedMutex);
            committed.emplace_back(Kind::Flip, flip);
        }
        expected.emplace_back(Kind::Flip, flip);
    }
    ring.Drain();
    Require(committed == expected, "labels, DMA, flips or submission ends left their place between the draws");
}

void FullRingBlocksTheProducer() {
    constexpr std::size_t capacity = 4;
    std::atomic<bool> open{false};
    SequenceRing ring(Small(capacity));
    const OpenOnExit opener{open};
    std::atomic<std::size_t> pushes{0};
    std::atomic<std::size_t> ran{0};
    std::exception_ptr producerError;
    std::thread producer([&] {
        try {
            for (std::size_t i = 0; i < capacity * 3; ++i) {
                ring.Push([&] {
                    while (!open.load(std::memory_order_acquire)) std::this_thread::yield();
                    ran.fetch_add(1);
                }, {});
                pushes.fetch_add(1);
            }
            ring.Drain();
        } catch (...) {
            producerError = std::current_exception();
        }
    });
    struct JoinOnExit {
        std::thread& thread;
        std::atomic<bool>& open;
        ~JoinOnExit() {
            open.store(true, std::memory_order_release);
            if (thread.joinable()) thread.join();
        }
    } joiner{producer, open};
    WaitUntil([&] { return pushes.load() == capacity; }, "the ring to fill");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Require(pushes.load() == capacity, "the producer pushed " + std::to_string(pushes.load()) + " items into a ring of " + std::to_string(capacity));
    Require(ring.Waiting() == capacity, "a full ring does not report every slot waiting");
    open.store(true, std::memory_order_release);
    producer.join();
    if (producerError != nullptr) std::rethrow_exception(producerError);
    Require(ran.load() == capacity * 3, "items were lost behind a full ring");
    Require(ring.TakeCounters().fullWaits != 0, "the full waits were not counted");
}

void FullRingRefusesTheGpuLockHolder() {
    std::atomic<bool> open{false};
    SequenceRing ring(Small(2));
    const OpenOnExit opener{open};
    for (int i = 0; i < 2; ++i) {
        ring.Push([&] {
            while (!open.load(std::memory_order_acquire)) std::this_thread::yield();
        }, {});
    }
    std::atomic<bool> refused{false};
    std::thread late([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        open.store(true, std::memory_order_release);
    });
    refused = Throws([&] { ring.Push([] {}, {}, true); });
    late.join();
    Require(refused, "a producer holding the GPU lock waited for room in a full ring");
    open.store(true, std::memory_order_release);
    ring.Drain();
    ring.Push([] {}, {}, true);
    ring.Drain();
}

void FailureSkipsLaterItemsUntilRethrown() {
    SequenceRing ring(Small(8));
    std::vector<int> ran;
    ring.Push([&] { ran.push_back(1); }, {});
    ring.Push([&] {
        ran.push_back(2);
        throw std::runtime_error("prepare failed");
    }, {});
    ring.Push([&] { ran.push_back(3); }, {});
    bool thrown = false;
    try {
        ring.Drain();
    } catch (const std::runtime_error& error) {
        thrown = std::string(error.what()) == "prepare failed";
    }
    Require(thrown, "the preparer's failure was not rethrown to the producer");
    Require(ran == std::vector<int>{1, 2}, "an item after a failed one ran before the failure was rethrown");
    ring.Drain();
    ring.Push([&] { ran.push_back(4); }, {});
    ring.Drain();
    Require(ran == std::vector<int>{1, 2, 4}, "the ring did not resume after the failure was rethrown");
    ring.Push([] { throw std::runtime_error("second failure"); }, {});
    ring.Settle();
    Require(Throws([&] { ring.Push([] {}, {}); }), "a failure was not rethrown by the next push");
}

void TeardownRunsWaitingItems() {
    std::atomic<int> ran{0};
    {
        SequenceRing ring(Small(8));
        for (int i = 0; i < 6; ++i) {
            ring.Push([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                ran.fetch_add(1);
            }, {});
        }
    }
    Require(ran.load() == 6, "a ring destroyed with waiting items dropped " + std::to_string(6 - ran.load()) + " of them");
    SequenceRing unused(Small(2));
    unused.Stop();
    unused.Stop();
    Require(Throws([&] { unused.Push([] {}, {}); }), "a stopped ring accepted an item");
    SequenceRing stopped(Small(2));
    stopped.Push([] {}, {});
    stopped.Stop();
    Require(stopped.Idle(), "a stopped ring kept an item waiting");
}

void OneProducerOnly() {
    std::atomic<bool> open{false};
    SequenceRing ring(Small(4));
    const OpenOnExit opener{open};
    ring.Push([&] {
        while (!open.load(std::memory_order_acquire)) std::this_thread::yield();
    }, {});
    bool refused = false;
    std::thread other([&] { refused = Throws([&] { ring.Push([] {}, {}); }); });
    other.join();
    Require(refused, "a second thread produced into a ring with waiting items");
    Require(Throws([&] { ring.Bypass(); }), "an item bypassed waiting items");
    open.store(true, std::memory_order_release);
    ring.Drain();
    bool selfPush = false;
    ring.Push([&] { selfPush = Throws([&] { ring.Push([] {}, {}); }); }, {});
    ring.Drain();
    Require(selfPush, "the preparer produced into its own ring");
    std::thread handover([&] { ring.Push([] {}, {}); ring.Drain(); });
    handover.join();
}

void WaitingItemsAreVisibleNewestFirst() {
    std::atomic<bool> open{false};
    SequenceRing ring(Small(8));
    const OpenOnExit opener{open};
    for (std::uint64_t i = 1; i <= 3; ++i) {
        SequenceRing::Held held;
        held.unknown = false;
        held.key = i;
        held.writes = {{i * 0x100, i * 0x100 + 4}};
        ring.Push([&] {
            while (!open.load(std::memory_order_acquire)) std::this_thread::yield();
        }, std::move(held));
    }
    std::vector<std::uint64_t> seen;
    ring.AnyNewestFirst([&](const SequenceRing::Held& held) {
        seen.push_back(held.key);
        return false;
    });
    Require(seen.size() >= 2 && seen.front() == 3, "waiting items are not visited newest first");
    Require(ring.AnyNewestFirst([](const SequenceRing::Held& held) { return held.writes.front().first == 0x300; }), "a waiting item's writes are not visible");
    open.store(true, std::memory_order_release);
    ring.Drain();
    Require(!ring.AnyNewestFirst([](const SequenceRing::Held&) { return true; }), "a prepared item stayed visible");
}

void FirstFailureIsReportedAtOnce() {
    std::atomic<int> reports{0};
    std::string reported;
    auto options = Small(4);
    options.failed = [&](std::exception_ptr error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& caught) {
            reported = caught.what();
        }
        reports.fetch_add(1);
    };
    std::atomic<bool> open{false};
    SequenceRing ring(std::move(options));
    const OpenOnExit opener{open};
    ring.Push([&] {
        while (!open.load(std::memory_order_acquire)) std::this_thread::yield();
        throw std::runtime_error("first");
    }, {});
    ring.Push([] { throw std::runtime_error("second"); }, {});
    open.store(true, std::memory_order_release);
    ring.Settle();
    Require(reports.load() == 1 && reported == "first", "the first failed preparation was not reported once when it happened");
    bool rethrown = false;
    try {
        ring.Drain();
    } catch (const std::runtime_error& error) {
        rethrown = std::string(error.what()) == "first";
    }
    Require(rethrown, "the reported failure was not also rethrown to the producer");
}

void ResidencyIsCountedPerTag() {
    auto options = Small(8);
    options.timed = true;
    SequenceRing ring(std::move(options));
    for (int i = 0; i < 3; ++i) ring.Push([] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }, {}, false, 0);
    ring.Push([] {}, {}, false, 5);
    ring.Drain();
    const auto counters = ring.TakeCounters();
    Require(counters.residency[0].items == 3 && counters.residency[5].items == 1, "residency was not counted under each item's tag");
    Require(counters.residency[0].totalNs >= 3'000'000 && counters.residency[0].longestNs >= 1'000'000, "residency does not cover the time an item waited and ran");
    Require(counters.residency[5].totalNs >= 1'000'000, "an item behind slower ones does not count the time it waited in the ring");
    Require(ring.TakeCounters().residency[0].items == 0, "taking the counters did not reset them");
    Require(Throws([&] { ring.Push([] {}, {}, false, SequenceRing::Tags); }), "an out-of-range tag was accepted");
    SequenceRing untimed(Small(4));
    untimed.Push([] {}, {});
    untimed.Drain();
    Require(untimed.TakeCounters().residency[0].items == 0, "an untimed ring counted residency");
}

void PreparedWorkIsReleasedByTheProducer() {
    SequenceRing ring(Small(4));
    auto held = std::make_shared<int>(1);
    const std::weak_ptr<int> watched = held;
    ring.Push([held = std::move(held)] {}, {});
    ring.Drain();
    Require(watched.expired(), "a prepared item's work outlived the producer's drain");
    auto second = std::make_shared<int>(2);
    const std::weak_ptr<int> secondWatched = second;
    ring.Push([second = std::move(second)] {}, {});
    ring.Settle();
    ring.Push([] {}, {});
    Require(secondWatched.expired(), "a prepared item's work outlived the next push");
    ring.Drain();
}

}

int main() {
    try {
        ItemsRunInSequenceOrder(std::chrono::nanoseconds::zero());
        ItemsRunInSequenceOrder(std::chrono::microseconds(50));
        MixedItemsKeepTheirOrder();
        FullRingBlocksTheProducer();
        FullRingRefusesTheGpuLockHolder();
        FailureSkipsLaterItemsUntilRethrown();
        TeardownRunsWaitingItems();
        OneProducerOnly();
        WaitingItemsAreVisibleNewestFirst();
        PreparedWorkIsReleasedByTheProducer();
        ResidencyIsCountedPerTag();
        FirstFailureIsReportedAtOnce();
        std::cout << "sequence ring tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
