#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawTiming.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>

namespace AgcDriver::DriverDetail {

PendingDrawPhases& Driver::pendingDrawPhases() {
    static thread_local PendingDrawPhases pending;
    return pending;
}

void Driver::addDrawPhases(const std::array<double, DrawDriverPhaseCount>& ms, bool drawn, std::uint64_t captures) {
    std::lock_guard lock(drawPhasesMutex);
    auto& totals = drawPhaseTotals;
    for (std::size_t i = 0; i < DrawDriverPhaseCount; ++i) totals.ms[i] += ms[i];
    ++totals.packets;
    if (drawn) ++totals.drawn;
    totals.captures += captures;
    const auto now = std::chrono::steady_clock::now();
    if (now - totals.lastReport < std::chrono::seconds(10)) return;
    totals.lastReport = now;
    std::string report;
    double total = 0;
    for (std::size_t i = 0; i < DrawDriverPhaseCount; ++i) {
        char text[64];
        std::snprintf(text, sizeof(text), " %s %.1f", DrawDriverPhaseNames[i], totals.ms[i] * 1000 / static_cast<double>(totals.packets));
        report += text;
        total += totals.ms[i];
    }
    AgcDriver::ProfilePrint_nid_no_patch( "[draw] driver phases (10 s, %llu draw packets, %llu drawn, %llu captures), us per packet:%s, total %.1f (%.1f ms; Graphics::Draw %.1f ms, skipped %.1f ms)\n", static_cast<unsigned long long>(totals.packets), static_cast<unsigned long long>(totals.drawn), static_cast<unsigned long long>(totals.captures), report.c_str(), total * 1000 / static_cast<double>(totals.packets), total, totals.ms[DrawRowGraphics], totals.ms[DrawRowSkipped]);
    totals.ms = {};
    totals.packets = totals.drawn = totals.captures = 0;
}

void DrawPhaseTiming::Phase(DrawDriverPhase which) {
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    phaseMs[which] += std::chrono::duration<double, std::milli>(now - phaseLap).count();
    if (ThreadPhases::Enabled()) ThreadPhases::AddPhase(which, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - phaseLap).count()));
    phaseLap = now;
}

namespace {

thread_local PhaseThread phaseRole = PhaseThread::Other;
std::array<std::array<std::atomic<std::uint64_t>, ThreadPhaseTotals::Groups>, ThreadPhaseTotals::Threads> phaseGroupNs{};
std::array<std::atomic<std::uint64_t>, ThreadPhaseTotals::Threads> phaseLapNs{}, phaseDrawNs{}, phaseDraws{};
std::atomic<std::uint64_t> phaseBusyNs{0}, phaseSubmissions{0};
std::array<std::atomic<std::uint64_t>, ThreadPhaseTotals::PacketBuckets> phasePacketNs{}, phasePackets{};
std::atomic<std::uint64_t> phasePacketDrains{0}, phasePacketDrainNs{0}, phaseEndDrains{0}, phaseEndDrainNs{0}, phaseRoomWaits{0}, phaseRoomWaitNs{0};
std::atomic<std::int64_t> phaseWindowStart{std::chrono::steady_clock::now().time_since_epoch().count()};

std::size_t groupOf(DrawDriverPhase which) {
    switch (which) {
    case DrawRowDecode:
    case DrawRowProgramPrepare:
    case DrawRowCapture:
    case DrawRowCaptureHookWaits:
    case DrawRowRecompile:
        return static_cast<std::size_t>(PhaseGroup::Prepare);
    case DrawRowKeyLookupValidate:
    case DrawRowValidateWait:
        return static_cast<std::size_t>(PhaseGroup::Lookup);
    case DrawRowLockWait:
        return static_cast<std::size_t>(PhaseGroup::Handoff);
    case DrawRowGraphics:
        return static_cast<std::size_t>(PhaseGroup::Graphics);
    default:
        return ThreadPhaseTotals::Groups;
    }
}

}

bool ThreadPhases::Enabled() {
    static const bool enabled = std::getenv("APS5_DRAW_THREAD_TIMES") != nullptr || std::getenv("APS5_PIPELINE_STATS") != nullptr || std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return enabled;
}

PhaseThread ThreadPhases::Current() {
    return phaseRole;
}

ThreadPhases::Role::Role(PhaseThread role) : previous(phaseRole) {
    phaseRole = role;
}

ThreadPhases::Role::~Role() {
    phaseRole = previous;
}

