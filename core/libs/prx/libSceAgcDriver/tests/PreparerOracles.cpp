#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <span>
#include <thread>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DrawFinisher;
using AgcDriver::DriverDetail::DrawPipeline;
using Kind = DrawPipeline::CommitKind;

constexpr std::uint64_t Watched = 0x48000;
constexpr std::uint32_t Before = 0x11111111u;
constexpr std::uint32_t After = 0x22222222u;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void WaitUntil(const std::atomic<bool>& flag, const char* what) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > until) throw std::runtime_error(std::string("timed out waiting for ") + what);
        std::this_thread::yield();
    }
}

std::vector<std::byte> Bytes(std::uint32_t value) {
    std::vector<std::byte> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

enum class Producer {
    TargetWrite,
    UnknownWrites,
    LabelStore,
};

const char* Name(Producer producer) {
    switch (producer) {
    case Producer::TargetWrite: return "a draw's target write";
    case Producer::UnknownWrites: return "a draw with unknown writes";
    case Producer::LabelStore: return "a queued label store";
    }
    return "?";
}

struct Older {
    std::atomic<bool> started{false};
    std::atomic<bool> open{false};
    std::atomic<std::uint32_t> memory{Before};
    std::atomic<std::uint32_t> readAtCommit{0};

    ~Older() {
        open.store(true, std::memory_order_release);
        try {
            DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
        } catch (...) {
        }
    }
};

struct Opener {
    std::thread thread;
    explicit Opener(Older& older) : thread([&older] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        older.open.store(true, std::memory_order_release);
    }) {}
    ~Opener() {
        if (thread.joinable()) thread.join();
    }
};

void EnqueueWriter(Older& older, Producer producer) {
    DrawFinisher::Pending held;
    held.key = 0x7100u + static_cast<std::uint64_t>(producer);
    held.unknown = producer == Producer::UnknownWrites;
    if (!held.unknown) held.writes = {{Watched, Watched + sizeof(std::uint32_t)}};
    if (producer == Producer::LabelStore) {
        held.store = true;
        held.labelAddress = Watched;
        held.labelBytes = Bytes(After);
    }
    DrawFinisher::Get().Enqueue([&older, producer] {
        older.started.store(true, std::memory_order_release);
        while (!older.open.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto store = [&older] { older.memory.store(After, std::memory_order_release); };
        if (producer == Producer::LabelStore) DrawPipeline::Queue0().EnqueueStore(store, Watched, Bytes(After));
        else DrawPipeline::Queue0().Enqueue(Kind::Draw, store, {{Watched, Watched + sizeof(std::uint32_t)}}, 0, {}, {}, true);
    }, std::move(held));
}

std::uint32_t CaptureRead(const Older& older) {
    auto& pipeline = DrawPipeline::Queue0();
    std::array<std::byte, sizeof(std::uint32_t)> known{};
    auto queued = DrawPipeline::QueuedWrite::None;
    if (pipeline.Busy()) queued = pipeline.StoredBytes(Watched, known.size(), known);
    if (queued == DrawPipeline::QueuedWrite::Stored) {
        std::uint32_t value = 0;
        std::memcpy(&value, known.data(), sizeof(value));
        return value;
    }
    if (queued == DrawPipeline::QueuedWrite::Unknown) pipeline.Drain(DrawPipeline::DrainReason::Capture);
    return older.memory.load(std::memory_order_acquire);
}

void YoungerCaptureSeesOlderWrite(Producer producer) {
    Older older;
    Opener opener(older);
    EnqueueWriter(older, producer);
    std::atomic<bool> done{false};
    std::atomic<bool> overlapped{false};
    std::atomic<std::uint32_t> seen{0};
    DrawFinisher::Get().Enqueue([&] {
        WaitUntil(older.started, "the older item to start");
        overlapped.store(!older.open.load(std::memory_order_acquire), std::memory_order_release);
        seen.store(CaptureRead(older), std::memory_order_release);
        done.store(true, std::memory_order_release);
    }, DrawFinisher::Pending{});
    DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
    Require(done.load(), std::string("the younger capture after ") + Name(producer) + " did not run");
    if (DrawFinisher::Preparers() > 1) Require(overlapped.load(), std::string("the younger capture after ") + Name(producer) + " did not overlap the older item, so the case proves nothing");
    Require(seen.load() == After, std::string("a younger capture read ") + std::to_string(seen.load()) + " where " + Name(producer) + " before it wrote " + std::to_string(After));
}

void OlderDrawReadsBeforeYoungerPublication() {
    Older older;
    Opener opener(older);
    DrawFinisher::Get().Enqueue([&older] {
        older.started.store(true, std::memory_order_release);
        while (!older.open.load(std::memory_order_acquire)) std::this_thread::yield();
        DrawPipeline::Queue0().Enqueue(Kind::Draw, [&older] { older.readAtCommit.store(older.memory.load(std::memory_order_acquire), std::memory_order_release); }, {}, 0, {}, {{Watched, Watched + sizeof(std::uint32_t)}}, true);
    }, [] {
        DrawFinisher::Pending held;
        held.unknown = false;
        return held;
    }());
    std::atomic<bool> done{false};
    std::atomic<bool> overlapped{false};
    DrawFinisher::Get().Enqueue([&] {
        WaitUntil(older.started, "the older draw to start");
        overlapped.store(!older.open.load(std::memory_order_acquire), std::memory_order_release);
        while (DrawPipeline::HeldReads(Watched, sizeof(std::uint32_t))) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Capture);
        older.memory.store(After, std::memory_order_release);
        done.store(true, std::memory_order_release);
    }, [] {
        DrawFinisher::Pending held;
        held.unknown = false;
        return held;
    }());
    DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
    Require(done.load(), "the younger publication did not run");
    if (DrawFinisher::Preparers() > 1) Require(overlapped.load(), "the younger publication did not overlap the older draw, so the case proves nothing");
    Require(older.readAtCommit.load() == Before, "an older draw read " + std::to_string(older.readAtCommit.load()) + " at its commit: a younger item published into memory the draw reads before the draw committed");
}


