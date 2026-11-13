#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
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

void PreparerWaitsForAnOlderUnpublishedWrite() {
    if (!DrawFinisher::Sequenced() || DrawFinisher::Preparers() < 2) return;
    Log log;
    Gate gate;
    gate.Close();
    Draw(log, 0, &gate);
    std::atomic<bool> checked{false};
    std::atomic<bool> overlapped{false};
    std::atomic<bool> seen{false};
    std::atomic<bool> committedFirst{false};
    DrawFinisher::Pending held;
    held.unknown = false;
    log.expected.emplace_back(Kind::Draw, 1);
    DrawFinisher::Get().Enqueue([&] {
        auto& pipeline = DrawPipeline::Queue0();
        overlapped.store(!gate.open.load(std::memory_order_acquire), std::memory_order_release);
        seen.store(pipeline.StoredBytes(Base + 0x1800, 4, {}) == DrawPipeline::QueuedWrite::Unknown, std::memory_order_release);
        pipeline.Drain(DrawPipeline::DrainReason::Capture);
        {
            std::lock_guard lock(log.mutex);
            committedFirst.store(!log.committed.empty() && log.committed.front() == std::make_pair(Kind::Draw, 0), std::memory_order_release);
        }
        checked.store(true, std::memory_order_release);
        pipeline.Enqueue(Kind::Draw, [&log] { log.Commit(Kind::Draw, 1); }, {}, 0, {}, {}, true);
    }, std::move(held));
    std::thread opener([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        gate.Open();
    });
    struct JoinOnExit {
        std::thread& thread;
        ~JoinOnExit() {
            if (thread.joinable()) thread.join();
        }
    } joiner{opener};
    DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
    opener.join();
    Require(checked.load() && overlapped.load(), "a second preparer did not run while the older draw was held");
    Require(seen.load(), "a preparer did not see the writes of an older draw that was prepared but not yet published");
    Require(committedFirst.load(), "a preparer's drain returned before the older draw it waited on was committed");
    Require(log.committed == log.expected, "draws prepared by several preparers committed out of order");
}

void OlderBufferedReadsHoldAYoungerFlush() {
    if (!DrawFinisher::Sequenced() || DrawFinisher::Preparers() < 2) return;
    Log log;
    Gate gate;
    gate.Close();
    constexpr std::uint64_t Reads = Base + 0x7000;
    std::atomic<bool> checked{false};
    std::atomic<bool> overlapped{false};
    std::atomic<bool> safe{false};
    DrawFinisher::Pending olderHeld;
    olderHeld.unknown = false;
    log.expected.emplace_back(Kind::Draw, 0);
    DrawFinisher::Get().Enqueue([&] {
        DrawPipeline::Queue0().Enqueue(Kind::Draw, [&log] { log.Commit(Kind::Draw, 0); }, {}, 0, {}, {{Reads, Reads + 0x100}}, true);
        gate.Pass();
    }, std::move(olderHeld));
    DrawFinisher::Pending youngerHeld;
    youngerHeld.unknown = false;
    log.expected.emplace_back(Kind::Draw, 1);
    DrawFinisher::Get().Enqueue([&] {
        overlapped.store(!gate.open.load(std::memory_order_acquire), std::memory_order_release);
        const bool held = DrawPipeline::HeldReads(Reads + 0x40, 4);
        bool committed = false;
        {
            std::lock_guard lock(log.mutex);
            committed = !log.committed.empty();
        }
        safe.store(held || committed, std::memory_order_release);
        checked.store(true, std::memory_order_release);
        DrawPipeline::Queue0().Enqueue(Kind::Draw, [&log] { log.Commit(Kind::Draw, 1); }, {}, 0, {}, {}, true);
    }, std::move(youngerHeld));
    std::thread opener([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        gate.Open();
    });
    struct JoinOnExit {
        std::thread& thread;
        ~JoinOnExit() {
            if (thread.joinable()) thread.join();
        }
    } joiner{opener};
    DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
    opener.join();
    Require(checked.load() && overlapped.load(), "a second preparer did not run while the older draw was held");
    Require(safe.load(), "a younger preparer was told no queued draw reads a range that an older buffered draw reads, so a flush could land before that draw");
    Require(log.committed == log.expected, "draws prepared by several preparers committed out of order");
}

void AbWindowReportsTheSettledPhase() {
    if (!DrawFinisher::Sequenced() || std::getenv("APS5_PIPELINE_STATS") == nullptr) return;
    Log log;
    Require(DrawFinisher::AbWindow(true, 'A', 0).empty(), "an A/B phase that never settled reported a window");
    Require(DrawFinisher::AbWindow(false, 'B', 1).empty(), "a settled A/B phase reported before it ended");
    for (int index = 0; index < 4; ++index) Draw(log, index);
    DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
    const auto line = DrawFinisher::AbWindow(true, 'A', 2);
    const auto jobs = std::to_string(4) + " jobs";
    Require(line.starts_with("[ring-preparers] phase B 1 ") && line.find(jobs) != std::string::npos, "the A/B window did not report the settled phase's jobs: " + line);
    Require(DrawFinisher::AbWindow(true, 'B', 3).empty(), "an A/B window was reported twice");
    Require(log.committed == log.expected, "draws around the A/B window committed out of order");
}