void ThreadPhases::AddPhase(DrawDriverPhase which, std::uint64_t ns) {
    phaseLapNs[static_cast<std::size_t>(phaseRole)].fetch_add(ns, std::memory_order_relaxed);
    const auto group = groupOf(which);
    if (group == ThreadPhaseTotals::Groups) return;
    phaseGroupNs[static_cast<std::size_t>(phaseRole)][group].fetch_add(ns, std::memory_order_relaxed);
}

void ThreadPhases::AddDraw(std::uint64_t ns) {
    const auto role = static_cast<std::size_t>(phaseRole);
    phaseDrawNs[role].fetch_add(ns, std::memory_order_relaxed);
    phaseDraws[role].fetch_add(1, std::memory_order_relaxed);
}

void ThreadPhases::AddSubmission(std::uint64_t ns) {
    phaseBusyNs.fetch_add(ns, std::memory_order_relaxed);
    phaseSubmissions.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    if (std::chrono::steady_clock::duration(now - phaseWindowStart.load(std::memory_order_relaxed)) < std::chrono::seconds(10)) return;
    AgcDriver::ProfilePrint_nid_no_patch("%s\n", Text(Take()).c_str());
    AgcDriver::ProfilePrint_nid_no_patch("%s\n", BoundaryTrace::Text(BoundaryTrace::Take()).c_str());
}

void ThreadPhases::AddPacket(std::size_t bucket, std::uint64_t ns) {
    const auto index = std::min(bucket, ThreadPhaseTotals::NoPacket);
    phasePacketNs[index].fetch_add(ns, std::memory_order_relaxed);
    phasePackets[index].fetch_add(1, std::memory_order_relaxed);
}

void ThreadPhases::AddPacketDrain(std::uint64_t ns) {
    phasePacketDrainNs.fetch_add(ns, std::memory_order_relaxed);
    phasePacketDrains.fetch_add(1, std::memory_order_relaxed);
}

void ThreadPhases::AddEndDrain(std::uint64_t ns) {
    phaseEndDrainNs.fetch_add(ns, std::memory_order_relaxed);
    phaseEndDrains.fetch_add(1, std::memory_order_relaxed);
}

void ThreadPhases::AddFinisherRoomWait(std::uint64_t ns) {
    phaseRoomWaitNs.fetch_add(ns, std::memory_order_relaxed);
    phaseRoomWaits.fetch_add(1, std::memory_order_relaxed);
}

ThreadPhaseTotals ThreadPhases::Take() {
    ThreadPhaseTotals totals;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    totals.windowNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::duration(now - phaseWindowStart.exchange(now, std::memory_order_relaxed))).count());
    for (std::size_t thread = 0; thread < ThreadPhaseTotals::Threads; ++thread) {
        for (std::size_t group = 0; group < ThreadPhaseTotals::Groups; ++group) totals.groupNs[thread][group] = phaseGroupNs[thread][group].exchange(0, std::memory_order_relaxed);
        totals.lapNs[thread] = phaseLapNs[thread].exchange(0, std::memory_order_relaxed);
        totals.drawNs[thread] = phaseDrawNs[thread].exchange(0, std::memory_order_relaxed);
        totals.draws[thread] = phaseDraws[thread].exchange(0, std::memory_order_relaxed);
    }
    totals.busyNs = phaseBusyNs.exchange(0, std::memory_order_relaxed);
    totals.submissions = phaseSubmissions.exchange(0, std::memory_order_relaxed);
    for (std::size_t bucket = 0; bucket < ThreadPhaseTotals::PacketBuckets; ++bucket) {
        totals.packetNs[bucket] = phasePacketNs[bucket].exchange(0, std::memory_order_relaxed);
        totals.packets[bucket] = phasePackets[bucket].exchange(0, std::memory_order_relaxed);
    }
    totals.packetDrains = phasePacketDrains.exchange(0, std::memory_order_relaxed);
    totals.packetDrainNs = phasePacketDrainNs.exchange(0, std::memory_order_relaxed);
    totals.endDrains = phaseEndDrains.exchange(0, std::memory_order_relaxed);
    totals.endDrainNs = phaseEndDrainNs.exchange(0, std::memory_order_relaxed);
    totals.roomWaits = phaseRoomWaits.exchange(0, std::memory_order_relaxed);
    totals.roomWaitNs = phaseRoomWaitNs.exchange(0, std::memory_order_relaxed);
    return totals;
}