constexpr std::size_t GateBytes = 1u << 20u;
constexpr std::size_t GateRead = 4096u;

class WatchedBlock {
public:
    WatchedBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(GateBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, GateBytes, PAGE_READWRITE, GateBytes);
#else
        void* mapped = mmap(nullptr, GateBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            block = static_cast<std::uint8_t*>(mapped);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, GateBytes);
        }
#endif
        Require(block != nullptr, "the preparer gate test cannot allocate its guest block");
        std::fill(block, block + GateBytes, std::uint8_t{0x11});
        AgcDriver::GuestMemory::CollectWritesUncached(Address(), GateBytes);
    }
    ~WatchedBlock() {
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, GateBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, GateBytes);
#else
        munmap(block, GateBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, GateBytes);
#endif
    }
    WatchedBlock(const WatchedBlock&) = delete;
    WatchedBlock& operator=(const WatchedBlock&) = delete;
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }
    std::uint8_t* Bytes() const { return block; }

private:
    std::uint8_t* block = nullptr;
};

struct GateCase {
    bool validated = false;
    bool threw = false;
};

GateCase CheckRead(const WatchedBlock& guest, std::uint64_t offset, const std::vector<std::byte>& captured, bool turnWaited, const std::function<void()>& between) {
    const auto ticket = AgcDriver::GuestMemory::ObserveTicket();
    between();
    const std::uint64_t untouched = GateBytes / 4u * 3u;
    const std::array<DrawFinisher::Range, 2> ranges{{{guest.Address() + untouched, guest.Address() + untouched + captured.size()}, {guest.Address() + offset, guest.Address() + offset + captured.size()}}};
    const std::array<std::span<const std::byte>, 2> bytes{std::span<const std::byte>(captured), std::span<const std::byte>(captured)};
    GateCase result;
    try {
        result.validated = DrawFinisher::CheckPreparerReads(ticket, ranges, bytes, turnWaited);
    } catch (const std::logic_error&) {
        result.threw = true;
    }
    return result;
}

