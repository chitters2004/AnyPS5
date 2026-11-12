#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/SequenceRing.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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
    Require(counters.worked == 4, "the ring did not count its prepared items");
    const auto again = ring.TakeCounters();
    Require(again.residency[0].items == 0 && again.worked == 0, "taking the counters did not reset them");
    Require(ring.CurrentTotals().worked == 4, "taking the counters reset the running totals the A/B windows read");
    Require(Throws([&] { ring.Push([] {}, {}, false, SequenceRing::Tags); }), "an out-of-range tag was accepted");
    SequenceRing untimed(Small(4));
    untimed.Push([] {}, {});
    untimed.Drain();
    Require(untimed.TakeCounters().residency[0].items == 0, "an untimed ring counted residency");
}

void ReclaimKeepsTheItemPushedIntoAFreedSlot() {
    constexpr std::uint64_t items = 200000;
    SequenceRing ring(Small(2));
    std::uint64_t ran = 0;
    for (std::uint64_t i = 0; i < items; ++i) ring.Push([&ran] { ++ran; }, {});
    ring.Drain();
    Require(ran == items, "an item pushed into a slot the preparer had just freed was cleared before it ran");
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


thread_local std::vector<std::uint64_t> outbox;

SequenceRing::Options Parallel(std::size_t capacity, std::size_t preparers, std::vector<std::uint64_t>& published) {
    SequenceRing::Options options;
    options.capacity = capacity;
    options.preparers = preparers;
    options.outbox = [&published] {
        return SequenceRing::Publish([&published, items = std::exchange(outbox, {})](bool keep) {
            if (keep) published.insert(published.end(), items.begin(), items.end());
        });
    };
    return options;
}

bool WaitFor(const std::atomic<bool>& flag) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::yield();
    }
    return true;
}

void PushOverlappingPair(SequenceRing& ring, std::atomic<bool>& overlapped, std::vector<std::uint64_t>* expected) {
    auto secondStarted = std::make_shared<std::atomic<bool>>(false);
    const auto first = ring.Push([&overlapped, secondStarted] {
        overlapped.store(WaitFor(*secondStarted), std::memory_order_release);
        outbox.push_back(SequenceRing::Running());
    }, {});
    const auto second = ring.Push([secondStarted] {
        secondStarted->store(true, std::memory_order_release);
        outbox.push_back(SequenceRing::Running());
    }, {});
    if (expected != nullptr) {
        expected->push_back(first);
        expected->push_back(second);
    }
}

void Spin(std::chrono::nanoseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until) {
    }
}

void ItemsPublishInSequenceOrder(std::size_t preparers) {
    std::vector<std::uint64_t> published;
    SequenceRing ring(Parallel(8, preparers, published));
    std::vector<std::uint64_t> expected;
    std::atomic<bool> overlapped{false};
    if (preparers > 1) PushOverlappingPair(ring, overlapped, &expected);
    for (std::uint64_t round = 0; round < 600; ++round) {
        if (round % 50 == 49) {
            ring.Drain();
            expected.push_back(ring.Bypass());
            published.push_back(expected.back());
            continue;
        }
        expected.push_back(ring.Push([&, slow = round % 3 == 0] {
            if (slow) std::this_thread::sleep_for(std::chrono::microseconds(200));
            outbox.push_back(SequenceRing::Running());
        }, {}));
    }
    ring.Drain();
    Require(published.size() == expected.size(), "with " + std::to_string(preparers) + " preparers " + std::to_string(published.size()) + " of " + std::to_string(expected.size()) + " items were published");
    Require(published == expected, "with " + std::to_string(preparers) + " preparers the items were not published in sequence order");
    if (preparers > 1) Require(overlapped.load(), "several preparers never prepared two items at once");
    Require(ring.Idle() && ring.PreparedSequence() <= ring.Issued(), "the prepared frontier did not reach the last item");
}