std::string ThreadPhases::PacketText(const ThreadPhaseTotals& totals) {
    constexpr std::size_t Shown = 8;
    std::array<std::size_t, ThreadPhaseTotals::PacketBuckets> order{};
    for (std::size_t bucket = 0; bucket < order.size(); ++bucket) order[bucket] = bucket;
    std::ranges::stable_sort(order, [&](std::size_t left, std::size_t right) { return totals.packetNs[left] > totals.packetNs[right]; });
    std::uint64_t timedNs = 0;
    for (const auto ns : totals.packetNs) timedNs += ns;
    const auto worker = static_cast<std::size_t>(PhaseThread::Worker);
    const auto otherNs = static_cast<double>(totals.busyNs) - static_cast<double>(totals.drawNs[worker]);
    std::string text = " (queue 0 packets outside the draw call, by opcode:";
    for (std::size_t rank = 0; rank < Shown && totals.packetNs[order[rank]] != 0; ++rank) {
        const auto bucket = order[rank];
        char item[64];
        if (bucket == ThreadPhaseTotals::NoPacket) std::snprintf(item, sizeof(item), " filler x%llu %.1f ms", static_cast<unsigned long long>(totals.packets[bucket]), static_cast<double>(totals.packetNs[bucket]) / 1e6);
        else std::snprintf(item, sizeof(item), " 0x%02zx x%llu %.1f ms", bucket, static_cast<unsigned long long>(totals.packets[bucket]), static_cast<double>(totals.packetNs[bucket]) / 1e6);
        text += item;
    }
    char tail[256];
    std::snprintf(tail, sizeof(tail), "; finisher drains before packets %llu, %.1f ms; outside the packet loop %.1f ms, of which the end-of-submission finisher drain %llu, %.1f ms)", static_cast<unsigned long long>(totals.packetDrains), static_cast<double>(totals.packetDrainNs) / 1e6, (otherNs - static_cast<double>(timedNs)) / 1e6, static_cast<unsigned long long>(totals.endDrains), static_cast<double>(totals.endDrainNs) / 1e6);
    return text + tail;
}

std::string ThreadPhases::Text(const ThreadPhaseTotals& totals) {
    const auto ms = [](std::uint64_t ns) { return static_cast<double>(ns) / 1e6; };
    const auto signedMs = [](std::uint64_t whole, std::uint64_t part) { return (static_cast<double>(whole) - static_cast<double>(part)) / 1e6; };
    const auto draws = [&](PhaseThread thread, const char* noun) {
        const auto index = static_cast<std::size_t>(thread);
        const auto& groups = totals.groupNs[index];
        const auto grouped = groups[0] + groups[1] + groups[2] + groups[3];
        char text[288];
        std::snprintf(text, sizeof(text), "%llu %s in %.1f ms (prepare %.1f, lookup %.1f, lock and commit hand-off %.1f, Graphics::Draw %.1f, other phases %.1f, untimed %.1f)", static_cast<unsigned long long>(totals.draws[index]), noun, ms(totals.drawNs[index]), ms(groups[0]), ms(groups[1]), ms(groups[2]), ms(groups[3]), signedMs(totals.lapNs[index], grouped), signedMs(totals.drawNs[index], totals.lapNs[index]));
        return std::string(text);
    };
    const auto worker = static_cast<std::size_t>(PhaseThread::Worker);
    char head[160];
    std::snprintf(head, sizeof(head), "[draw-threads] (%.1f s): queue 0 worker busy %.1f ms in %llu submissions, idle %.1f ms; ", static_cast<double>(totals.windowNs) / 1e9, ms(totals.busyNs), static_cast<unsigned long long>(totals.submissions), signedMs(totals.windowNs, totals.busyNs));
    char packets[64];
    std::snprintf(packets, sizeof(packets), ", other packets %.1f ms", signedMs(totals.busyNs, totals.drawNs[worker]));
    char waits[128];
    std::snprintf(waits, sizeof(waits), "; finisher room waits %llu, %.1f ms (in the waiting thread's draws)", static_cast<unsigned long long>(totals.roomWaits), ms(totals.roomWaitNs));
    return head + draws(PhaseThread::Worker, "draws") + packets + PacketText(totals) + waits + "; finisher " + draws(PhaseThread::Finisher, "jobs") + "; other queues " + draws(PhaseThread::Other, "draws");
}


