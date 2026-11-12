#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/CommitEffects.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/CommitPhases.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class StderrCapture {
public:
    StderrCapture() : path(std::filesystem::temp_directory_path() / ("aps5_pipeline_profile_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt")) {
        std::fflush(stderr);
#ifdef _WIN32
        saved = _dup(_fileno(stderr));
        Check(saved >= 0, "cannot save stderr");
        Check(_wfreopen(path.wstring().c_str(), L"wb", stderr) != nullptr, "cannot redirect stderr");
#else
        saved = dup(fileno(stderr));
        Check(saved >= 0, "cannot save stderr");
        Check(std::freopen(path.c_str(), "wb", stderr) != nullptr, "cannot redirect stderr");
#endif
    }

    ~StderrCapture() {
        std::fflush(stderr);
#ifdef _WIN32
        _dup2(saved, _fileno(stderr));
        _close(saved);
#else
        dup2(saved, fileno(stderr));
        close(saved);
#endif
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    std::string Text() const {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    std::string ReadReport(std::string_view prefix = "[draw] pipelined ") const {
        return ReadLine(prefix);
    }

    std::string ReadLine(std::string_view prefix) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            std::ifstream file(path, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            const auto begin = text.find(prefix);
            if (begin != std::string::npos) {
                const auto end = text.find('\n', begin);
                if (end != std::string::npos) return text.substr(begin, end - begin);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("pipeline profile report was not written");
    }

private:
    std::filesystem::path path;
    int saved = -1;
};

std::size_t Count(std::string_view text, std::string_view value) {
    std::size_t count = 0;
    for (std::size_t offset = 0; (offset = text.find(value, offset)) != std::string_view::npos; offset += value.size()) ++count;
    return count;
}

void TestCommitEffects() {
    using namespace AgcDriver::DriverDetail;
    for (std::size_t i = 0; i < static_cast<std::size_t>(DrawPipeline::CommitKind::Count); ++i) {
        const auto kind = static_cast<DrawPipeline::CommitKind>(i);
        const auto effects = CommitEffects(kind);
        Check(effects != 0, "a commit kind has no row in the effects table");
        if ((effects & CommitEffectBit(CommitEffect::Unknown)) != 0) Check(!CommitCovered(kind), "a commit kind with an unknown effect breaks proofs as covered");
    }
    Check(CommitEffectsCovered(DeliveredCommitEffects), "effects that all reach a bump were not covered");
    Check(!CommitEffectsCovered(CommitEffectBit(CommitEffect::CompleteWrites) | CommitEffectBit(CommitEffect::Unknown)), "an unknown effect was covered by a delivered one");
    Check(!CommitEffectsCovered(CommitEffectBit(CommitEffect::CompleteWrites) | CommitEffectBit(CommitEffect::BoundedWrites)), "bounded writes were covered without a bump");
    Check(!CommitEffectsCovered(CommitEffectBit(CommitEffect::CacheReplacement)), "a cache replacement was covered without a bump");
    Check(!CommitEffectsCovered(0), "an empty row was covered");
}

void TestReport() {
    using AgcDriver::DriverDetail::DrawPipeline;
    auto& pipeline = DrawPipeline::Queue0();
    Check(DrawPipeline::Depth() >= 2, "test requires at least two pipeline slots");
    const auto reportAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(10100);
    StderrCapture capture;
    std::binary_semaphore release{0};
    std::exception_ptr failure;
    try {
        const auto certificateBreaks = AgcDriver::GuestMemory::CertificateBreakSerial();
        pipeline.Enqueue(DrawPipeline::CommitKind::DmaWrite, [] {}, {{0x3000, 0x3010}}, 0, {}, {}, false);
        pipeline.Enqueue(DrawPipeline::CommitKind::Seal, [] {}, {}, 0, {}, {}, false);
        const auto committed = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((pipeline.CommitBreaks(DrawPipeline::CommitKind::DmaWrite, true) == 0 || pipeline.CommitBreaks(DrawPipeline::CommitKind::Seal, false) == 0) && std::chrono::steady_clock::now() < committed) std::this_thread::yield();
        Check(AgcDriver::GuestMemory::CertificateBreakSerial() - certificateBreaks == 2, "a commit kind with an unknown effect did not void certificates");
        pipeline.Enqueue(DrawPipeline::CommitKind::Draw, [&] { release.acquire(); }, {}, 0, {}, {{0x1000, 0x1010}, {0x2000, 0x2010}}, true);
        Check(DrawPipeline::HeldReads(0x1000, 4), "queued read range was not held");
        for (unsigned i = 0; i < 3; ++i) pipeline.NoteDecodeDrain();
        DrawPipeline::NoteDbClear(true, false, DrawPipeline::DbClearBlocker::Unproven, 0x410d900);
        DrawPipeline::NoteDbClear(true, false, DrawPipeline::DbClearBlocker::StencilTest, 0x410d900);
        DrawPipeline::NoteDbClear(true, true, DrawPipeline::DbClearBlocker::Unproven, 0x410d900);
        DrawPipeline::NoteDbClear(false, true, DrawPipeline::DbClearBlocker::SampleMask, 0x2000);
        std::this_thread::sleep_until(reportAfter);
        pipeline.Enqueue(DrawPipeline::CommitKind::IndirectDraw, [] {}, {}, 0, {}, {}, true);
    } catch (...) {
        failure = std::current_exception();
    }
    release.release();
    pipeline.Stop();
    if (failure) std::rethrow_exception(failure);
    const auto report = capture.ReadReport();
    std::cout << report << '\n';
    Check(report.find(" 2 read ranges, 1 stores held, 3 decode drains") != std::string::npos, "pipeline read counters changed");
    Check(Count(report, " read ranges") == 1, "pipeline read-range counters were printed more than once");
    Check(Count(report, " stores held") == 1, "pipeline held-store counters were printed more than once");
    Check(Count(report, " decode drains") == 1, "pipeline decode-drain counters were printed more than once");
    Check(report.find("; drains:") != std::string::npos, "pipeline drain counters were lost");
    Check(pipeline.CommitBreaks(DrawPipeline::CommitKind::DmaWrite, true) == 1 && pipeline.CommitBreaks(DrawPipeline::CommitKind::DmaWrite, false) == 0, "a non-draw commit with declared writes was not counted by kind as declared");
    Check(pipeline.CommitBreaks(DrawPipeline::CommitKind::Seal, false) == 1 && pipeline.CommitBreaks(DrawPipeline::CommitKind::Seal, true) == 0, "a non-draw commit without declared writes was not counted by kind as undeclared");
    const auto breaks = capture.ReadLine("[commit-breaks] ");
    Check(breaks.starts_with("[commit-breaks] proof breaks after non-draw commits by kind (10 s):") && breaks.find(" dmawrite 1 (1 with declared writes)") != std::string::npos && breaks.find(" seal 1 (0 with declared writes)") != std::string::npos, "the per-kind proof-break line is missing or wrong");
    Check(report.find("packet drains by opcode:") != std::string::npos, "pipeline opcode counters were lost");
    const auto text = capture.Text();
    const auto idle = text.find("[committer-idle] (10 s): ");
    Check(idle != std::string::npos && text.find(" while the finisher held work (", idle) != std::string::npos && text.find(" while it was idle (", idle) != std::string::npos, "the committer idle counters were not reported");
    const auto clears = capture.ReadReport("[dbclear] ");
    std::cout << clears << '\n';
    Check(clears == "[dbclear] (10 s): 4 rect-list draws with a DB_RENDER_CONTROL clear (depth 2, stencil 1, both 1); first blocker of a native clear: "
                    "stencil test on a depth-only clear 1, partial sample mask 1, layered target 0, rectangle not proven 2; vertex programs: 0x410d900 x3 0x2000 x1", "the DB clear counters differ");
    const auto totals = pipeline.LifetimeTotals();
    Check(totals.enqueued >= 4 && totals.commits == totals.enqueued && totals.drawCommits == 2, "the lifetime totals lost what the report printed and reset");
    Check(totals.commitNs >= 9'000'000'000ull && totals.drawNs >= 9'000'000'000ull && totals.drawNs <= totals.commitNs, "the lifetime totals lost the commits' time");
}

void TestDbClearBlockers() {
    using AgcDriver::DriverDetail::DrawPipeline;
    using Blocker = DrawPipeline::DbClearBlocker;
    Check(DrawPipeline::DbClearBlockerOf(false, true, true, true) == Blocker::StencilTest, "a depth-only clear under a stencil test is not blocked by the test first");
    Check(DrawPipeline::DbClearBlockerOf(true, true, false, false) == Blocker::Unproven, "a stencil clear, which replaces the stencil test, was blocked by it");
    Check(DrawPipeline::DbClearBlockerOf(false, false, true, true) == Blocker::SampleMask, "a partial sample mask is not the next blocker");
    Check(DrawPipeline::DbClearBlockerOf(true, false, false, true) == Blocker::Layered, "a layered target is not a blocker");
    Check(DrawPipeline::DbClearBlockerOf(false, false, false, false) == Blocker::Unproven, "an otherwise eligible clear is not waiting on the rectangle proof");
}

}

void TestFollowEpochForwardOnly() {
    using AgcDriver::DriverDetail::DrawPipeline;
    std::uint64_t followed = 0;
    std::uint64_t afterOlder = 0;
    std::uint64_t afterSame = 0;
    std::uint64_t afterNewer = 0;
    std::uint64_t mintedBackward = 0;
    std::uint64_t beforeBackward = 0;
    std::thread committer([&] {
        DrawPipeline::FollowEpoch(1000);
        followed = AgcDriver::GuestMemory::CollectEpoch();
        DrawPipeline::FollowEpoch(999);
        afterOlder = AgcDriver::GuestMemory::CollectEpoch();
        DrawPipeline::FollowEpoch(1000);
        afterSame = AgcDriver::GuestMemory::CollectEpoch();
        DrawPipeline::FollowEpoch(1001);
        afterNewer = AgcDriver::GuestMemory::CollectEpoch();
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::FollowOlderEpochTokens, true);
        beforeBackward = AgcDriver::GuestMemory::CollectEpoch();
        DrawPipeline::FollowEpoch(1000);
        mintedBackward = AgcDriver::GuestMemory::CollectEpoch();
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::FollowOlderEpochTokens, false);
    });
    committer.join();
    Check(mintedBackward != beforeBackward, "an older epoch token did not take a new collect epoch with APS5_FOLLOW_OLDER_EPOCH_TOKENS flipped on at run time");
    Check(followed != 0, "following an epoch token left the committing thread without a collect epoch");
    Check(afterOlder == followed && afterSame == followed, "a token older than the one already followed took a new collect epoch");
    Check(afterNewer != followed, "a newer epoch token did not take a new collect epoch");
}

