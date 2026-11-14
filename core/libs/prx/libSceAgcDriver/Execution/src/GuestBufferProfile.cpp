#include "prx/libSceAgcDriver/Execution/include/GuestBufferProfile.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace AgcDriver::GuestBufferProfile {
namespace {

constexpr std::size_t BuilderSlots = std::size_t{1} << 16u;
constexpr std::uint64_t PagesPerRange = 4096;
constexpr std::uint64_t BuilderPagesPerSubmission = 64;
constexpr std::size_t CompactPages = 65536;
constexpr std::size_t DetailBuffers = 24;
constexpr std::uint64_t AbandonFrames = 16;

struct Record {
    std::uint64_t serial = 0;
    std::uint64_t frame = 0;
    std::uint64_t group = 0;
    std::uint32_t queue = 0;
    std::uint32_t submitter = 0;
    std::vector<std::uint32_t> builders;
    std::uint64_t draws = 0;
    std::uint64_t dispatches = 0;
    std::vector<std::uint64_t> reads;
    std::vector<std::uint64_t> writes;
    std::vector<std::uint64_t> signals;
    std::vector<std::uint64_t> waits;
    std::uint64_t truncated = 0;
    std::uint64_t workerNs = 0;
    std::uint64_t waitNs = 0;
    std::uint64_t committerNs = 0;
    std::uint32_t running = 0;
    bool executed = false;
    bool quiet = false;
};

struct Group {
    std::uint64_t id = 0;
    bool active = false;
};

struct State {
    std::mutex mutex;
    std::map<std::uint64_t, std::unique_ptr<Record>> records;
    std::map<std::uint64_t, std::vector<std::uint64_t>> frames;
    std::uint64_t frame = 0;
    std::uint64_t groups = 0;
    Totals total;
    Totals window;
    std::string detail;
    std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
};

State& state() {
    static auto* const created = new State();
    return *created;
}

std::array<std::atomic<std::uint64_t>, BuilderSlots>& builderTable() {
    static auto* const table = new std::array<std::atomic<std::uint64_t>, BuilderSlots>();
    return *table;
}

std::uint32_t threadIndex() {
    static std::atomic<std::uint32_t> next{0};
    struct Tag {};
    auto& index = HostThreadLocal<std::uint32_t, Tag>();
    if (index == 0) index = next.fetch_add(1, std::memory_order_relaxed) % 0xffffu + 1u;
    return index;
}

Record*& current() {
    struct Tag {};
    return HostThreadLocal<Record*, Tag>();
}

Group& group() {
    struct Tag {};
    return HostThreadLocal<Group, Tag>();
}

void sortPages(std::vector<std::uint64_t>& pages) {
    std::ranges::sort(pages);
    pages.erase(std::ranges::unique(pages).begin(), pages.end());
}

bool intersects(const std::vector<std::uint64_t>& left, const std::vector<std::uint64_t>& right) {
    auto a = left.begin();
    auto b = right.begin();
    while (a != left.end() && b != right.end()) {
        if (*a == *b) return true;
        if (*a < *b) ++a;
        else ++b;
    }
    return false;
}

bool conflicts(const Record& left, const Record& right) {
    return intersects(left.writes, right.writes) || intersects(left.writes, right.reads) || intersects(left.reads, right.writes);
}

std::size_t workBin(std::uint64_t count) {
    if (count == 0) return 0;
    if (count < 10) return 1;
    if (count < 100) return 2;
    if (count < 1000) return 3;
    return 4;
}

std::size_t groupBin(std::uint64_t count) {
    if (count <= 1) return 0;
    if (count <= 3) return 1;
    if (count <= 7) return 2;
    return 3;
}

std::uint64_t cost(const Record& record) {
    return (record.workerNs - std::min(record.workerNs, record.waitNs)) + record.committerNs;
}

void accumulate(Totals& into, const Totals& from) {
    into.frames += from.frames;
    for (std::size_t kind = 0; kind < 2; ++kind) {
        into.submissions[kind] += from.submissions[kind];
        into.buffers[kind] += from.buffers[kind];
        into.mostBuffers[kind] = std::max(into.mostBuffers[kind], from.mostBuffers[kind]);
    }
    for (std::size_t bin = 0; bin < GroupBins; ++bin) into.buffersPerSubmission[bin] += from.buffersPerSubmission[bin];
    for (std::size_t bin = 0; bin < WorkBins; ++bin) {
        into.draws[bin] += from.draws[bin];
        into.dispatches[bin] += from.dispatches[bin];
    }
    into.submitThreads += from.submitThreads;
    into.mostSubmitThreads = std::max(into.mostSubmitThreads, from.mostSubmitThreads);
    into.buildThreads += from.buildThreads;
    into.mostBuildThreads = std::max(into.mostBuildThreads, from.mostBuildThreads);
    into.builtElsewhere += from.builtElsewhere;
    into.unbuilt += from.unbuilt;
    into.pairs += from.pairs;
    into.conflictingPairs += from.conflictingPairs;
    into.conflictingBuffers += from.conflictingBuffers;
    into.labelEdges += from.labelEdges;
    into.ownWaits += from.ownWaits;
    into.outsideWaits += from.outsideWaits;
    into.largeFrames += from.largeFrames;
    into.cyclicFrames += from.cyclicFrames;
    into.truncatedRanges += from.truncatedRanges;
    into.workerNs += from.workerNs;
    into.committerNs += from.committerNs;
    into.lateCommitterNs += from.lateCommitterNs;
    into.criticalNs += from.criticalNs;
    into.largestNs += from.largestNs;
    into.coveringBuffers += from.coveringBuffers;
}

std::string describe(const std::vector<Record*>& records, const std::vector<std::uint32_t>& partners) {
    std::string text;
    for (std::size_t index = 0; index < records.size() && index < DetailBuffers; ++index) {
        const auto& record = *records[index];
        std::string builders;
        for (const auto builder : record.builders) builders += (builders.empty() ? "" : ",") + std::to_string(builder);
        char item[200];
        std::snprintf(item, sizeof(item), " q%x#%llu t%u/%s d%llu c%llu %.2f+%.2f ms r%zu w%zu x%u;", record.queue, static_cast<unsigned long long>(record.serial), record.submitter, builders.empty() ? "-" : builders.c_str(),
                      static_cast<unsigned long long>(record.draws), static_cast<unsigned long long>(record.dispatches), static_cast<double>(record.workerNs - std::min(record.workerNs, record.waitNs)) / 1e6,
                      static_cast<double>(record.committerNs) / 1e6, record.reads.size(), record.writes.size(), partners[index]);
        text += item;
    }
    if (records.size() > DetailBuffers) text += " ...";
    return text;
}

Totals analyse(std::vector<Record*>& records, std::string& detail) {
    Totals result;
    result.frames = 1;
    std::ranges::sort(records, {}, &Record::serial);
    const auto count = records.size();
    std::array<std::set<std::uint64_t>, 2> groupsOfKind;
    std::map<std::uint64_t, std::uint64_t> groupSizes;
    std::set<std::uint32_t> submitters;
    std::set<std::uint32_t> builders;
    std::vector<std::uint64_t> costs(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& record = *records[index];
        const std::size_t kind = record.queue == 0 ? 0 : 1;
        ++result.buffers[kind];
        groupsOfKind[kind].insert(record.group);
        ++groupSizes[record.group];
        ++result.draws[workBin(record.draws)];
        ++result.dispatches[workBin(record.dispatches)];
        submitters.insert(record.submitter);
        builders.insert(record.builders.begin(), record.builders.end());
        if (record.builders.empty()) ++result.unbuilt;
        else if (std::ranges::any_of(record.builders, [&](std::uint32_t builder) { return builder != record.submitter; })) ++result.builtElsewhere;
        result.truncatedRanges += record.truncated;
        result.workerNs += record.workerNs - std::min(record.workerNs, record.waitNs);
        result.committerNs += record.committerNs;
        costs[index] = cost(record);
    }
    for (std::size_t kind = 0; kind < 2; ++kind) {
        result.submissions[kind] = groupsOfKind[kind].size();
        result.mostBuffers[kind] = result.buffers[kind];
    }
    for (const auto& [id, size] : groupSizes) ++result.buffersPerSubmission[groupBin(size)];
    result.submitThreads = result.mostSubmitThreads = submitters.size();
    result.buildThreads = result.mostBuildThreads = builders.size();
    const auto sum = result.workerNs + result.committerNs;
    auto sorted = costs;
    std::ranges::sort(sorted, std::greater<>());
    if (!sorted.empty()) result.largestNs = sorted.front();
    std::uint64_t covered = 0;
    for (const auto value : sorted) {
        if (sum == 0 || static_cast<double>(covered) >= 0.8 * static_cast<double>(sum)) break;
        covered += value;
        ++result.coveringBuffers;
    }
    std::vector<std::uint32_t> partners(count, 0);
    if (count > PairLimit) {
        result.largeFrames = 1;
        result.criticalNs = sum;
        detail = describe(records, partners);
        return result;
    }
    std::vector<std::vector<bool>> edges(count, std::vector<bool>(count, false));
    std::vector<bool> conflicted(count, false);
    auto labelled = edges;
    for (std::size_t first = 0; first < count; ++first) {
        for (std::size_t second = first + 1; second < count; ++second) {
            ++result.pairs;
            if (!conflicts(*records[first], *records[second])) continue;
            ++result.conflictingPairs;
            conflicted[first] = conflicted[second] = true;
            ++partners[first];
            ++partners[second];
            edges[first][second] = true;
        }
    }
    result.conflictingBuffers = static_cast<std::uint64_t>(std::ranges::count(conflicted, true));
    std::map<std::uint64_t, std::vector<std::size_t>> signallers;
    for (std::size_t index = 0; index < count; ++index) {
        for (const auto address : records[index]->signals) {
            auto& list = signallers[address];
            if (list.empty() || list.back() != index) list.push_back(index);
        }
    }
    for (std::size_t waiter = 0; waiter < count; ++waiter) {
        for (const auto address : records[waiter]->waits) {
            const auto found = signallers.find(address);
            if (found == signallers.end()) {
                ++result.outsideWaits;
                continue;
            }
            for (const auto writer : found->second) {
                if (writer == waiter) {
                    ++result.ownWaits;
                    continue;
                }
                if (labelled[writer][waiter]) continue;
                labelled[writer][waiter] = true;
                edges[writer][waiter] = true;
                ++result.labelEdges;
            }
        }
    }
    std::vector<std::size_t> incoming(count, 0);
    for (std::size_t from = 0; from < count; ++from) {
        for (std::size_t to = 0; to < count; ++to) {
            if (edges[from][to]) ++incoming[to];
        }
    }
    std::vector<std::size_t> ready;
    for (std::size_t index = 0; index < count; ++index) {
        if (incoming[index] == 0) ready.push_back(index);
    }
    std::vector<std::uint64_t> finish(count, 0);
    std::vector<std::uint64_t> startAt(count, 0);
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto node = ready.back();
        ready.pop_back();
        ++visited;
        finish[node] = startAt[node] + costs[node];
        result.criticalNs = std::max(result.criticalNs, finish[node]);
        for (std::size_t to = 0; to < count; ++to) {
            if (!edges[node][to]) continue;
            startAt[to] = std::max(startAt[to], finish[node]);
            if (--incoming[to] == 0) ready.push_back(to);
        }
    }
    if (visited != count) {
        result.cyclicFrames = 1;
        result.criticalNs = sum;
    }
    detail = describe(records, partners);
    return result;
}