namespace {

constexpr std::size_t BoundarySlots = std::size_t{1} << 20;

struct BoundarySlot {
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::int64_t> ns{0};
    std::atomic<std::uint64_t> payload{0};
};

std::uint64_t packBoundary(BoundaryEvent event, std::uint32_t finisherJobs, std::uint32_t committerItems) {
    constexpr std::uint32_t Limit = (1u << 28) - 1;
    return static_cast<std::uint64_t>(event) << 56 | static_cast<std::uint64_t>(std::min(finisherJobs, Limit)) << 28 | std::min(committerItems, Limit);
}

BoundaryRecord unpackBoundary(std::int64_t ns, std::uint64_t payload) {
    constexpr std::uint64_t Mask = (std::uint64_t{1} << 28) - 1;
    return {ns, static_cast<BoundaryEvent>(payload >> 56), static_cast<std::uint32_t>(payload >> 28 & Mask), static_cast<std::uint32_t>(payload & Mask)};
}

struct BoundaryRing {
    std::unique_ptr<BoundarySlot[]> slots = std::make_unique<BoundarySlot[]>(BoundarySlots);
    std::atomic<std::uint64_t> head{0};
    std::mutex reader;
    std::uint64_t tail = 0;
    std::int64_t windowStart = std::chrono::steady_clock::now().time_since_epoch().count();
    BoundaryState state;
};

BoundaryRing& boundaryRing() {
    static BoundaryRing ring;
    return ring;
}

std::int64_t boundaryNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::size_t drainLengthBucket(std::uint64_t ns) {
    if (ns < 100000u) return 0;
    if (ns < 1000000u) return 1;
    if (ns < 5000000u) return 2;
    return 3;
}

}

bool BoundaryTrace::Enabled() {
    return ThreadPhases::Enabled();
}

void BoundaryTrace::Note(BoundaryEvent event, std::uint32_t finisherJobs, std::uint32_t committerItems) {
    if (!Enabled()) return;
    auto& ring = boundaryRing();
    const auto index = ring.head.fetch_add(1, std::memory_order_relaxed);
    auto& slot = ring.slots[index & (BoundarySlots - 1)];
    slot.sequence.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    slot.ns.store(boundaryNow(), std::memory_order_relaxed);
    slot.payload.store(packBoundary(event, finisherJobs, committerItems), std::memory_order_relaxed);
    slot.sequence.store(index + 1, std::memory_order_release);
}

BoundaryTotals BoundaryTrace::Take() {
    auto& ring = boundaryRing();
    std::lock_guard lock(ring.reader);
    BoundaryTotals totals;
    const auto end = ring.head.load(std::memory_order_acquire);
    if (end - ring.tail > BoundarySlots) {
        totals.dropped += end - ring.tail - BoundarySlots;
        ring.tail = end - BoundarySlots;
    }
    std::vector<BoundaryRecord> records;
    records.reserve(static_cast<std::size_t>(end - ring.tail));
    for (auto index = ring.tail; index < end; ++index) {
        const auto& slot = ring.slots[index & (BoundarySlots - 1)];
        std::uint64_t sequence = 0;
        for (int spin = 0; spin < 4096; ++spin) {
            sequence = slot.sequence.load(std::memory_order_acquire);
            if (sequence >= index + 1) break;
        }
        if (sequence != index + 1) {
            ++totals.dropped;
            continue;
        }
        const auto ns = slot.ns.load(std::memory_order_relaxed);
        const auto payload = slot.payload.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.sequence.load(std::memory_order_relaxed) == index + 1) records.push_back(unpackBoundary(ns, payload));
        else ++totals.dropped;
    }
    ring.tail = end;
    const auto now = boundaryNow();
    totals.windowNs = static_cast<std::uint64_t>(std::max<std::int64_t>(0, now - ring.windowStart));
    ring.windowStart = now;
    Fold(records, now, ring.state, totals);
    return totals;
}