void PacketDrainsAreSplitByClass() {
    using Site = DrawFinisher::WaitSite;
    const std::array<std::uint32_t, 4> dispatch{0xC0031500u, 8u, 8u, 1u};
    const std::array<std::uint32_t, 4> sampleDump{0xC0024600u, 0x39u, 0x1000u, 0u};
    const std::array<std::uint32_t, 4> otherEvent{0xC0024600u, 0x16u, 0x1000u, 0u};
    const std::array<std::uint32_t, 3> nop{0xC0011000u, 0u, 0u};
    Require(DrawFinisher::PacketWaitSite(dispatch) == Site::PacketDispatch, "a dispatch packet drain was not counted as packet_dispatch");
    Require(DrawFinisher::PacketWaitSite(sampleDump) == Site::PacketSampleDump, "a sample-count dump drain was not counted as packet_sample_dump");
    Require(DrawFinisher::PacketWaitSite(otherEvent) == Site::Packet, "an EVENT_WRITE that is not a sample dump left the packet site");
    Require(DrawFinisher::PacketWaitSite(nop) == Site::Packet, "a plain packet left the packet site");
    Require(std::string(DrawFinisher::WaitSiteNames[static_cast<std::size_t>(Site::PacketDispatch)]) == "packet_dispatch", "the packet_dispatch site is misnamed");
    Require(std::string(DrawFinisher::WaitSiteNames[static_cast<std::size_t>(Site::PacketSampleDump)]) == "packet_sample_dump", "the packet_sample_dump site is misnamed");
}

void DrainWaitsAreCountedBySite() {
    using Site = DrawFinisher::WaitSite;
    const auto index = [](Site site) { return static_cast<std::size_t>(site); };
    const auto before = DrawFinisher::SiteWaitTotals();
    Log log;
    const auto heldDrain = [&](Site site, int first, int draws) {
        Gate gate;
        gate.Close();
        Draw(log, first, &gate);
        for (int extra = 1; extra < draws; ++extra) Draw(log, first + extra);
        std::thread opener([&gate] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            gate.Open();
        });
        DrawFinisher::Get().Drain(site);
        opener.join();
    };
    heldDrain(Site::Packet, 0, 3);
    heldDrain(Site::Lookup, 3, 1);
    DrawFinisher::Get().Drain(Site::Packet);
    DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
    const auto after = DrawFinisher::SiteWaitTotals();
    const auto waits = [&](Site site) { return after[index(site)].waits - before[index(site)].waits; };
    const auto waitNs = [&](Site site) { return after[index(site)].waitNs - before[index(site)].waitNs; };
    const auto queued = [&](Site site) { return after[index(site)].queued - before[index(site)].queued; };
    Require(waits(Site::Packet) == 1, "a packet drain of a busy finisher was not counted once, or an idle one was counted");
    Require(waits(Site::Lookup) == 1, "a lookup drain of a busy finisher was not counted at its own site");
    Require(waitNs(Site::Packet) >= 15'000'000 && waitNs(Site::Lookup) >= 15'000'000, "a drain's wait was not timed to the held draw's release");
    Require(queued(Site::Packet) >= 2 && queued(Site::Lookup) >= 1, "the items queued at a drain's entry were not counted");
    for (std::size_t site = 0; site < after.size(); ++site) {
        if (site == index(Site::Packet) || site == index(Site::Lookup)) continue;
        Require(after[site].waits == before[site].waits, std::string("a drain was counted at the wrong site: ") + DrawFinisher::WaitSiteNames[site]);
    }
    Require(log.committed == log.expected, "draws committed out of order around counted drains");
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

void MixedFrame(Log& log, int& index, int frame) {
    Draw(log, index++);
    Forward(log, Kind::EndOfPipe, index++, Base + 0x40, Bytes(0x300u + frame));
    Draw(log, index++, nullptr, frame % 2 == 0);
    Forward(log, Kind::Store, index++, Base + 0x80, Bytes(9));
    Forward(log, Kind::SubmitEnd, index++);
}

void TheRingSwitchesBetweenSubmissions() {
    const bool started = DrawFinisher::Sequenced();
    Require(!DrawFinisher::FollowSequenceSwitch(), "the ring mode changed although APS5_NO_SEQUENCE_RING was not flipped");
    Log log;
    int index = 0;
    for (int round = 0; round < 4; ++round) {
        const bool before = DrawFinisher::Sequenced();
        MixedFrame(log, index, round);
        Gate gate;
        gate.Close();
        Draw(log, index++, &gate);
        Require(DrawFinisher::Busy(), "the held draw did not keep the finisher busy");
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::NoSequenceRing, before);
        std::thread opener([&gate] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            gate.Open();
        });
        const bool flipped = DrawFinisher::FollowSequenceSwitch();
        const bool waited = gate.open.load(std::memory_order_acquire);
        opener.join();
        Require(waited, "the ring mode changed before the finisher had run the held draw");
        Require(flipped && DrawFinisher::Sequenced() == !before, "flipping APS5_NO_SEQUENCE_RING between submissions did not change the ring mode");
        Require(!DrawFinisher::Busy(), "the finisher still held work after the ring mode changed");
        for (int frame = 0; frame < 3; ++frame) MixedFrame(log, index, frame);
        DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
        DrawFinisher::Get().Drain(DrawFinisher::WaitSite::Other);
        DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
        Require(log.committed == log.expected, std::string("items were lost or reordered across a switch to ") + (before ? "the drained hand-off" : "the sequence ring"));
    }
    AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::NoSequenceRing, !started);
    DrawFinisher::FollowSequenceSwitch();
    Require(DrawFinisher::Sequenced() == started, "the ring mode did not return to its start");
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
        PreparerWaitsForAnOlderUnpublishedWrite();
        OlderBufferedReadsHoldAYoungerFlush();
        AbWindowReportsTheSettledPhase();
        DrainWaitsAreCountedBySite();
        PacketDrainsAreSplitByClass();
        DriverThreadsAreNamed();
        TheRingSwitchesBetweenSubmissions();
        std::cout << "draw sequence tests passed (" << (DrawFinisher::Sequenced() ? "sequence ring" : "drained hand-off") << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