void closeFrames(State& shared, bool all) {
    for (auto frame = shared.frames.begin(); frame != shared.frames.end();) {
        const auto index = frame->first;
        if (index >= shared.frame) break;
        if (!all && index + 2 > shared.frame) break;
        std::vector<Record*> records;
        bool finished = true;
        bool running = false;
        for (const auto serial : frame->second) {
            const auto found = shared.records.find(serial);
            if (found == shared.records.end()) continue;
            if (!found->second->executed) finished = false;
            if (found->second->running != 0) running = true;
            records.push_back(found->second.get());
        }
        if (running || (!finished && index + AbandonFrames > shared.frame)) {
            if (all) {
                ++frame;
                continue;
            }
            break;
        }
        if (!records.empty()) {
            const auto totals = analyse(records, shared.detail);
            accumulate(shared.total, totals);
            accumulate(shared.window, totals);
        }
        for (const auto serial : frame->second) shared.records.erase(serial);
        frame = shared.frames.erase(frame);
    }
}

double perFrame(std::uint64_t value, std::uint64_t frames) {
    return frames == 0 ? 0.0 : static_cast<double>(value) / static_cast<double>(frames);
}

void report(State& shared, bool now) {
    const auto time = std::chrono::steady_clock::now();
    if ((!now && time - shared.reported < std::chrono::seconds(10)) || shared.window.frames == 0) return;
    const auto seconds = std::chrono::duration<double>(time - shared.reported).count();
    shared.reported = time;
    const auto& w = shared.window;
    const auto frames = w.frames;
    const auto work = w.workerNs + w.committerNs;
    const auto bins = [](const auto& values) {
        std::string text;
        for (const auto value : values) text += (text.empty() ? "" : "/") + std::to_string(value);
        return text;
    };
    AgcDriver::ProfilePrint_nid_no_patch(
        "[guest-cbs] %.0f s, %llu frames: graphics %.1f submits and %.1f buffers per frame (most %llu), compute %.1f submits and %.1f buffers per frame (most %llu); buffers per submit 1/2-3/4-7/8+ %s; "
        "draws per buffer 0/1-9/10-99/100-999/1000+ %s, dispatches %s; threads per frame: %.1f submitting (most %llu), %.1f building (most %llu), %llu buffers built off their submitting thread, %llu with no builder seen; "
        "sharing: %llu of %llu pairs share a 64 KiB page with a writer, %.1f buffers per frame in such a pair, %llu label edges, %llu waits on the buffer's own label, %llu on labels from outside the frame, %llu frames over %zu buffers, %llu cyclic, %llu ranges truncated; "
        "per frame: worker %.2f ms, committer %.2f ms (%.2f ms after its frame closed), critical path %.2f ms (ceiling %.2fx), largest buffer %.0f%%, %.1f buffers make 80%%\n",
        seconds, static_cast<unsigned long long>(frames), perFrame(w.submissions[0], frames), perFrame(w.buffers[0], frames), static_cast<unsigned long long>(w.mostBuffers[0]), perFrame(w.submissions[1], frames), perFrame(w.buffers[1], frames),
        static_cast<unsigned long long>(w.mostBuffers[1]), bins(w.buffersPerSubmission).c_str(), bins(w.draws).c_str(), bins(w.dispatches).c_str(), perFrame(w.submitThreads, frames), static_cast<unsigned long long>(w.mostSubmitThreads),
        perFrame(w.buildThreads, frames), static_cast<unsigned long long>(w.mostBuildThreads), static_cast<unsigned long long>(w.builtElsewhere), static_cast<unsigned long long>(w.unbuilt), static_cast<unsigned long long>(w.conflictingPairs),
        static_cast<unsigned long long>(w.pairs), perFrame(w.conflictingBuffers, frames), static_cast<unsigned long long>(w.labelEdges), static_cast<unsigned long long>(w.ownWaits), static_cast<unsigned long long>(w.outsideWaits),
        static_cast<unsigned long long>(w.largeFrames), PairLimit, static_cast<unsigned long long>(w.cyclicFrames), static_cast<unsigned long long>(w.truncatedRanges), perFrame(w.workerNs, frames) / 1e6, perFrame(w.committerNs, frames) / 1e6,
        perFrame(w.lateCommitterNs, frames) / 1e6, perFrame(w.criticalNs, frames) / 1e6, w.criticalNs == 0 ? 0.0 : static_cast<double>(work) / static_cast<double>(w.criticalNs), work == 0 ? 0.0 : 100.0 * static_cast<double>(w.largestNs) / static_cast<double>(work),
        perFrame(w.coveringBuffers, frames));
    AgcDriver::ProfilePrint_nid_no_patch("[guest-cbs] last frame (queue#serial tsubmit/builders draws dispatches worker+committer reads writes sharing):%s\n", shared.detail.c_str());
    shared.window = Totals{};
}

}