void BoundaryTrace::Fold(std::vector<BoundaryRecord>& records, std::int64_t until, BoundaryState& state, BoundaryTotals& totals) {
    std::stable_sort(records.begin(), records.end(), [](const BoundaryRecord& a, const BoundaryRecord& b) { return a.ns < b.ns; });
    const auto advance = [&](std::int64_t to) {
        if (!state.started) {
            state.started = true;
            state.cursor = to;
            return;
        }
        if (to <= state.cursor) return;
        const auto span = static_cast<std::uint64_t>(to - state.cursor);
        state.cursor = to;
        if (state.roomWaiters != 0) {
            totals.roomWaitNs += span;
            if (state.draining) {
                totals.roomWaitInDrainNs += span;
                state.drainRoomWaitNs += span;
            }
        }
        if (state.committerBusy) {
            totals.busyNs += span;
            if (state.draining) {
                totals.busyInDrainNs += span;
                state.drainBusyNs += span;
            }
            return;
        }
        totals.idleNs += span;
        if (state.draining) {
            totals.idleInDrainNs += span;
            state.drainIdleNs += span;
        } else if (state.afterDrain) {
            totals.idleAfterDrainNs += span;
        }
    };
    for (const auto& record : records) {
        advance(record.ns);
        switch (record.event) {
        case BoundaryEvent::DrainBegin:
            if (state.draining) break;
            state.draining = true;
            state.afterDrain = false;
            state.drainStart = state.cursor;
            state.drainIdleNs = state.drainBusyNs = state.drainRoomWaitNs = 0;
            ++totals.drains;
            totals.finisherJobsAtDrain += record.finisherJobs;
            if (record.committerItems == 0) ++totals.emptyAtDrain;
            else ++totals.heldAtDrain;
            break;
        case BoundaryEvent::DrainEnd: {
            if (!state.draining) break;
            state.draining = false;
            state.afterDrain = !state.committerBusy;
            const auto length = static_cast<std::uint64_t>(state.cursor - state.drainStart);
            totals.drainNs += length;
            ++totals.drainLengths[drainLengthBucket(length)];
            if (state.drainIdleNs * 2 > length) ++totals.starved;
            else if (state.drainBusyNs * 2 > length && state.drainRoomWaitNs != 0) ++totals.saturated;
            else ++totals.other;
            break;
        }
        case BoundaryEvent::CommitBegin:
            state.committerBusy = true;
            state.afterDrain = false;
            ++totals.commits;
            break;
        case BoundaryEvent::CommitEnd:
            state.committerBusy = false;
            break;
        case BoundaryEvent::RoomWaitBegin:
            ++state.roomWaiters;
            break;
        case BoundaryEvent::RoomWaitEnd:
            if (state.roomWaiters != 0) --state.roomWaiters;
            break;
        }
    }
    advance(until);
}

std::string BoundaryTrace::Text(const BoundaryTotals& totals) {
    const auto ms = [](std::uint64_t ns) { return static_cast<double>(ns) / 1e6; };
    const auto elsewhere = totals.idleNs - totals.idleInDrainNs - totals.idleAfterDrainNs;
    char text[768];
    std::snprintf(text, sizeof(text), "[draw-boundaries] (%.1f s): %llu submission drains, %.1f ms (lengths <0.1/<1/<5/>=5 ms: %llu/%llu/%llu/%llu), %llu finisher jobs outstanding at their starts; committer busy %.1f ms in %llu commits, idle %.1f ms: %.1f during drains, %.1f from a drain's end to the next commit, %.1f elsewhere; committer busy during drains %.1f ms; producer room waits %.1f ms, %.1f during drains; at drain start the committer queue was empty %llu times and held items %llu times; boundaries: %llu starved (committer idle over half the drain), %llu saturated (committer busy over half the drain and a producer waited for room), %llu other; dropped events %llu",
        static_cast<double>(totals.windowNs) / 1e9, static_cast<unsigned long long>(totals.drains), ms(totals.drainNs), static_cast<unsigned long long>(totals.drainLengths[0]), static_cast<unsigned long long>(totals.drainLengths[1]), static_cast<unsigned long long>(totals.drainLengths[2]), static_cast<unsigned long long>(totals.drainLengths[3]), static_cast<unsigned long long>(totals.finisherJobsAtDrain), ms(totals.busyNs), static_cast<unsigned long long>(totals.commits), ms(totals.idleNs), ms(totals.idleInDrainNs), ms(totals.idleAfterDrainNs), ms(elsewhere), ms(totals.busyInDrainNs), ms(totals.roomWaitNs), ms(totals.roomWaitInDrainNs), static_cast<unsigned long long>(totals.emptyAtDrain), static_cast<unsigned long long>(totals.heldAtDrain), static_cast<unsigned long long>(totals.starved), static_cast<unsigned long long>(totals.saturated), static_cast<unsigned long long>(totals.other), static_cast<unsigned long long>(totals.dropped));
    return text;
}

}