void PreparerGateCounts() {
    using AgcDriver::GuestMemory::TicketRefusal;
    if (!AgcDriver::GuestMemory::WriteWatched()) {
        std::cout << "guest memory has no write watch: the preparer read gate is not tested\n";
        return;
    }
    const bool verify = DrawFinisher::VerifyPreparerReads();
    const bool tickets = AgcDriver::Graphics::RuntimeSwitches::Set(AgcDriver::Graphics::RuntimeSwitch::WriteTickets);
    WatchedBlock guest;
    std::vector<std::byte> captured(GateRead, std::byte{0x11});
    Require(CheckRead(guest, 0, captured, false, [] {}).validated, "the preparer gate refused a capture whose range nobody wrote after the ticket");
    DrawFinisher::AbWindow(false, 'A', 1);
    const auto before = DrawFinisher::PreparerReadsCounted();
    const auto stampsBefore = AgcDriver::GuestMemory::WriteTicketsCounted();
    const auto clean = CheckRead(guest, 0, captured, false, [] {});
    Require(clean.validated && !clean.threw, "the preparer gate refused a capture whose range nobody wrote after the ticket");
    const auto disjoint = CheckRead(guest, 0, captured, false, [&] { AgcDriver::GuestMemory::ForceWriteBumpForTest(guest.Address() + 8 * 65536, 4, AgcDriver::GuestMemory::WriteBumpSource::Driver); });
    Require(disjoint.validated && !disjoint.threw, "the preparer gate refused a capture after a write to another 64 KiB block");
    const auto sameBytes = CheckRead(guest, 0, captured, true, [&] { AgcDriver::GuestMemory::ForceWriteBumpForTest(guest.Address(), 4, AgcDriver::GuestMemory::WriteBumpSource::Driver); });
    Require(!sameBytes.validated && !sameBytes.threw, "the preparer gate validated a capture whose range was stamped after the ticket, or failed a verified capture whose bytes are unchanged");
    const auto changed = CheckRead(guest, 64, captured, false, [&] {
        guest.Bytes()[64] = 0x22;
        AgcDriver::GuestMemory::ForceWriteBumpForTest(guest.Address() + 64, 1, AgcDriver::GuestMemory::WriteBumpSource::Driver);
    });
    Require(!changed.validated && changed.threw == verify, verify ? "APS5_VERIFY_PREPARER_READS let a capture whose guest bytes changed before the commit through" : "the preparer gate failed a capture outside APS5_VERIFY_PREPARER_READS");
    const auto after = DrawFinisher::PreparerReadsCounted();
    const auto stampsAfter = AgcDriver::GuestMemory::WriteTicketsCounted();
    const auto stamped = static_cast<std::size_t>(TicketRefusal::Stamped);
    Require(after.validated - before.validated == 2 && after.missed - before.missed == 2 && after.refused - before.refused == 1 && after.changed - before.changed == (verify ? 1u : 0u), "the preparer gate counted " + std::to_string(after.validated - before.validated) + " validated, " + std::to_string(after.missed - before.missed) + " missed, " + std::to_string(after.refused - before.refused) + " refused and " + std::to_string(after.changed - before.changed) + " changed, not 2, 2, 1 and " + (verify ? "1" : "0"));
    Require(stampsAfter.refused[stamped] - stampsBefore.refused[stamped] == 2, "the preparer gate's two misses were not both stamped refusals");
    const auto line = DrawFinisher::AbWindow(true, 'A', 1);
    const std::string expected = tickets ? "write tickets: 2 validated, 2 missed (no ticket 0, unwatched 0, stamped 2, in flight 0), 1 refused (turn waits)" + std::string(verify ? ", 1 changed (verified)" : "") : "write tickets off";
    Require(line.find(expected) != std::string::npos, "the [ring-preparers] phase line does not carry the gate counts '" + expected + "': " + line);
    std::cout << "preparer read gate counted " << (tickets ? "with" : "without") << " write tickets" << (verify ? ", verified" : "") << '\n';
}

}

int main() {
    try {
        if (DrawPipeline::Depth() == 0 || !DrawFinisher::Sequenced()) return 77;
        if (!DrawFinisher::Overlap()) throw std::runtime_error("the draw finisher is disabled; set APS5_DRAW_PIPELINE");
        DrawPipeline::Active() = true;
        for (const auto producer : {Producer::TargetWrite, Producer::UnknownWrites, Producer::LabelStore}) {
            for (int round = 0; round < 4; ++round) YoungerCaptureSeesOlderWrite(producer);
        }
        for (int round = 0; round < 4; ++round) OlderDrawReadsBeforeYoungerPublication();
        PreparerGateCounts();
        std::cout << "preparer oracles passed with " << DrawFinisher::Preparers() << " preparers\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