bool Enabled() {
    static const bool enabled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return enabled;
}

void NoteCommandWrite(std::uint64_t address, std::size_t bytes) {
    if (!Enabled()) return;
    const auto thread = threadIndex();
    auto& table = builderTable();
    const auto first = address >> PageShift;
    const auto last = bytes == 0 ? first : (address + bytes - 1) >> PageShift;
    for (auto page = first; page <= last; ++page) {
        auto& slot = table[page % BuilderSlots];
        const auto value = (page << 16u) | thread;
        if (slot.load(std::memory_order_relaxed) != value) slot.store(value, std::memory_order_relaxed);
    }
}

void NoteSubmitted(std::uint64_t serial, std::uint32_t queue, const std::uint32_t* commands, std::size_t dwords, bool flip, bool cpuFlip) {
    if (!Enabled()) return;
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    if (!cpuFlip) {
        auto record = std::make_unique<Record>();
        record->serial = serial;
        record->frame = shared.frame;
        record->queue = queue;
        record->submitter = threadIndex();
        auto& submitGroup = group();
        record->group = submitGroup.active ? submitGroup.id : ++shared.groups;
        if (commands != nullptr && dwords != 0) {
            const auto address = reinterpret_cast<std::uint64_t>(commands);
            const auto first = address >> PageShift;
            const auto last = std::min((address + dwords * sizeof(std::uint32_t) - 1) >> PageShift, first + BuilderPagesPerSubmission - 1);
            auto& table = builderTable();
            for (auto page = first; page <= last; ++page) {
                const auto value = table[page % BuilderSlots].load(std::memory_order_relaxed);
                if ((value >> 16u) != page) continue;
                const auto builder = static_cast<std::uint32_t>(value & 0xffffu);
                if (std::ranges::find(record->builders, builder) == record->builders.end()) record->builders.push_back(builder);
            }
            std::ranges::sort(record->builders);
        }
        shared.frames[shared.frame].push_back(serial);
        shared.records[serial] = std::move(record);
    }
    if (flip || cpuFlip) ++shared.frame;
}