void OlderUnpublishedItemsAreVisible() {
    std::vector<std::uint64_t> published;
    SequenceRing ring(Parallel(8, 3, published));
    std::atomic<bool> open{false};
    const OpenOnExit opener{open};
    std::atomic<int> sawOlder{-1};
    std::atomic<int> sawSelf{-1};
    std::atomic<int> sawNewer{-1};
    SequenceRing::Held first;
    first.unknown = false;
    first.key = 1;
    first.writes = {{0x100, 0x104}};
    ring.Push([&] {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!open.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    }, std::move(first));
    SequenceRing::Held second;
    second.unknown = false;
    second.key = 2;
    ring.Push([&] {
        sawOlder = ring.AnyOlderNewestFirst([](const SequenceRing::Held& held) { return held.key == 1 && held.writes.front().first == 0x100; }) ? 1 : 0;
        sawSelf = ring.AnyOlderNewestFirst([](const SequenceRing::Held& held) { return held.key == 2; }) ? 1 : 0;
        sawNewer = ring.AnyOlderNewestFirst([](const SequenceRing::Held& held) { return held.key == 3; }) ? 1 : 0;
        open.store(true, std::memory_order_release);
    }, std::move(second));
    SequenceRing::Held third;
    third.key = 3;
    ring.Push([] {}, std::move(third));
    ring.Drain();
    Require(sawOlder.load() == 1, "a preparer did not see an older item that was prepared but not yet published");
    Require(sawSelf.load() == 0 && sawNewer.load() == 0, "a preparer saw its own item or a newer one as older");
    std::atomic<int> afterDrain{-1};
    ring.Push([&] { afterDrain = ring.AnyOlderNewestFirst([](const SequenceRing::Held&) { return true; }) ? 1 : 0; }, {});
    ring.Drain();
    Require(afterDrain.load() == 0, "a published item stayed visible to later preparers");
    Require(!ring.AnyOlderNewestFirst([](const SequenceRing::Held&) { return true; }), "the producer saw items through the preparers' older view");
}

void FailureCancelsLaterPublishes(std::size_t preparers) {
    std::vector<std::uint64_t> published;
    auto options = Parallel(8, preparers, published);
    std::atomic<int> reports{0};
    options.failed = [&](std::exception_ptr) { reports.fetch_add(1); };
    SequenceRing ring(std::move(options));
    std::vector<std::uint64_t> expected;
    expected.push_back(ring.Push([] { outbox.push_back(SequenceRing::Running()); }, {}));
    expected.push_back(ring.Push([] {
        outbox.push_back(SequenceRing::Running());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        throw std::runtime_error("prepare failed");
    }, {}));
    for (int i = 0; i < 6; ++i) ring.Push([] { outbox.push_back(SequenceRing::Running()); }, {});
    bool thrown = false;
    try {
        ring.Drain();
    } catch (const std::runtime_error& error) {
        thrown = std::string(error.what()) == "prepare failed";
    }
    Require(thrown, "the failed preparation was not rethrown to the producer");
    Require(reports.load() == 1, "the failure was reported " + std::to_string(reports.load()) + " times");
    Require(published == expected, "an item after a failed one was published, or the failed item's own commits were dropped");
    expected.push_back(ring.Push([] { outbox.push_back(SequenceRing::Running()); }, {}));
    ring.Drain();
    Require(published == expected, "the ring did not resume publishing after the failure was rethrown");
}

