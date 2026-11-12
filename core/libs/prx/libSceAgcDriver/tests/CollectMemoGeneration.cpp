#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr std::uint64_t Block = 65536;
constexpr std::uint64_t Page = 4096;
constexpr std::uint64_t Span = 128 * Block;
std::uint64_t Base = std::uint64_t{1} << 32;
constexpr std::chrono::seconds Patience{10};

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

#ifndef _WIN32
struct FakeWatch {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> written;
    bool holdNextWalk = false;
    bool holding = false;
    bool released = false;
    bool timedOut = false;
    bool failWalks = false;
    std::size_t walks = 0;
};

FakeWatch& Fake() {
    static FakeWatch fake;
    return fake;
}
#endif

}

#ifndef _WIN32
namespace GuestWriteWatch {

extern "C" {

bool GuestWriteWatchAvailable_nid_postfix() {
    return true;
}

bool GuestWriteWatchUnregister_nid_postfix(const void*, std::size_t) {
    return true;
}

bool GuestWriteWatchCovers_nid_postfix(std::uintptr_t address, std::size_t bytes) {
    return address >= Base && bytes <= Span && address - Base <= Span - bytes;
}

bool GuestWriteWatchCollect_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context) {
    auto& fake = Fake();
    const auto end = address + bytes;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> runs;
    {
        std::unique_lock lock(fake.mutex);
        ++fake.walks;
        if (fake.failWalks) return false;
        if (fake.holdNextWalk) {
            fake.holdNextWalk = false;
            fake.holding = true;
            fake.changed.notify_all();
            fake.timedOut = !fake.changed.wait_for(lock, Patience, [&] { return fake.released; });
        }
        for (auto it = fake.written.begin(); it != fake.written.end();) {
            if (it->first < end && address < it->second) {
                runs.emplace_back(std::max(it->first, address), std::min(it->second, end));
                it = fake.written.erase(it);
            } else ++it;
        }
    }
    for (const auto& [from, to] : runs) written(context, from, to);
    return true;
}

}

}
#endif