void NoteFlip() {
    if (!Enabled()) return;
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    ++shared.frame;
}

bool Recording() {
    return current() != nullptr;
}

void NotePacketEffect(PacketEffect effect, std::uint64_t address) {
    auto* const record = current();
    if (record == nullptr) return;
    record->quiet = false;
    switch (effect) {
    case PacketEffect::Draw:
        ++record->draws;
        break;
    case PacketEffect::Dispatch:
        ++record->dispatches;
        break;
    case PacketEffect::Wait:
        record->waits.push_back(address & ~std::uint64_t{7});
        record->quiet = true;
        break;
    case PacketEffect::Label:
        record->signals.push_back(address & ~std::uint64_t{7});
        record->quiet = true;
        break;
    case PacketEffect::Other:
        break;
    }
}

void NoteStoreSignal(std::uint64_t address) {
    if (auto* const record = current()) record->signals.push_back(address);
}

void NoteRange(std::uint64_t address, std::size_t bytes, bool write) {
    if (!Enabled() || bytes == 0) return;
    auto* const record = current();
    if (record == nullptr || record->quiet) return;
    const auto first = address >> PageShift;
    auto last = (address + bytes - 1) >> PageShift;
    if (last - first >= PagesPerRange) {
        last = first + PagesPerRange - 1;
        ++record->truncated;
    }
    auto& pages = write ? record->writes : record->reads;
    for (auto page = first; page <= last; ++page) {
        if (pages.empty() || pages.back() != page) pages.push_back(page);
    }
    if (pages.size() > CompactPages) sortPages(pages);
}

