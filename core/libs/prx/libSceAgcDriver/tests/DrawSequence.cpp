#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DrawFinisher;
using AgcDriver::DriverDetail::DrawPipeline;
using AgcDriver::DriverDetail::SequenceRing;
using Kind = DrawPipeline::CommitKind;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void WaitUntil(const auto& ready, const char* what) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready()) {
        if (std::chrono::steady_clock::now() > until) throw std::runtime_error(std::string("timed out waiting for ") + what);
        std::this_thread::yield();
    }
}

struct Log {
    std::mutex mutex;
    std::vector<std::pair<Kind, int>> committed;
    std::vector<std::pair<Kind, int>> expected;
    std::vector<std::uint64_t> sequences;

    void Commit(Kind kind, int index) {
        std::lock_guard lock(mutex);
        committed.emplace_back(kind, index);
    }
};

struct Gate {
    std::atomic<bool> open{true};
    void Close() { open.store(false, std::memory_order_release); }
    void Open() { open.store(true, std::memory_order_release); }
    void Pass() const {
        while (!open.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    ~Gate() {
        Open();
        try {
            DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
        } catch (...) {
        }
    }
};

constexpr std::uint64_t Base = 0x10000;

void Draw(Log& log, int index, const Gate* gate = nullptr, bool knownWrites = true) {
    DrawFinisher::Pending held;
    held.key = 0x1000u + static_cast<std::uint64_t>(index);
    held.unknown = !knownWrites;
    if (knownWrites) held.writes = {{Base + 0x1000, Base + 0x2000}};
    log.expected.emplace_back(Kind::Draw, index);
    DrawFinisher::Get().Enqueue([&log, index, gate] {
        if (gate != nullptr) gate->Pass();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        DrawPipeline::Queue0().Enqueue(Kind::Draw, [&log, index] { log.Commit(Kind::Draw, index); }, {{Base + 0x1000, Base + 0x2000}}, 0, {}, {{Base + 0x3000, Base + 0x3100}}, true);
    }, std::move(held));
}

void Forward(Log& log, Kind kind, int index, std::uint64_t address = 0, std::vector<std::byte> label = {}) {
    log.expected.emplace_back(kind, index);
    std::vector<DrawPipeline::Range> writes;
    if (address != 0) writes.emplace_back(address, address + (label.empty() ? 16 : label.size()));
    if (kind == Kind::Store) {
        DrawPipeline::Queue0().EnqueueStore([&log, kind, index] { log.Commit(kind, index); }, address, std::move(label));
        return;
    }
    DrawPipeline::Queue0().Enqueue(kind, [&log, kind, index] { log.Commit(kind, index); }, std::move(writes), address, std::move(label));
}

std::vector<std::byte> Bytes(std::uint32_t value) {
    std::vector<std::byte> bytes(4);
    std::memcpy(bytes.data(), &value, 4);
    return bytes;
}

void Flip(Log& log, int index) {
    log.expected.emplace_back(Kind::Other, index);
    DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Packet, 0xff);
    log.Commit(Kind::Other, index);
}

void MixedItemsCommitInIssueOrder() {
    Log log;
    int index = 0;
    for (int frame = 0; frame < 8; ++frame) {
        Draw(log, index++);
        Forward(log, Kind::EndOfPipe, index++, Base + 0x40, Bytes(0x100u + frame));
        Draw(log, index++);
        Forward(log, Kind::DmaWrite, index++, Base + 0x5000);
        Forward(log, Kind::Store, index++, Base + 0x80, Bytes(7));
        Draw(log, index++, nullptr, frame % 2 == 0);
        Forward(log, Kind::EndOfPipe, index++, Base + 0x44, Bytes(0x200u + frame));
        Forward(log, Kind::SubmitEnd, index++);
        Flip(log, index++);
    }
    DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
    Require(log.committed == log.expected, "draws, labels, DMA, stores, submission ends and flips were not committed in issue order");
}

void ForwardsDoNotWaitForEarlierDraws() {
    if (!DrawFinisher::Sequenced()) return;
    Log log;
    Gate gate;
    gate.Close();
    Draw(log, 0, &gate);
    const auto issued = DrawFinisher::Get().Ring().Issued();
    Forward(log, Kind::EndOfPipe, 1, Base + 0x40, Bytes(0x55));
    Forward(log, Kind::DmaWrite, 2, Base + 0x5000);
    Forward(log, Kind::Store, 3, Base + 0x80, Bytes(9));
    Require(DrawFinisher::Busy() && DrawFinisher::Get().Ring().Waiting() >= 3, "a label, DMA or store behind a held draw waited for the draw instead of taking its place in the ring");
    Require(DrawFinisher::Get().Ring().Issued() == issued + 3, "a forwarded item did not take the next sequence number");

    auto& pipeline = DrawPipeline::Queue0();
    const auto label = pipeline.PendingLabel(Base + 0x40, 4);
    Require(label.has_value() && *label == 0x55, "a label waiting in the ring is not visible to the worker");
    std::array<std::byte, 4> stored{};
    Require(pipeline.StoredBytes(Base + 0x80, 4, stored) == DrawPipeline::QueuedWrite::Stored && stored == std::array<std::byte, 4>{std::byte{9}, {}, {}, {}}, "a store waiting in the ring is not answered from its bytes");
    Require(pipeline.StoredBytes(Base + 0x40, 4, {}) == DrawPipeline::QueuedWrite::Unknown, "a label waiting in the ring answered a store query");
    Require(pipeline.Overlaps(Base + 0x5008, 4), "a DMA write waiting in the ring is not visible to the worker");
    Require(pipeline.Overlaps(Base + 0x1800, 4), "a held draw's writes are not visible to the worker");
    Require(!pipeline.Overlaps(Base + 0x9000, 4), "an untouched range overlaps the ring");
    Require(DrawFinisher::Get().PendingKey(0x1000, 0), "a held draw's key is not visible to the worker");
    gate.Open();
    pipeline.Drain(DrawPipeline::DrainReason::Submission);
    Require(log.committed == log.expected, "items behind a held draw committed out of order");
    Require(pipeline.CommittedSequence() == DrawFinisher::Get().Ring().Issued(), "the committer did not record the last sequence it committed");
    Require(!pipeline.Overlaps(Base + 0x5008, 4) && pipeline.StoredBytes(Base + 0x80, 4, {}) == DrawPipeline::QueuedWrite::None, "committed items stayed visible");
}

void ReadsDoNotWaitInTheRing() {
    if (!DrawFinisher::Sequenced()) return;
    Log log;
    Gate gate;
    gate.Close();
    Draw(log, 0, &gate);
    std::atomic<bool> opened{false};
    std::thread opener([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        opened.store(true);
        gate.Open();
    });
    struct JoinOnExit {
        std::thread& thread;
        ~JoinOnExit() {
            if (thread.joinable()) thread.join();
        }
    } joiner{opener};
    log.expected.emplace_back(Kind::Draw, 1);
    DrawPipeline::Queue0().Enqueue(Kind::Draw, [&log] { log.Commit(Kind::Draw, 1); }, {}, 0, {}, {{Base + 0x7000, Base + 0x7100}}, true);
    const bool waited = opened.load();
    opener.join();
    Require(waited, "an item that reads guest memory was queued behind a held draw instead of waiting for it");
    DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
    Require(log.committed == log.expected, "a reading item committed out of order");
}

void IdleDrainRethrowsAFailedPreparation() {
    DrawFinisher::Get().Enqueue([] { throw std::runtime_error("prepare failed before the drain"); }, {});
    WaitUntil([] { return !DrawFinisher::Busy(); }, "the failed preparation to finish");
    bool thrown = false;
    try {
        DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
    } catch (const std::runtime_error& error) {
        thrown = std::string(error.what()) == "prepare failed before the drain";
    }
    Require(thrown, "a pipeline drain after the failed preparation had finished did not rethrow it");
    DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
}

void ResidencyIsCountedByKind() {
    if (!DrawFinisher::Sequenced() || std::getenv("APS5_PIPELINE_STATS") == nullptr) return;
    static_cast<void>(DrawFinisher::Get().Ring().TakeCounters());
    Log log;
    Gate gate;
    gate.Close();
    Draw(log, 0, &gate);
    Forward(log, Kind::EndOfPipe, 1, Base + 0x40, Bytes(3));
    Forward(log, Kind::DmaWrite, 2, Base + 0x5000);
    gate.Open();
    DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
    const auto counters = DrawFinisher::Get().Ring().TakeCounters();
    const auto tagOf = [](Kind kind) { return 1 + static_cast<std::size_t>(kind); };
    Require(counters.residency[0].items == 1, "the held draw's residency was not counted as a preparation");
    Require(counters.residency[tagOf(Kind::EndOfPipe)].items == 1 && counters.residency[tagOf(Kind::DmaWrite)].items == 1, "forwarded items' residency was not counted by commit kind");
    Require(log.committed == log.expected, "items committed out of order while timed");
}

void FullRingHoldsTheProducer() {
    if (!DrawFinisher::Sequenced()) return;
    Log log;
    Gate gate;
    gate.Close();
    const auto capacity = DrawFinisher::Get().Ring().Capacity();
    std::atomic<std::size_t> issued{0};
    std::exception_ptr error;
    std::thread producer([&] {
        try {
            DrawPipeline::Active() = true;
            Draw(log, 0, &gate);
            issued.fetch_add(1);
            for (std::size_t i = 1; i < capacity + 8; ++i) {
                Forward(log, Kind::DmaFill, static_cast<int>(i), Base + 0x6000);
                issued.fetch_add(1);
            }
            DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
        } catch (...) {
            error = std::current_exception();
        }
    });
    struct JoinOnExit {
        std::thread& thread;
        Gate& gate;
        ~JoinOnExit() {
            gate.Open();
            if (thread.joinable()) thread.join();
        }
    } joiner{producer, gate};
    WaitUntil([&] { return issued.load() == capacity; }, "the ring to fill");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Require(issued.load() == capacity, "the worker issued " + std::to_string(issued.load()) + " items into a ring of " + std::to_string(capacity));
    gate.Open();
    producer.join();
    if (error != nullptr) std::rethrow_exception(error);
    Require(log.committed == log.expected, "items behind a full ring committed out of order");
}

void FailedPrepareReachesTheWorker() {
    Log log;
    DrawFinisher::Get().Enqueue([] { throw std::runtime_error("prepare failed"); }, {});
    bool thrown = false;
    try {
        DrawFinisher::Get().Drain();
    } catch (const std::runtime_error& error) {
        thrown = std::string(error.what()) == "prepare failed";
    }
    Require(thrown, "a failed preparation was not rethrown to the worker");
    Draw(log, 0);
    Forward(log, Kind::EndOfPipe, 1, Base + 0x40, Bytes(1));
    DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
    Require(log.committed == log.expected, "the pipeline did not resume after a failed preparation");
}

}

