#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
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
        std::cout << "preparer oracles passed with " << DrawFinisher::Preparers() << " preparers\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