void NoteWaitNs(std::uint64_t nanoseconds) {
    if (!Enabled()) return;
    if (auto* const record = current()) record->waitNs += nanoseconds;
}

std::uint64_t Current() {
    if (!Enabled()) return 0;
    const auto* const record = current();
    return record == nullptr ? 0 : record->serial;
}

void AddCommitterNs(std::uint64_t serial, std::uint64_t nanoseconds) {
    if (!Enabled() || serial == 0) return;
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    const auto found = shared.records.find(serial);
    if (found != shared.records.end()) {
        found->second->committerNs += nanoseconds;
        return;
    }
    shared.total.lateCommitterNs += nanoseconds;
    shared.window.lateCommitterNs += nanoseconds;
}

SubmitGroup::SubmitGroup(std::uint32_t count) {
    if (!Enabled() || count == 0) return;
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    auto& submitGroup = group();
    if (submitGroup.active) return;
    submitGroup.id = ++shared.groups;
    submitGroup.active = true;
    active = true;
}

SubmitGroup::~SubmitGroup() {
    if (active) group().active = false;
}

Execution::Execution(std::uint64_t serial) {
    if (!Enabled() || current() != nullptr) return;
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    const auto found = shared.records.find(serial);
    if (found == shared.records.end()) return;
    auto& record = *found->second;
    ++record.running;
    record.executed = false;
    current() = &record;
    active = true;
    start = std::chrono::steady_clock::now();
}

Execution::~Execution() {
    if (!active) return;
    const auto spent = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    auto* const record = current();
    current() = nullptr;
    sortPages(record->reads);
    sortPages(record->writes);
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    record->workerNs += spent;
    record->executed = --record->running == 0;
    closeFrames(shared, false);
    report(shared, false);
}

Totals Closed() {
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    return shared.total;
}

void CloseFinished() {
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    closeFrames(shared, true);
    report(shared, true);
}

}