void DriverThreadsAreNamed() {
#ifdef __linux__
    bool committer = false;
    bool finisher = false;
    for (const auto& task : std::filesystem::directory_iterator("/proc/self/task")) {
        std::ifstream comm(task.path() / "comm");
        std::string name;
        std::getline(comm, name);
        committer = committer || name == "draw-commit";
        finisher = finisher || name == "draw-finish";
    }
    Require(committer, "the draw committer thread is not named draw-commit");
    Require(finisher, "the draw finisher thread is not named draw-finish");
#endif
}

int main() {
    try {
        if (DrawPipeline::Depth() == 0) return 77;
        if (!DrawFinisher::Overlap()) throw std::runtime_error("the draw finisher is disabled; set APS5_DRAW_PIPELINE");
        DrawPipeline::Active() = true;
        MixedItemsCommitInIssueOrder();
        ForwardsDoNotWaitForEarlierDraws();
        ReadsDoNotWaitInTheRing();
        FailedPrepareReachesTheWorker();
        IdleDrainRethrowsAFailedPreparation();
        ResidencyIsCountedByKind();
        FullRingHoldsTheProducer();
        DriverThreadsAreNamed();
        std::cout << "draw sequence tests passed (" << (DrawFinisher::Sequenced() ? "sequence ring" : "drained hand-off") << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