void TestAppendedPhasesKeepTheirNames() {
    using AgcDriver::Graphics::CommitPhase;
    struct Named {
        CommitPhase phase;
        std::string_view name;
        std::string_view group;
    };
    const std::array<Named, 52> phases{{
        {CommitPhase::TargetsDepthClaims, "targets.depth.claims", "targets"},
        {CommitPhase::TargetsDepthClear, "targets.depth.clear", "targets"},
        {CommitPhase::TargetsDepthFind, "targets.depth.find", "targets"},
        {CommitPhase::TargetsDepthSync, "targets.depth.sync", "targets"},
        {CommitPhase::TargetsDepthStale, "targets.depth.stale", "targets"},
        {CommitPhase::PlanHitFind, "plan.hit.find", "plan.hit"},
        {CommitPhase::PlanHitWords, "plan.hit.words", "plan.hit"},
        {CommitPhase::PlanHitInstance, "plan.hit.instance", "plan.hit"},
        {CommitPhase::PlanHitTargets, "plan.hit.targets", "plan.hit"},
        {CommitPhase::PlanHitRebind, "plan.hit.rebind", "plan.hit"},
        {CommitPhase::ResolveSetup, "resolve.setup", "resolve"},
        {CommitPhase::ResolveSpace, "resolve.space", "resolve"},
        {CommitPhase::ResolveMoved, "resolve.moved", "resolve"},
        {CommitPhase::ResolveBuild, "resolve.build", "resolve"},
        {CommitPhase::ResolveEvict, "resolve.evict", "resolve"},
        {CommitPhase::TargetsLookupPure, "targets.lookup.pure", "targets"},
        {CommitPhase::TargetsLookupGuards, "targets.lookup.guards", "targets"},
        {CommitPhase::TargetsLookupCache, "targets.lookup.cache", "targets"},
        {CommitPhase::InputsValidate, "inputs.validate", "inputs"},
        {CommitPhase::InputsFlush, "inputs.flush", "inputs"},
        {CommitPhase::InputsInPlace, "inputs.inplace", "inputs"},
        {CommitPhase::InputsReuse, "inputs.reuse", "inputs"},
        {CommitPhase::InputsArm, "inputs.arm", "inputs"},
        {CommitPhase::InputsAlloc, "inputs.alloc", "inputs"},
        {CommitPhase::InputsKeep, "inputs.keep", "inputs"},
        {CommitPhase::InputsIndexRange, "inputs.indexrange", "inputs"},
        {CommitPhase::InputsExpand, "inputs.expand", "inputs"},
        {CommitPhase::InputsLayout, "inputs.layout", "inputs"},
        {CommitPhase::RecordSetup, "record.setup", "record"},
        {CommitPhase::RecordPass, "record.pass", "record"},
        {CommitPhase::RecordBegin, "record.begin", "record"},
        {CommitPhase::RecordBind, "record.bind", "record"},
        {CommitPhase::RecordPush, "record.push", "record"},
        {CommitPhase::RecordCommands, "record.commands", "record"},
        {CommitPhase::RecordLeave, "record.leave", "record"},
        {CommitPhase::RecordAfter, "record.after", "record"},
        {CommitPhase::RecordRecipe, "record.recipe", "record"},
        {CommitPhase::RecordStores, "record.stores", "record"},
        {CommitPhase::RecordPassKey, "record.passkey", "record"},
        {CommitPhase::RecordBeginContinue, "record.begin.continue", "record"},
        {CommitPhase::RecordBeginProxies, "record.begin.proxies", "record"},
        {CommitPhase::RecordBeginBarriers, "record.begin.barriers", "record"},
        {CommitPhase::RecordBeginLds, "record.begin.lds", "record"},
        {CommitPhase::RecordBeginArguments, "record.begin.arguments", "record"},
        {CommitPhase::RecordBeginOrder, "record.begin.order", "record"},
        {CommitPhase::RecordBeginStaged, "record.begin.staged", "record"},
        {CommitPhase::RecordBeginPipeline, "record.begin.pipeline", "record"},
        {CommitPhase::RecordBeginBind, "record.begin.bind", "record"},
        {CommitPhase::RecordBeginDynamic, "record.begin.dynamic", "record"},
        {CommitPhase::RecordBeginLibrary, "record.begin.library", "record"},
        {CommitPhase::RevalidateCertified, "revalidate certified", "revalidate"},
        {CommitPhase::TargetsMemo, "targets.memo", "targets"},
    }};
    const auto first = static_cast<std::size_t>(phases.front().phase);
    for (std::size_t index = 0; index < phases.size(); ++index) {
        const auto& entry = phases[index];
        Check(static_cast<std::size_t>(entry.phase) == first + index, ("commit phase " + std::string(entry.name) + " is not in its appended position").c_str());
        Check(AgcDriver::Graphics::CommitPhaseName(entry.phase) == entry.name, ("commit phase " + std::string(entry.name) + " is named " + AgcDriver::Graphics::CommitPhaseName(entry.phase) + ": the phase enum and its name table are out of order").c_str());
        Check(AgcDriver::Graphics::CommitPhaseGroup(entry.phase) == entry.group, ("commit phase " + std::string(entry.name) + " is in group " + AgcDriver::Graphics::CommitPhaseGroup(entry.phase) + ": the phase enum and its group table are out of order").c_str());
    }
    Check(static_cast<std::size_t>(phases.back().phase) + 1 == static_cast<std::size_t>(CommitPhase::Count), "an appended commit phase is missing from the name test");
}