void AwaitTurnWaitsForOlderPublishes() {
    std::vector<std::uint64_t> published;
    std::atomic<std::uint64_t> publishedThrough{0};
    auto options = Parallel(8, 3, published);
    options.outbox = [&] {
        return SequenceRing::Publish([&, items = std::exchange(outbox, {})](bool keep) {
            if (!keep) return;
            published.insert(published.end(), items.begin(), items.end());
            if (!items.empty()) publishedThrough.store(items.back(), std::memory_order_release);
        });
    };
    SequenceRing ring(std::move(options));
    Require(Throws([&] { ring.AwaitTurn(); }), "the producer waited for a preparer's turn");
    std::atomic<std::uint64_t> seenAtTurn{0};
    const auto first = ring.Push([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        outbox.push_back(SequenceRing::Running());
    }, {});
    ring.Push([&] {
        ring.AwaitTurn();
        seenAtTurn = publishedThrough.load(std::memory_order_acquire);
        outbox.push_back(SequenceRing::Running());
    }, {});
    ring.Drain();
    Require(seenAtTurn.load() == first, "an item's turn came before the older item was published");
    std::atomic<bool> cancelled{false};
    ring.Push([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        throw std::runtime_error("boom");
    }, {});
    ring.Push([&] {
        try {
            ring.AwaitTurn();
        } catch (const SequenceRing::Cancelled&) {
            cancelled = true;
            throw;
        }
    }, {});
    Require(Throws([&] { ring.Drain(); }), "the failure before a waiting item was not rethrown");
    Require(cancelled.load(), "an item waiting for its turn behind a failed one was not cancelled");
}

void ReclaimSurvivesOlderVisits() {
    std::vector<std::uint64_t> published;
    SequenceRing ring(Parallel(4, 3, published));
    std::atomic<bool> cleared{false};
    std::atomic<std::uint64_t> visited{0};
    for (std::uint64_t i = 1; i <= 100000; ++i) {
        SequenceRing::Held held;
        held.unknown = false;
        held.key = i;
        held.writes = {{i, i + 1}};
        ring.Push([&] {
            ring.AnyOlderNewestFirst([&](const SequenceRing::Held& older) {
                if (older.key == 0 || older.writes.size() != 1 || older.writes.front().first != older.key) cleared.store(true, std::memory_order_relaxed);
                visited.fetch_add(1, std::memory_order_relaxed);
                return false;
            });
        }, std::move(held));
    }
    ring.Drain();
    Require(!cleared.load(), "the producer reclaimed an older slot while a preparer was reading it");
    std::cout << "older items visited while preparing: " << visited.load() << '\n';
}

std::atomic<bool> singlePreparer{false};

void SinglePreparerSwitchParksTheOthers() {
    std::vector<std::uint64_t> published;
    auto options = Parallel(8, 3, published);
    options.single = [] { return singlePreparer.load(std::memory_order_relaxed); };
    singlePreparer = true;
    SequenceRing ring(std::move(options));
    std::mutex idsMutex;
    std::set<std::thread::id> ids;
    const auto push = [&] {
        ring.Push([&] {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            std::lock_guard lock(idsMutex);
            ids.insert(std::this_thread::get_id());
        }, {});
    };
    for (int i = 0; i < 60; ++i) push();
    ring.Drain();
    Require(ids.size() == 1, "with the single-preparer switch set, " + std::to_string(ids.size()) + " preparers ran items");
    singlePreparer = false;
    std::atomic<bool> overlapped{false};
    PushOverlappingPair(ring, overlapped, nullptr);
    ring.Drain();
    Require(overlapped.load(), "with the single-preparer switch clear, only one preparer ran items");
    singlePreparer = true;
    ids.clear();
    for (int i = 0; i < 20; ++i) push();
    ring.Drain();
    Require(ids.size() == 1, "setting the single-preparer switch again did not park the other preparers");
    singlePreparer = false;
}

void ParallelTeardownPublishesWaitingItems() {
    std::vector<std::uint64_t> published;
    std::vector<std::uint64_t> expected;
    {
        SequenceRing ring(Parallel(16, 3, published));
        for (int i = 0; i < 12; ++i) {
            expected.push_back(ring.Push([] {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                outbox.push_back(SequenceRing::Running());
            }, {}));
        }
    }
    Require(published == expected, "a ring with several preparers destroyed with waiting items did not publish all of them in order");
}

