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


std::shared_ptr<WriteSubscription> Subscribe(std::uint64_t address, std::uint64_t blocks, std::uint32_t mask = WriteChangeAll) {
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> ranges{{{address, address + blocks * Block}}};
    return SubscribeWrites(ranges, mask);
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

void CheckWriteIndex() {
    CheckObserveAndArm();
    CheckHistoryOutlivesTheArmedBit();
    CheckTypeMask();
    CheckReusedSlot();
    CheckBackingKeys();
    CheckDomains();
}
}

int main(int argc, char** argv) {
    try {
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