void TestDrainThrough() {
    using AgcDriver::DriverDetail::DrawPipeline;
    using AgcDriver::Graphics::RuntimeSwitch;
    namespace RuntimeSwitches = AgcDriver::Graphics::RuntimeSwitches;
    auto& pipeline = DrawPipeline::Queue0();
    Check(DrawPipeline::Depth() >= 3, "the drain-through test requires at least three pipeline slots");
    RuntimeSwitches::Assign(RuntimeSwitch::PartialDrains, true);
    std::binary_semaphore first{0};
    std::binary_semaphore second{0};
    std::binary_semaphore third{0};
    std::atomic<bool> thirdCommitted{false};
    pipeline.Enqueue([&] { first.acquire(); }, {{0x10000, 0x10010}});
    pipeline.Enqueue([&] { second.acquire(); }, {{0x20000, 0x20010}});
    pipeline.Enqueue([&] {
        third.acquire();
        thirdCommitted.store(true, std::memory_order_release);
    }, {{0x30000, 0x30010}});
    const std::array<DrawPipeline::Range, 1> elsewhere{{{0x40000, 0x40004}}};
    pipeline.DrainThrough(elsewhere, DrawPipeline::CaptureSite::Decode);
    const bool clearWaited = thirdCommitted.load(std::memory_order_acquire);
    std::atomic<bool> drained{false};
    std::thread waiter([&] {
        const std::array<DrawPipeline::Range, 1> read{{{0x20004, 0x20008}}};
        pipeline.DrainThrough(read, DrawPipeline::CaptureSite::Validate);
        drained.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const bool early = drained.load(std::memory_order_acquire);
    first.release();
    second.release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!drained.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool drainedBeforeThird = drained.load(std::memory_order_acquire) && !thirdCommitted.load(std::memory_order_acquire);
    third.release();
    waiter.join();
    pipeline.Drain(DrawPipeline::DrainReason::Capture);
    RuntimeSwitches::Assign(RuntimeSwitch::PartialDrains, false);
    Check(!clearWaited, "a drain-through over memory no queued item writes waited for the queue");
    Check(!early, "a drain-through returned before the queued item that writes its reads committed");
    Check(drainedBeforeThird, "a drain-through waited for queued items behind the last one that writes its reads");
}

int main() {
    try {
        TestAppendedPhasesKeepTheirNames();
        TestDrainThrough();
        TestFollowEpochForwardOnly();
        TestDbClearBlockers();
        TestCommitEffects();
        TestReport();
        std::cout << "Draw pipeline profile passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