void EarlyRethrowStillCancelsItemsThatRanBeforeTheLatch() {
    std::vector<std::uint64_t> published;
    auto options = Parallel(8, 3, published);
    SequenceRing* self = nullptr;
    std::atomic<bool> rethrown{false};
    options.failed = [&](std::exception_ptr) {
        try {
            self->RethrowFailure();
        } catch (const std::runtime_error&) {
            rethrown.store(true, std::memory_order_release);
        }
    };
    SequenceRing ring(std::move(options));
    self = &ring;
    std::atomic<bool> laterRan{false};
    std::atomic<bool> overlapped{false};
    ring.Push([&] {
        overlapped.store(WaitFor(laterRan), std::memory_order_release);
        throw std::runtime_error("the older item failed");
    }, {});
    const auto later = ring.Push([&] {
        outbox.push_back(SequenceRing::Running());
        laterRan.store(true, std::memory_order_release);
    }, {});
    ring.Settle();
    Require(overlapped.load() && rethrown.load(), "the later item did not run before the older one failed, or the failure was not rethrown at the latch");
    Require(std::find(published.begin(), published.end(), later) == published.end(), "an item that ran before an older failure was published after the failure was rethrown");
    ring.Drain();
}

void SimultaneousFinishesNeverStrandAnItem() {
    std::vector<std::uint64_t> published;
    SequenceRing ring(Parallel(8, 3, published));
    std::uint64_t expected = 0;
    for (int round = 0; round < 20000; ++round) {
        for (int item = 0; item < 3; ++item) {
            ring.Push([] { outbox.push_back(SequenceRing::Running()); }, {});
        }
        expected += 3;
        WaitUntil([&] { return ring.Idle(); }, "the last of several simultaneously finished items to be published");
        Require(published.size() == expected, "an item finished while another preparer was publishing was never published");
    }
}

void Bench(std::chrono::nanoseconds work, std::uint64_t items, std::chrono::nanoseconds spin) {
    for (std::size_t preparers = 1; preparers <= 3; ++preparers) {
        std::vector<std::uint64_t> published;
        published.reserve(items);
        auto options = Parallel(32, preparers, published);
        options.timed = true;
        options.spin = spin;
        SequenceRing ring(std::move(options));
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < items; ++i) {
            ring.Push([work] {
                Spin(work);
                outbox.push_back(SequenceRing::Running());
            }, {});
        }
        ring.Drain();
        const auto wallNs = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        const auto counters = ring.TakeCounters();
        Require(published.size() == items, "the bench lost items");
        std::printf("[ring-bench] work %.1f us, spin %.0f us, preparers %zu: %.2f us per item, %.0f items/s, published by another preparer %llu, full waits %llu, preparer sleeps %llu\n", static_cast<double>(work.count()) / 1000.0, static_cast<double>(spin.count()) / 1000.0, preparers, wallNs / 1000.0 / static_cast<double>(items), static_cast<double>(items) * 1e9 / wallNs, static_cast<unsigned long long>(counters.publishedForOthers), static_cast<unsigned long long>(counters.fullWaits), static_cast<unsigned long long>(counters.preparerSleeps));
    }
}

}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--bench") {
            const auto workUs = argc > 2 ? std::strtod(argv[2], nullptr) : 25.0;
            const auto items = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 20000ull;
            const auto spin = std::chrono::nanoseconds(static_cast<std::int64_t>((argc > 4 ? std::strtod(argv[4], nullptr) : 0.0) * 1000.0));
            Bench(std::chrono::nanoseconds(static_cast<std::int64_t>(workUs * 1000.0)), items, spin);
            Bench(std::chrono::nanoseconds::zero(), items * 10, spin);
            return 0;
        }
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
        ReclaimKeepsTheItemPushedIntoAFreedSlot();
        ResidencyIsCountedPerTag();
        FirstFailureIsReportedAtOnce();
        for (std::size_t preparers = 1; preparers <= 3; ++preparers) {
            ItemsPublishInSequenceOrder(preparers);
            FailureCancelsLaterPublishes(preparers);
        }
        OlderUnpublishedItemsAreVisible();
        AwaitTurnWaitsForOlderPublishes();
        ReclaimSurvivesOlderVisits();
        SinglePreparerSwitchParksTheOthers();
        ParallelTeardownPublishesWaitingItems();
        SimultaneousFinishesNeverStrandAnItem();
        EarlyRethrowStillCancelsItemsThatRanBeforeTheLatch();
        std::cout << "sequence ring tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