namespace {

using namespace AgcDriver::GuestMemory;

#ifndef _WIN32
void CheckMemoHitDuringWalk() {
    auto& fake = Fake();
    BumpCollectEpoch();
    const auto collected = CollectWrites(Base, Block);
    Require(collected != 0 && UnchangedSince(Base, Block, collected), "the block was not collected");
    {
        const std::lock_guard lock(fake.mutex);
        fake.written.emplace_back(Base + Page, Base + 2 * Page);
        fake.holdNextWalk = true;
    }
    std::thread walker([] { CollectWritesUncached(Base, 2 * Block); });
    bool holding = false;
    {
        std::unique_lock lock(fake.mutex);
        holding = fake.changed.wait_for(lock, Patience, [&] { return fake.holding; });
    }
    const auto memoized = holding ? CollectWrites(Base, Block) : 0;
    {
        const std::lock_guard lock(fake.mutex);
        fake.released = true;
    }
    fake.changed.notify_all();
    walker.join();
    Require(holding, "the second walk did not reach the write watch");
    Require(!fake.timedOut, "the memoized collect waited for the walk in progress");
    Require(!UnchangedSince(Base + Page, Page, memoized), "a memoized collect returned the generation a walk in progress then stamped a write with");
    BumpCollectEpoch();
    Require(CollectWrites(Base, Block) > memoized && !UnchangedSince(Base, Block, memoized), "a later epoch trusts the write stamped by the walk in progress");
}

std::size_t Walks() {
    auto& fake = Fake();
    const std::lock_guard lock(fake.mutex);
    return fake.walks;
}

void SetFailing(bool failing) {
    auto& fake = Fake();
    const std::lock_guard lock(fake.mutex);
    fake.failWalks = failing;
}

void CheckFailedWalkIsRetried() {
    BumpCollectEpoch();
    const auto address = Base + 11 * Page + 16;
    SetFailing(true);
    Require(CollectWrites(address, Page) == 0, "a failed walk returned a generation");
    SetFailing(false);
    const auto before = Walks();
    Require(CollectWrites(address, Page) != 0 && Walks() > before, "a failed walk was memoized");
}

void Store(std::uint64_t address) {
    auto& fake = Fake();
    const std::lock_guard lock(fake.mutex);
    fake.written.emplace_back(address & ~(Page - 1), (address & ~(Page - 1)) + Page);
}
#else
struct Arena {
    static constexpr std::size_t Bytes = 96 * Block;
    void* memory = nullptr;
    Arena() {
        memory = GuestArena::GuestArenaAllocate_nid_postfix(Bytes, Block);
        Require(memory != nullptr, "the guest arena allocation failed");
        GuestArena::GuestArenaCommit_nid_postfix(memory, Bytes, PAGE_READWRITE, Bytes);
        std::memset(memory, 0x11, Bytes);
        Base = reinterpret_cast<std::uint64_t>(memory);
    }
    ~Arena() {
        GuestArena::GuestArenaReset_nid_postfix(memory, Bytes);
        GuestArena::GuestArenaRelease_nid_postfix(memory, Bytes);
    }
};

void Store(std::uint64_t address) {
    auto* byte = reinterpret_cast<volatile std::uint8_t*>(address);
    *byte = static_cast<std::uint8_t>(*byte + 1);
}
#endif

bool Walked(std::uint64_t address, std::size_t bytes, std::uint64_t probe, bool cached = true) {
    const auto before = TrackerGeneration();
    Store(probe);
    Require((cached ? CollectWrites(address, bytes) : CollectWritesUncached(address, bytes)) != 0, "a collect failed");
    return !UnchangedSince(probe & ~(Page - 1), Page, before);
}

void CheckUnalignedRepeatReachesKernelOnce() {
    BumpCollectEpoch();
    const auto address = Base + 3 * Page + 100;
    const auto probe = Base + 5 * Page;
    Require(Walked(address, 2 * Page, probe), "the first collect did not reach the write watch");
    for (std::uint64_t i = 0; i < 70; ++i) Require(CollectWrites(Base + (16 + i) * Block + 5 * Page + 8, Page) != 0, "a filler collect failed");
    Require(!Walked(address, 2 * Page, probe), "an identical unaligned request in the same epoch reached the write watch again");
    Require(!Walked(Base + 9 * Page, Page, probe), "a request inside the widened block reached the write watch again");
}

void CheckUncachedWalksKeepCachedEntries() {
    BumpCollectEpoch();
    Require(CollectWrites(Base, 2 * Block) != 0, "the large collect failed");
    const auto sub = Base + Block + Page;
    Require(!Walked(sub, Page, sub), "a request inside the large walk reached the write watch");
    for (int i = 0; i < 80; ++i) Require(CollectWritesUncached(Base, Page) != 0, "an uncached collect failed");
    Require(!Walked(sub, Page, sub), "uncached walks evicted a cached entry");
}

void CheckNeighbouringBlockStoreIsFound() {
    BumpCollectEpoch();
    Require(CollectWrites(Base + 24 * Block + 5 * Page + 8, Page) != 0, "the block was not collected");
    const auto across = Base + 25 * Block - Page - 100;
    Require(Walked(across, 2 * Page, Base + 25 * Block + 3 * Page), "a request reaching past the retained block trusted it");
}

void CheckInvalidationReachesBlockKeyedEntry() {
    BumpCollectEpoch();
    const auto address = Base + 30 * Block + 3 * Page + 100;
    const auto probe = Base + 30 * Block + 5 * Page;
    Require(Walked(address, Page, probe), "the first collect did not reach the write watch");
    Require(!Walked(address, Page, probe), "the retained entry was not reused");
    Unwatch(Base + 90 * Block, Page);
    Require(Walked(address, Page, probe), "an unwatch did not invalidate the retained entry");
}

void CheckThreadWithoutEpochWalksEveryTime() {
    bool first = false;
    bool second = false;
    const auto address = Base + 35 * Block + 3 * Page + 100;
    std::thread worker([&] {
        first = Walked(address, Page, address);
        second = Walked(address, Page, address);
    });
    worker.join();
    Require(first && second, "a thread that never bumped its epoch reused an entry");
}

void CheckStoreInNewEpochIsFound() {
    BumpCollectEpoch();
    const auto address = Base + 7 * Page + 8;
    Require(Walked(address, Page, address), "the first collect did not reach the write watch");
    BumpCollectEpoch();
    Require(Walked(address, Page, address), "a store in a new epoch was not found");
}

void CheckInteriorCoverageAfterRingEviction() {
    BumpCollectEpoch();
    Require(CollectWrites(Base, 8 * Block) != 0, "the covering collection failed");
    for (std::uint64_t i = 0; i < 70; ++i) Require(CollectWrites(Base + (16 + i) * Block, Block) != 0, "a ring eviction collection failed");
    const auto address = Base + 3 * Block + 3 * Page + 100;
    Require(!Walked(address, 2 * Page, address + Page), "an interior range lost its completed observation after ring eviction");
}

void CheckAdjacentCoverage() {
    BumpCollectEpoch();
    Require(CollectWrites(Base + 2 * Block, 2 * Block) != 0, "the right collection failed");
    Require(CollectWrites(Base, 2 * Block) != 0, "the left collection failed");
    Require(!Walked(Base + Page, 4 * Block - 2 * Page, Base + Block + Page), "adjacent completed observations were collected again");

    BumpCollectEpoch();
    Require(CollectWrites(Base, Block) != 0, "the first collection failed");
    Require(CollectWrites(Base + 2 * Block, Block) != 0, "the last collection failed");
    Require(CollectWrites(Base + Block, Block) != 0, "the connecting collection failed");
    Require(!Walked(Base, 3 * Block, Base + Block + Page), "a completed connection did not join both observations");
}

void CheckOverlappingCoverage() {
    BumpCollectEpoch();
    Require(CollectWrites(Base + 4 * Block, 4 * Block) != 0, "the right collection failed");
    Require(CollectWrites(Base + 2 * Block, 4 * Block) != 0, "the overlapping collection failed");
    Require(!Walked(Base + 3 * Block, 4 * Block, Base + 5 * Block + Page), "overlapping completed observations were collected again");
    Require(CollectWrites(Base + Block, 8 * Block) != 0, "the containing collection failed");
    Require(!Walked(Base + 2 * Block, 6 * Block, Base + 6 * Block + Page), "a containing observation lost its interior coverage");
}

void CheckCoverageGapsAndInvalidations() {
    BumpCollectEpoch();
    Require(CollectWrites(Base, 2 * Block) != 0, "the left collection failed");
    Require(CollectWrites(Base + 3 * Block, 2 * Block) != 0, "the right collection failed");
    Require(Walked(Base + Block, 3 * Block, Base + 2 * Block + Page), "a hole between observations was treated as observed");
    Require(CollectWrites(Base, 0) == 0, "an empty range acquired a coverage proof");
    Require(CollectWrites(std::numeric_limits<std::uint64_t>::max() - 31, 8) == 0, "page rounding wrapped an invalid address into observed memory");
    Require(CollectWrites(std::numeric_limits<std::uint64_t>::max() - 31, 64) == 0, "an overflowing range acquired a coverage proof");

    BumpCollectEpoch();
    const auto probe = Base + 2 * Block + Page;
    Require(Walked(Base + Block, 3 * Block, probe), "a new epoch inherited an old observation");
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(static_cast<std::uintptr_t>(Base + 90 * Block), Page);
    Require(Walked(Base + Block, 3 * Block, probe), "an allocation change retained an old observation");
    Unwatch(Base + 91 * Block, Page);
    Require(Walked(Base + Block, 3 * Block, probe), "an unwatch retained a merged observation");
}

void CheckCoverageThreadAndDriverWrites() {
    BumpCollectEpoch();
    Require(CollectWrites(Base, 4 * Block) != 0, "the covering collection failed");
    bool walked = false;
    std::thread other([&] {
        BumpCollectEpoch();
        walked = Walked(Base + Block, 2 * Block, Base + Block + Page);
    });
    other.join();
    Require(walked, "a second worker inherited the first worker's observation");
    const auto before = TrackerGeneration();
    const auto written = MarkWritten(Base + 2 * Block, Page);
    Require(written > before, "the driver write did not advance the tracker");
    Require(CollectWrites(Base + Block, 2 * Block) >= written, "an observation hid a completed driver write generation");
    Require(!UnchangedSince(Base + 2 * Block, Page, before), "an observation hid a driver write stamp");
    Require(Walked(Base + Block, 2 * Block, Base + Block + Page, false), "an uncached collection reused an observation");
}


std::shared_ptr<WriteSubscription> Subscribe(std::uint64_t address, std::uint64_t blocks, std::uint32_t mask = WriteChangeAll, bool history = true) {
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> ranges{{{address, address + blocks * Block}}};
    return SubscribeWrites(ranges, mask, history);
}

void Pending(std::uint64_t address) {
    NoteGpuPending(address, 4);
}

void CheckObserveAndArm() {
    const auto subscription = Subscribe(Base, 3);
    const auto seen = ObserveSubscription(*subscription);
    Require((seen & 1u) != 0 && ObserveSubscription(*subscription) == seen, "an observe without a bump changed the version");
    Pending(Base);
    const auto moved = ObserveSubscription(*subscription);
    Require(moved != seen, "a bump after an observe did not move the version the reader holds");
    Pending(Base);
    Require(ObserveSubscription(*subscription) != moved, "a bump after the next observe did not move the version again");
    const auto before = ObserveSubscription(*subscription);
    const auto since = SubscriptionGeneration(*subscription);
    Pending(Base + Block);
    Pending(Base + 2 * Block);
    const auto after = ObserveSubscription(*subscription);
    Require(after == before + 2, "two bumps without a read in between moved the version " + std::to_string((after - before) / 2) + " times instead of once");
    std::vector<std::uint64_t> plain;
    std::vector<std::uint64_t> lookup;
    Require(SubscriptionBumpsSince(*subscription, since, plain, lookup) && plain == std::vector<std::uint64_t>{WriteBlockOf(Base + Block), WriteBlockOf(Base + 2 * Block)} && lookup.empty(), "the history did not list both blocks bumped without a read in between");
}

void CheckHistoryOutlivesTheArmedBit() {
    const auto subscription = Subscribe(Base, 3);
    const auto since = SubscriptionGeneration(*subscription);
    NoteTextureReleased(Base);
    Pending(Base + Block);
    std::vector<std::uint64_t> plain;
    std::vector<std::uint64_t> lookup;
    Require(SubscriptionBumpsSince(*subscription, since, plain, lookup), "the history reported an overflow after two bumps");
    Require(lookup == std::vector<std::uint64_t>{WriteBlockOf(Base)}, "the history lost the bump that moved the version");
    Require(plain == std::vector<std::uint64_t>{WriteBlockOf(Base + Block)}, "the history lost a bump made while the queried bit was clear");
}

void CheckTypeMask() {
    const auto subscription = Subscribe(Base, 2, WriteChangeBit(WriteChange::Identity));
    const auto since = SubscriptionGeneration(*subscription);
    const auto seen = ObserveSubscription(*subscription);
    Pending(Base);
    Require(ObserveSubscription(*subscription) == seen, "a bump of a type outside the mask moved the version");
    std::vector<std::uint64_t> plain;
    std::vector<std::uint64_t> lookup;
    Require(SubscriptionBumpsSince(*subscription, since, plain, lookup) && plain == std::vector<std::uint64_t>{WriteBlockOf(Base)}, "a bump of a type outside the mask was left out of the history");
    NoteTextureReleased(Base + Block);
    Require(ObserveSubscription(*subscription) != seen, "a bump of a type inside the mask did not move the version");
}

void CheckReusedSlot() {
    {
        const auto first = Subscribe(Base, 2);
        Pending(Base);
    }
    const auto second = Subscribe(Base + 8 * Block, 2);
    const auto since = SubscriptionGeneration(*second);
    const auto seen = ObserveSubscription(*second);
    Pending(Base);
    NoteTextureReleased(Base + Block);
    std::vector<std::uint64_t> plain;
    std::vector<std::uint64_t> lookup;
    Require(ObserveSubscription(*second) == seen && SubscriptionBumpsSince(*second, since, plain, lookup) && plain.empty() && lookup.empty(), "a bump of a released subscription's range reached the subscription that replaced it");
    Pending(Base + 8 * Block);
    Require(ObserveSubscription(*second) != seen, "a bump over a subscription's own range did not reach it");
}

constexpr std::uint64_t AliasA = std::uint64_t{1} << 36;
constexpr std::uint64_t AliasB = AliasA + 64 * Block;

bool ResolveAlias(std::uint64_t address, std::uint64_t bytes, int* file, std::uint64_t* offset) {
    for (const auto base : {AliasA, AliasB}) {
        if (address >= base && address + bytes <= base + 8 * Block) {
            *file = 5;
            *offset = address - base;
            return true;
        }
    }
    return false;
}

std::atomic<int> backingProbes{0};

bool CountAliasProbe(std::uint64_t address, std::uint64_t bytes, int* file, std::uint64_t* offset) {
    ++backingProbes;
    return ResolveAlias(address, bytes, file, offset);
}

void CheckBackingProbesAreBounded() {
    struct Scope {
        Scope() { SetWriteBackingResolver(&CountAliasProbe); }
        ~Scope() { SetWriteBackingResolver(nullptr); }
    } scope;
    const auto plain = Subscribe(Base + 40 * Block, 2);
    backingProbes = 0;
    NoteGpuPending(Base, 120 * Block);
    Require(backingProbes.load() == 0, "a bump looked for backing keys while no subscription had any");
    {
        const auto backed = Subscribe(AliasA, 2);
        backingProbes = 0;
        NoteGpuPending(Base, 120 * Block);
        Require(backingProbes.load() == 1, "a bump over a large unbacked range probed its backing more than once");
        backingProbes = 0;
        NoteGpuPending(Base, 8 * Block);
        Require(backingProbes.load() <= 9, "a bump over a small unbacked range probed below 64 KiB blocks");
    }
    backingProbes = 0;
    NoteGpuPending(Base, 8 * Block);
    Require(backingProbes.load() == 0, "a released backed subscription left backing discovery on");
}

void CheckBackingKeys() {
    struct Scope {
        Scope() { SetWriteBackingResolver(&ResolveAlias); }
        ~Scope() { SetWriteBackingResolver(nullptr); }
    } scope;
    const auto subscription = Subscribe(AliasA, 4);
    const auto bystander = Subscribe(AliasA + 16 * Block, 2);
    const auto since = SubscriptionGeneration(*subscription);
    const auto seen = ObserveSubscription(*bystander);
    Pending(AliasB + 2 * Block);
    std::vector<std::uint64_t> plain;
    std::vector<std::uint64_t> lookup;
    Require(SubscriptionBumpsSince(*subscription, since, plain, lookup) && plain == std::vector<std::uint64_t>{WriteBlockOf(AliasA + 2 * Block)}, "a bump through an alias did not reach a subscription registered through the other view");
    Require(ObserveSubscription(*bystander) == seen, "a bump through an alias reached a subscription over other memory");
    const auto reverse = SubscriptionGeneration(*subscription);
    const auto other = Subscribe(AliasB + 2 * Block, 2);
    const auto otherSince = SubscriptionGeneration(*other);
    NoteTextureReleased(AliasA + 3 * Block);
    Require(SubscriptionBumpsSince(*other, otherSince, plain, lookup) && lookup == std::vector<std::uint64_t>{WriteBlockOf(AliasB + 3 * Block)} && plain.empty(), "a bump through the first view did not reach a subscription registered through the alias");
    Require(SubscriptionBumpsSince(*subscription, reverse, plain, lookup) && plain.empty() && lookup == std::vector<std::uint64_t>{WriteBlockOf(AliasA + 3 * Block)}, "a bump through the first view did not reach its own subscription");
}

void CheckDomains() {
    const auto subscription = Subscribe(Base, 2);
    constexpr std::uint32_t all = WriteDomainBit(WriteDomain::Proof) | WriteDomainBit(WriteDomain::Unwatch) | WriteDomainBit(WriteDomain::Mapping);
    auto certificate = CertifySubscription(*subscription, all);
    Require(CertificateHolds(*subscription, certificate, all), "a certificate did not hold with nothing changed");
    BreakProofs();
    Require(!CertificateHolds(*subscription, certificate, all), "a certificate held after BreakProofs");
    Require(CertificateHolds(*subscription, certificate, all & ~WriteDomainBit(WriteDomain::Proof)), "a certificate that does not depend on the proof domain failed on it");
    certificate = CertifySubscription(*subscription, all);
    Require(CertificateHolds(*subscription, certificate, all), "a fresh certificate did not hold");
    Unwatch(Base + 64 * Block, Block);
    Require(!CertificateHolds(*subscription, certificate, all), "a certificate held after an Unwatch");
    certificate = CertifySubscription(*subscription, all);
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(static_cast<std::uintptr_t>(Base + 96 * Block), static_cast<std::size_t>(Block));
    Require(!CertificateHolds(*subscription, certificate, all), "a certificate held after an allocation invalidation");
    certificate = CertifySubscription(*subscription, all);
    Require(CertificateHolds(*subscription, certificate, all), "a fresh certificate did not hold after the domains settled");
    Pending(Base);
    Require(!CertificateHolds(*subscription, certificate, all), "a certificate held after a bump of its range");
}

void CheckCrowdedKey() {
    std::vector<std::shared_ptr<WriteSubscription>> crowd;
    std::vector<std::uint64_t> seen;
    for (int i = 0; i < 7; ++i) {
        crowd.push_back(Subscribe(Base + 20 * Block, 1));
        seen.push_back(ObserveSubscription(*crowd.back()));
    }
    Pending(Base + 20 * Block);
    for (std::size_t i = 0; i < crowd.size(); ++i) Require(ObserveSubscription(*crowd[i]) != seen[i], "a subscription sharing its block with six others missed a bump");
    crowd.erase(crowd.begin(), crowd.begin() + 4);
    seen.erase(seen.begin(), seen.begin() + 4);
    for (int i = 0; i < 3; ++i) {
        crowd.push_back(Subscribe(Base + 20 * Block, 1));
        seen.push_back(ObserveSubscription(*crowd.back()));
    }
    for (std::size_t i = 0; i < crowd.size(); ++i) seen[i] = ObserveSubscription(*crowd[i]);
    Pending(Base + 20 * Block);
    for (std::size_t i = 0; i < crowd.size(); ++i) Require(ObserveSubscription(*crowd[i]) != seen[i], "a subscription missed a bump after its neighbours were released and replaced");
}

void CheckWithoutHistory() {
    const auto subscription = Subscribe(Base + 80 * Block, 2, WriteChangeAll, false);
    const auto masked = Subscribe(Base + 80 * Block, 2, WriteChangeBit(WriteChange::Identity), false);
    const auto since = SubscriptionGeneration(*subscription);
    const auto seen = ObserveSubscription(*subscription);
    const auto maskedSeen = ObserveSubscription(*masked);
    Pending(Base + 81 * Block);
    Require(ObserveSubscription(*subscription) != seen, "a bump did not move a subscription without history");
    Require(ObserveSubscription(*masked) == maskedSeen, "a bump outside the mask moved a subscription without history");
    std::vector<std::uint64_t> plain;
    std::vector<std::uint64_t> lookup;
    Require(!SubscriptionBumpsSince(*subscription, since, plain, lookup), "a subscription without history listed its bumps, so a narrowing reader would trust an empty list");
    const auto calls = WriteIndexCounted().bumps;
    {
        const auto released = Subscribe(Base + 84 * Block, 1, WriteChangeAll, false);
    }
    const auto replacement = Subscribe(Base + 86 * Block, 1, WriteChangeAll, false);
    const auto replacementSeen = ObserveSubscription(*replacement);
    Pending(Base + 84 * Block);
    Require(ObserveSubscription(*replacement) == replacementSeen && WriteIndexCounted().bumps == calls + 0, "a bump of a released subscription's range reached the subscription without history that replaced its slot");
}

void CheckProducerBumps() {
    const auto mirrors = Subscribe(Base + 40 * Block, 2, WriteChangeBit(WriteChange::Mirror));
    const auto guest = Subscribe(Base + 40 * Block, 2);
    const auto bytes = Subscribe(Base + 40 * Block, 2, WriteChangeBit(WriteChange::Bytes));
    const auto mirrorSeen = ObserveSubscription(*mirrors);
    const auto bytesSeen = ObserveSubscription(*bytes);
    const auto guestSeen = ObserveSubscription(*guest);
    BumpWrites(Base + 50 * Block, Block, WriteBumpSource::Mirror);
    BumpWrites(Base + 50 * Block, Block, WriteBumpSource::Depth);
    Require(ObserveSubscription(*mirrors) == mirrorSeen && ObserveSubscription(*bytes) == bytesSeen, "a producer bump of a disjoint range moved a subscription");
    BumpWrites(Base + 41 * Block, 4, WriteBumpSource::Depth);
    Require(ObserveSubscription(*mirrors) == mirrorSeen, "a depth bump moved a subscription to mirror changes only");
    Require(ObserveSubscription(*bytes) != bytesSeen, "a depth bump of the range did not move a subscription to byte changes");
    const auto guestAfterDepth = ObserveSubscription(*guest);
    Require(guestAfterDepth != guestSeen, "a depth bump of the range did not move a subscription to every guest change");
    BumpWrites(Base + 40 * Block, 4, WriteBumpSource::Mirror);
    Require(ObserveSubscription(*mirrors) != mirrorSeen, "a mirror bump of the range did not move a subscription to mirror changes");
    Require(ObserveSubscription(*guest) == guestAfterDepth, "a mirror bump moved a subscription to guest memory changes");
    bool refused = false;
    try {
        BumpWrites(Base + 40 * Block, 4, WriteBumpSource::Driver);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    Require(refused, "BumpWrites accepted a source the index owns");
}

void CheckProducerDomains() {
    const auto subscription = Subscribe(Base + 60 * Block, 1);
    constexpr std::uint32_t mask = WriteDomainBit(WriteDomain::Depth) | WriteDomainBit(WriteDomain::Mirror);
    auto certificate = CertifySubscription(*subscription, mask);
    BumpDomainVersion(WriteDomain::Depth);
    Require(!CertificateHolds(*subscription, certificate, mask), "a certificate held after its depth domain moved");
    Require(CertificateHolds(*subscription, certificate, WriteDomainBit(WriteDomain::Mirror)), "a depth domain bump broke a certificate on the mirror domain only");
    certificate = CertifySubscription(*subscription, mask);
    BumpDomainVersion(WriteDomain::Mirror);
    Require(!CertificateHolds(*subscription, certificate, mask), "a certificate held after its mirror domain moved");
    for (const auto domain : {WriteDomain::Proof, WriteDomain::Unwatch, WriteDomain::Mapping, WriteDomain::CertifiedProof}) {
        bool refused = false;
        try {
            BumpDomainVersion(domain);
        } catch (const std::runtime_error&) {
            refused = true;
        }
        Require(refused, "BumpDomainVersion accepted a domain the index owns");
    }
}

std::vector<WriteDomain> domainReads;

void RecordDomainRead(WriteDomain domain) {
    domainReads.push_back(domain);
}

bool MirrorReadAfterMappingMoved() {
    const auto subscription = Subscribe(Base + 70 * Block, 1);
    constexpr std::uint32_t mask = WriteDomainBit(WriteDomain::Mapping) | WriteDomainBit(WriteDomain::Mirror);
    const auto certificate = CertifySubscription(*subscription, mask);
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(static_cast<std::uintptr_t>(Base + 98 * Block), static_cast<std::size_t>(Block));
    domainReads.clear();
    SetDomainReadObserverForTest(RecordDomainRead);
    const bool held = CertificateHolds(*subscription, certificate, mask);
    SetDomainReadObserverForTest(nullptr);
    Require(!held, "a certificate held after a mapping change");
    return std::find(domainReads.begin(), domainReads.end(), WriteDomain::Mirror) != domainReads.end();
}

void CheckDomainOrder() {
    Require(!MirrorReadAfterMappingMoved(), "CertificateHolds read the mirror domain after the mapping domain had failed");
    SetCertificateMutantForTest(CertificateMutant::ReverseDomainOrder);
    const bool mutantRead = MirrorReadAfterMappingMoved();
    SetCertificateMutantForTest(CertificateMutant::None);
    Require(mutantRead, "the reversed domain order mutant survived");
}

bool OwnerBumpVoids() {
    const auto owner = RegisterWriteOwner(Base + 100 * Block, 4 * Block);
    const auto subscription = Subscribe(Base + 101 * Block, 1, WriteChangeAll, false);
    const auto certificate = CertifySubscription(*subscription, 0);
    Require(CertificateHolds(*subscription, certificate, 0), "a certificate on an owner did not hold with nothing changed");
    Pending(Base + 103 * Block);
    const bool voided = !CertificateHolds(*subscription, certificate, 0);
    UnregisterWriteOwner(owner);
    return voided;
}

bool ManyOwnersVoid() {
    std::vector<WriteOwner> owners;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (std::uint64_t i = 0; i < CertificateInlineOwners + 1; ++i) {
        const auto base = Base + (160 + 2 * i) * Block;
        owners.emplace_back(base, Block);
        ranges.emplace_back(base, base + 64);
    }
    const auto subscription = SubscribeWrites(ranges, WriteChangeAll, false);
    const auto certificate = CertifySubscription(*subscription, 0);
    Require(certificate.ownerCount == CertificateInlineOwners + 1 && CertificateHolds(*subscription, certificate, 0), "a certificate over more owners than it holds inline did not hold with nothing changed");
    Pending(ranges.back().first);
    return !CertificateHolds(*subscription, certificate, 0);
}

void CheckOwners() {
    const auto dependenciesBefore = WriteIndexCounted().ownerDependencies;
    const auto first = RegisterWriteOwner(Base + 100 * Block, 4 * Block);
    const auto second = RegisterWriteOwner(Base + 110 * Block, 2 * Block);
    const auto inside = Subscribe(Base + 101 * Block, 1, WriteChangeAll, false);
    const auto other = Subscribe(Base + 110 * Block, 1, WriteChangeAll, false);
    const auto masked = Subscribe(Base + 101 * Block, 1, WriteChangeBit(WriteChange::Identity), false);
    Require(WriteIndexCounted().ownerDependencies == dependenciesBefore + 3, "subscriptions inside owners did not take owner dependencies");
    auto certificate = CertifySubscription(*inside, 0);
    const auto otherCertificate = CertifySubscription(*other, 0);
    const auto maskedCertificate = CertifySubscription(*masked, 0);
    Pending(Base + 104 * Block + 16);
    Require(CertificateHolds(*inside, certificate, 0), "a bump beside an owner voided a certificate inside it");
    const auto fastBefore = WriteIndexCounted().ownerFastBumps;
    Pending(Base + 103 * Block);
    Require(WriteIndexCounted().ownerFastBumps == fastBefore + 1, "a bump inside an owner with no handle subscribers took the block walk");
    Require(!CertificateHolds(*inside, certificate, 0), "a bump inside an owner left a certificate on that owner standing");
    Require(CertificateHolds(*other, otherCertificate, 0), "a bump inside one owner voided a certificate on another");
    Require(CertificateHolds(*masked, maskedCertificate, 0), "a bump outside the type mask voided a certificate on its owner");
    {
        const auto straddling = Subscribe(Base + 99 * Block, 2, WriteChangeAll, false);
        const auto seen = ObserveSubscription(*straddling);
        const auto fast = WriteIndexCounted().ownerFastBumps;
        Pending(Base + 100 * Block + 8);
        Require(ObserveSubscription(*straddling) != seen, "a bump inside an owner missed a subscription that straddles the owner's start");
        Require(WriteIndexCounted().ownerFastBumps == fast, "a bump inside an owner with a straddling subscriber took the fast path");
    }
    const auto fast = WriteIndexCounted().ownerFastBumps;
    Pending(Base + 102 * Block);
    Require(WriteIndexCounted().ownerFastBumps == fast + 1, "the owner kept counting a straddling subscriber after it was released");
    const auto early = Subscribe(Base + 120 * Block, 1, WriteChangeAll, false);
    const auto third = RegisterWriteOwner(Base + 120 * Block, 2 * Block);
    const auto earlySeen = ObserveSubscription(*early);
    Pending(Base + 120 * Block + 16);
    Require(ObserveSubscription(*early) != earlySeen, "a bump inside an owner registered after a subscription missed that subscription");
    certificate = CertifySubscription(*inside, 0);
    NoteGpuPending(Base + 104 * Block - 8, 16);
    Require(!CertificateHolds(*inside, certificate, 0), "a bump across an owner's end left a certificate on that owner standing");
    certificate = CertifySubscription(*inside, 0);
    UnregisterWriteOwner(first);
    Require(!CertificateHolds(*inside, certificate, 0), "a certificate on an owner held after the owner was unregistered");
    UnregisterWriteOwner(second);
    UnregisterWriteOwner(third);
    Require(OwnerBumpVoids(), "a bump inside an owner left its certificate standing");
    Require(ManyOwnersVoid(), "a bump inside the fifth owner of a subscription left its certificate standing");
    for (const auto mutant : {CertificateMutant::IgnoreOwners, CertificateMutant::HideOwnerBumps, CertificateMutant::CertificateDropsOwners}) {
        SetCertificateMutantForTest(mutant);
        const bool caught = OwnerBumpVoids();
        SetCertificateMutantForTest(CertificateMutant::None);
        Require(!caught, "the owner test does not catch an owner mutant");
    }
}

bool ThrowsOnRegister(std::uint64_t base, std::uint64_t bytes) {
    try {
        UnregisterWriteOwner(RegisterWriteOwner(base, bytes));
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

bool ObservationWithOwnerHolds() {
    const WriteOwner owner(Base + 80 * Block, 2 * Block);
    try {
        CheckWithoutHistory();
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

void CheckOwnerObservation() {
    Require(ObservationWithOwnerHolds(), "the observation contract failed for subscriptions inside an owner");
    SetCertificateMutantForTest(CertificateMutant::ObserveIgnoresOwners);
    const bool mutantHolds = ObservationWithOwnerHolds();
    SetCertificateMutantForTest(CertificateMutant::None);
    Require(!mutantHolds, "the observation contract test does not catch an observation that ignores owners");
    auto owner = std::make_unique<WriteOwner>(Base + 130 * Block, 2 * Block);
    const auto subscription = Subscribe(Base + 130 * Block, 1, WriteChangeAll, false);
    const auto seen = ObserveSubscription(*subscription);
    Require(ObserveSubscription(*subscription) == seen, "observing an owner-backed subscription twice with nothing written moved it");
    Pending(Base + 131 * Block);
    const auto bumped = ObserveSubscription(*subscription);
    Require(bumped != seen && ObserveSubscription(*subscription) == bumped, "an observe, bump, observe sequence on an owner-backed subscription did not move exactly once");
    owner.reset();
    const auto dead = ObserveSubscription(*subscription);
    Require(dead != bumped && ObserveSubscription(*subscription) != dead, "an owner-backed subscription read as unchanged after its owner was unregistered");
    Require(ThrowsOnRegister(~std::uint64_t{0} - Block, 2 * Block), "a write owner range that wraps around the address space was registered");
    Require(!ThrowsOnRegister(Base + 140 * Block, Block), "a plain write owner range was refused");
}

bool BlockBumpVoids() {
    const auto subscription = Subscribe(Base + 200 * Block, 1, WriteChangeAll, false);
    const auto certificate = CertifySubscription(*subscription, 0);
    Require(CertificateHolds(*subscription, certificate, 0), "a certificate on block counters did not hold with nothing changed");
    Pending(Base + 200 * Block + 128);
    return !CertificateHolds(*subscription, certificate, 0);
}

bool LateOwnerSeesCountedSubscription() {
    const auto subscription = Subscribe(Base + 210 * Block, 1, WriteChangeAll, false);
    const WriteOwner owner(Base + 210 * Block, 2 * Block);
    const auto certificate = CertifySubscription(*subscription, 0);
    NoteGpuPending(Base + 210 * Block, 2 * Block);
    return !CertificateHolds(*subscription, certificate, 0);
}

bool HandleBumpVoids() {
    const auto subscription = Subscribe(Base + 230 * Block, 1);
    const auto certificate = CertifySubscription(*subscription, 0);
    Require(!certificate.handles || CertificateHolds(*subscription, certificate, 0), "a certificate on handles did not hold with nothing changed");
    Pending(Base + 230 * Block + 64);
    return !CertificateHolds(*subscription, certificate, 0);
}

void CheckBlockCounters() {
    const auto subscription = Subscribe(Base + 202 * Block, 1, WriteChangeAll, false);
    const auto masked = Subscribe(Base + 202 * Block, 1, WriteChangeBit(WriteChange::Identity), false);
    const auto certificate = CertifySubscription(*subscription, 0);
    const auto maskedCertificate = CertifySubscription(*masked, 0);
    const auto seen = ObserveSubscription(*subscription);
    Pending(Base + 203 * Block);
    Require(CertificateHolds(*subscription, certificate, 0) && ObserveSubscription(*subscription) == seen, "a bump of the next block moved a subscription on block counters");
    const auto notified = WriteIndexCounted().bumps;
    Pending(Base + 202 * Block + 4096);
    Require(WriteIndexCounted().bumps == notified, "a bump notified a handle of a subscription on block counters");
    Require(!CertificateHolds(*subscription, certificate, 0) && ObserveSubscription(*subscription) != seen, "a bump inside a subscription's block left it standing on block counters");
    Require(CertificateHolds(*masked, maskedCertificate, 0), "a bump outside the type mask voided a certificate on block counters");
    const auto wide = Subscribe(Base + 220 * Block, CertificateInlineOwners + 1, WriteChangeAll, false);
    const auto wideCertificate = CertifySubscription(*wide, 0);
    Require(wideCertificate.blockCount == CertificateInlineOwners + 1 && CertificateHolds(*wide, wideCertificate, 0), "a certificate over more blocks than it holds inline did not hold with nothing changed");
    Pending(Base + (220 + CertificateInlineOwners) * Block);
    Require(!CertificateHolds(*wide, wideCertificate, 0), "a bump in the last block of a wide subscription left its certificate standing");
    Require(BlockBumpVoids(), "a bump inside a block left its certificate standing");
    Require(!certificate.handles && HandleBumpVoids(), "a certificate on block counters read its version, or one on handles missed a bump");
    SetCertificateMutantForTest(CertificateMutant::CertificateSkipsVersion);
    const bool skipped = HandleBumpVoids();
    SetCertificateMutantForTest(CertificateMutant::None);
    Require(!skipped, "the handle test does not catch a certificate that skips its version");
    Require(LateOwnerSeesCountedSubscription(), "an owner registered after a subscription on block counters took the fast path past it");
    for (const auto mutant : {CertificateMutant::HideBlockBumps, CertificateMutant::CertificateDropsBlocks}) {
        SetCertificateMutantForTest(mutant);
        const bool caught = BlockBumpVoids();
        SetCertificateMutantForTest(CertificateMutant::None);
        Require(!caught, "the block counter test does not catch a block counter mutant");
    }
}

void CheckCountingOff() {
    const auto subscription = Subscribe(Base + 150 * Block, 1, WriteChangeAll, false);
    SetWriteIndexCountingForTest(false);
    const auto before = WriteIndexCounted();
    Pending(Base + 150 * Block);
    const auto after = WriteIndexCounted();
    SetWriteIndexCountingForTest(true);
    Require(after.calls == before.calls && after.bumps == before.bumps && after.lookups == before.lookups, "a bump updated the shared write index counters with counting off");
}

void CheckWriteIndex() {
    SetWriteIndexCountingForTest(true);
    CheckObserveAndArm();
    CheckHistoryOutlivesTheArmedBit();
    CheckTypeMask();
    CheckReusedSlot();
    CheckCrowdedKey();
    CheckBackingKeys();
    CheckBackingProbesAreBounded();
    CheckDomains();
    CheckWithoutHistory();
    CheckProducerBumps();
    CheckProducerDomains();
    CheckDomainOrder();
    CheckOwners();
    CheckOwnerObservation();
    CheckBlockCounters();
    CheckCountingOff();
}
}

bool HasSource(std::uint64_t address, std::size_t bytes, ExclusionSource source) {
    return (UnwatchedSources(address, bytes) & (std::uint32_t{1} << static_cast<unsigned>(source))) != 0;
}

bool ExplicitUnwatchNamed(std::uint64_t address) {
    Unwatch(address, Page);
    return HasSource(address, Page, ExclusionSource::Explicit);
}

void CheckExclusionSources() {
    const auto explicitAt = Base + 40 * Page;
    Require(ExplicitUnwatchNamed(explicitAt), "an explicit unwatch was not attributed");
    Require(!HasSource(explicitAt + Page, Page, ExclusionSource::Explicit), "a page next to an explicit unwatch was attributed");
    const auto importAt = Base + 48 * Page;
    Require(ImportWatched(importAt, 2 * Page, [] { return true; }, false), "the compared import failed");
    Require(HasSource(importAt, 2 * Page, ExclusionSource::ComparedImport) && !HasSource(importAt, 2 * Page, ExclusionSource::Explicit), "a compared import was not attributed as one");
    Require(ExplicitUnwatchNamed(importAt) && !HasSource(importAt, Page, ExclusionSource::ComparedImport) && HasSource(importAt + Page, Page, ExclusionSource::ComparedImport), "a later exclusion did not replace the source of its own pages only");
#ifndef _WIN32
    const auto walkAt = Base + 56 * Page;
    {
        const std::lock_guard lock(Fake().mutex);
        Fake().failWalks = true;
    }
    const bool imported = ImportWatched(walkAt, Page, [] { return true; }, true);
    {
        const std::lock_guard lock(Fake().mutex);
        Fake().failWalks = false;
    }
    Require(imported && HasSource(walkAt, Page, ExclusionSource::ImportWalk), "an import whose walk failed was not attributed to the walk");
#endif
    SetCertificateMutantForTest(CertificateMutant::DropExclusionSources);
    const bool caught = !ExplicitUnwatchNamed(Base + 60 * Page);
    SetCertificateMutantForTest(CertificateMutant::None);
    Require(caught, "dropping exclusion sources still attributed the unwatch, so the case tests nothing");
    std::vector<std::byte> expected(Page);
    std::memcpy(expected.data(), reinterpret_cast<const void*>(Base), Page);
    const auto firstLine = static_cast<std::uint32_t>(__LINE__ + 1);
    Require(EqualsCommitted(Base, expected), "the first compared site differs");
    const auto secondLine = static_cast<std::uint32_t>(__LINE__ + 1);
    Require(EqualsCommittedUnsynced(Base, std::span<const std::byte>(expected).first(Page / 2)) && EqualsCommittedUnsynced(Base, std::span<const std::byte>(expected).first(Page / 2)), "the second compared site differs");
    ReportMemoryProfileNextForTest();
    Require(EqualsCommitted(Base, std::span<const std::byte>(expected).first(16)), "the reporting compare differs");
    Require(CompareSiteBytesForTest(firstLine) == Page && CompareSiteBytesForTest(secondLine) == Page, "compared bytes were not attributed to their call sites");
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--sources") {
#ifdef _WIN32
            _putenv_s("APS5_PROFILE_DRAW", "1");
            if (!WriteWatched()) return 77;
            const Arena arena;
#else
            setenv("APS5_PROFILE_DRAW", "1", 1);
            std::vector<std::byte> backing(static_cast<std::size_t>(Span + Block));
            Base = (reinterpret_cast<std::uint64_t>(backing.data()) + Block - 1) & ~(std::uint64_t{Block} - 1);
            std::memset(reinterpret_cast<void*>(Base), 0x11, static_cast<std::size_t>(Span));
#endif
            CheckExclusionSources();
            std::cout << "exclusion source test passed\n";
            return 0;
        }
        const bool writeIndexOnly = argc == 2 && std::string(argv[1]) == "--write-index";
#ifdef _WIN32
        if (!WriteWatched()) {
            std::cout << "collect memo generation test skipped: the guest arena is not write watched\n";
            return 77;
        }
        const Arena arena;
#else
        if (!writeIndexOnly) {
            CheckMemoHitDuringWalk();
            CheckFailedWalkIsRetried();
        }
#endif
        if (argc == 2) {
            const std::string selected = argv[1];
            if (selected == "--write-index") CheckWriteIndex();
            else if (selected == "--interior") CheckInteriorCoverageAfterRingEviction();
            else if (selected == "--unions") {
                CheckAdjacentCoverage();
                CheckOverlappingCoverage();
            } else if (selected == "--invalidation") {
                CheckCoverageGapsAndInvalidations();
                CheckCoverageThreadAndDriverWrites();
            } else throw std::runtime_error("unknown collect coverage test");
            std::cout << "collect coverage test passed\n";
            return 0;
        }
        CheckUnalignedRepeatReachesKernelOnce();
        CheckUncachedWalksKeepCachedEntries();
        CheckNeighbouringBlockStoreIsFound();
        CheckInvalidationReachesBlockKeyedEntry();
        CheckThreadWithoutEpochWalksEveryTime();
        CheckStoreInNewEpochIsFound();
    } catch (const std::exception& error) {
        std::cerr << "collect memo generation test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "collect memo generation test passed\n";
    return 0;
}
