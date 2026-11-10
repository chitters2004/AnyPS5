#pragma once
// Local diagnostic only. Never in a PR. Wait census: timers at blocking sites, per thread, with a 10 s report.
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#ifdef WC_TRACE
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#endif

// kind: W blocking wait on another thread or the GPU, I idle wait for work, B busy (running the thread's own work), N nested inside another site (do not add)
#define WC_SITES(X) \
    X(FinDrainReloc, "FinDrain.reloc", 'W') \
    X(FinDrainLookup, "FinDrain.lookup", 'W') \
    X(FinDrainReads, "FinDrain.reads", 'W') \
    X(FinDrainKept, "FinDrain.kept", 'W') \
    X(FinDrainEnqueue, "FinDrain.pipeEnqueue", 'W') \
    X(FinDrainPipe, "FinDrain.pipeDrain", 'W') \
    X(FinDrainLife, "FinDrain.lifecycle", 'W') \
    X(FinDrainPkt298, "FinDrain.packet298", 'W') \
    X(FinDrainPkt610, "FinDrain.packet610", 'W') \
    X(FinDrainOther, "FinDrain.other", 'W') \
    X(FinDrainSpin, "FinDrain.spin(in drain)", 'N') \
    X(FinDrainCv, "FinDrain.cvwait(in drain)", 'N') \
    X(FinSettle, "FinSettle", 'W') \
    X(FinPendingKey, "FinPendingKey", 'W') \
    X(FinEnqueue, "FinEnqueue.total", 'W') \
    X(FinEnqueueFull, "FinEnqueue.queueFull(in enqueue)", 'N') \
    X(FinIdle, "Finisher.idle", 'I') \
    X(FinJob, "Finisher.job", 'B') \
    X(CacheLockReloc, "drawCacheMutex.reloc", 'W') \
    X(CacheLockPeek, "drawCacheMutex.peek", 'W') \
    X(CacheLockLookup, "drawCacheMutex.lookup", 'W') \
    X(CommitIdle, "Committer.idle", 'I') \
    X(CommitJob, "Committer.job", 'B') \
    X(CommitEnqueue, "Committer.enqueue.total", 'W') \
    X(CommitEnqueueFull, "Committer.enqueue.full(in enqueue)", 'N') \
    X(CommitDrain, "Committer.drain.total", 'W') \
    X(QwBusy, "QueueWorker.busy", 'B') \
    X(QwIdle, "QueueWorker.idleLoop", 'I') \
    X(QwMutex, "QueueWorker.loopMutex", 'W') \
    X(WaitRegMem, "WAIT_REG_MEM.blocked", 'W') \
    X(PollSleep, "PollSleep", 'N') \
    X(RewindBlocked, "Rewind.blocked", 'W') \
    X(FlipAhead, "FlipHold.flipsAhead", 'W') \
    X(FlipWork, "FlipHold.flipWork", 'W') \
    X(CaptureHold, "CaptureHold.submit", 'W') \
    X(DevDrain, "DeviceDrain.block", 'W') \
    X(DevDrainIdle, "DeviceDrain.WaitIdle(in block)", 'N') \
    X(DevDrainRecorded, "DeviceDrain.WaitRecorded(in block)", 'N') \
    X(GpuTimeline, "Gpu.timelineWait", 'W') \
    X(GpuSerialFor, "Gpu.serialWaitFor", 'W') \
    X(GpuFence, "Gpu.fenceWait.recorder", 'W') \
    X(GpuHookWait, "Gpu.hookUnlockedWait", 'W') \
    X(VkWaitIdle, "vkDeviceWaitIdle/QueueWaitIdle", 'W') \
    X(QueueSubmit, "vkQueueSubmit.recorder", 'W') \
    X(RecorderRelease, "Recorder.releaseThread.idle", 'I') \
    X(ProgressWatcher, "Recorder.progressWatcher.idle", 'I') \
    X(ShaderPrepared, "Shader.preparedSettled", 'W') \
    X(ShaderUnfinished, "Shader.unfinishedWait", 'W') \
    X(ShaderPool, "Shader.pool.idle", 'I') \
    X(PipelineLib, "PipelineLibrary.idleWait", 'W') \
    X(PipelineLibIdle, "PipelineLibrary.worker.idle", 'I') \
    X(BufFence, "GuestBuffer.fenceWait", 'W') \
    X(BufHelpers, "GuestBuffer.helpersDone", 'W') \
    X(BufHelperIdle, "GuestBuffer.helper.idle", 'I') \
    X(BufYield, "GuestBuffer.yieldLoop", 'W') \
    X(MirrorFence, "ResidentMirror.fenceWait", 'W') \
    X(TexFence, "Texture.fenceWait", 'W') \
    X(TexSleep, "Texture.sleep", 'W') \
    X(TexYield, "Texture.yieldLoop", 'W') \
    X(ResFence, "Resources.fenceWait", 'W') \
    X(MapImport, "MapImportGate", 'W') \
    X(ParCmpWait, "ParallelCompare.wait", 'W') \
    X(ParCmpIdle, "ParallelCompare.worker.idle", 'I') \
    X(PresAcquireFence, "Present.acquireFenceWait", 'W') \
    X(PresAcquire, "Present.vkAcquireNextImage", 'W') \
    X(PresQueuePresent, "Present.vkQueuePresent", 'W') \
    X(PresFenceWait, "Present.fenceWait", 'W') \
    X(VoFlipVblank, "VideoOut.flipWaitsVblank", 'W') \
    X(VoRenderingWait, "VideoOut.renderingWait", 'W') \
    X(VoFlipRoom, "VideoOut.flipRoomWait", 'W') \
    X(VoWaitVblank, "VideoOut.sceWaitVblank", 'W') \
    X(VoPresentIdle, "VideoOut.presentLoop.idle", 'I') \
    X(VoVblankSleep, "VideoOut.vblankLoop.sleep", 'I')

namespace WaitCensus {

enum Id : int {
#define WC_ENUM(id, name, kind) id,
    WC_SITES(WC_ENUM)
#undef WC_ENUM
    SiteCount,
    GpuBase = SiteCount,
    Total = SiteCount + 32
};

inline const char* const Names[] = {
#define WC_NAME(id, name, kind) name,
    WC_SITES(WC_NAME)
#undef WC_NAME
};
inline const char Kinds[] = {
#define WC_KIND(id, name, kind) kind,
    WC_SITES(WC_KIND)
#undef WC_KIND
};

struct Cell {
    std::atomic<std::uint64_t> count{0}, blocked{0}, ns{0}, max{0}, slow{0};
};

struct Row {
    char name[48]{};
    unsigned long tid = 0;
    Cell cells[Total];
    Row* next = nullptr;
};

inline std::atomic<Row*> head{nullptr};
inline thread_local Row* localRow = nullptr;
inline thread_local const char* localName = nullptr;
inline std::atomic<const char* const*> gpuNames{nullptr};
inline std::atomic<int> gpuCount{0};
inline std::once_flag started;

inline std::uint64_t Now() {
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

inline void Report();

inline Row* MakeRow() {
    auto* row = new Row();
    row->tid = GetCurrentThreadId();
    if (localName != nullptr) {
        std::snprintf(row->name, sizeof row->name, "%s", localName);
    } else {
        using Get = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        static const auto get = reinterpret_cast<Get>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernelbase.dll"), "GetThreadDescription")));
        PWSTR description = nullptr;
        if (get != nullptr && SUCCEEDED(get(GetCurrentThread(), &description)) && description != nullptr && description[0] != 0) {
            std::snprintf(row->name, sizeof row->name, "%.40ls#%lu", description, row->tid);
        } else {
            std::snprintf(row->name, sizeof row->name, "tid%lu", row->tid);
        }
        if (description != nullptr) LocalFree(description);
    }
    row->next = head.load(std::memory_order_relaxed);
    while (!head.compare_exchange_weak(row->next, row, std::memory_order_release, std::memory_order_relaxed)) {}
    std::call_once(started, [] {
        std::thread([] {
            const char* configured = std::getenv("APS5_WAITS_SEC");
            const int period = configured != nullptr ? std::max(1, std::atoi(configured)) : 10;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(period));
                Report();
            }
        }).detach();
    });
    return row;
}

inline void NameThread(const char* name) {
    localName = name;
    if (localRow != nullptr) std::snprintf(localRow->name, sizeof localRow->name, "%s", name);
}

inline void Record(int id, std::uint64_t begin, std::uint64_t end, bool contended = false) {
    if (localRow == nullptr) localRow = MakeRow();
    const std::uint64_t ns = end > begin ? end - begin : 0;
    auto& cell = localRow->cells[id];
    cell.count.fetch_add(1, std::memory_order_relaxed);
    if (ns >= 2000 || contended) cell.blocked.fetch_add(1, std::memory_order_relaxed);
    cell.ns.fetch_add(ns, std::memory_order_relaxed);
    if (ns > cell.max.load(std::memory_order_relaxed)) cell.max.store(ns, std::memory_order_relaxed);
    if (ns >= 20000) cell.slow.fetch_add(1, std::memory_order_relaxed);
#ifdef WC_TRACE
    if (ns >= 5000 && AgcDriver::PerformanceTrace::Recording()) {
        if (auto* trace = AgcDriver::PerformanceTrace::Thread()) {
            const char* name = id < SiteCount ? Names[id] : (gpuNames.load(std::memory_order_relaxed) != nullptr ? gpuNames.load(std::memory_order_relaxed)[id - GpuBase] : "GpuMutex.site");
            const auto stop = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(end));
            trace->Add("Wait", name, std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(begin)), stop);
        }
    }
#endif
}

struct Scope {
    int id;
    std::uint64_t begin;
    explicit Scope(int id) : id(id), begin(Now()) {}
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void Done() {
        if (id < 0) return;
        Record(id, begin, Now());
        id = -1;
    }
    ~Scope() { Done(); }
};

struct Busy {
    int id;
    std::uint64_t begin = 0;
    explicit Busy(int id) : id(id) {}
    void Begin() { begin = Now(); }
    void End() {
        if (begin == 0) return;
        Record(id, begin, Now());
        begin = 0;
    }
    ~Busy() { End(); }
};

template<typename Lock>
inline void LockTimed(int id, Lock& lock) {
    const auto begin = Now();
    if (lock.try_lock()) {
        Record(id, begin, begin);
        return;
    }
    lock.lock();
    Record(id, begin, Now(), true);
}

inline void SetGpuNames(const char* const* names, int count) {
    gpuNames.store(names, std::memory_order_relaxed);
    gpuCount.store(count, std::memory_order_relaxed);
}

inline void Report() {
    static std::mutex reportMutex;
    static std::vector<std::vector<std::uint64_t>> previous;
    static std::uint64_t lastAt = Now();
    std::lock_guard lock(reportMutex);
    const auto now = Now();
    const double interval = static_cast<double>(now - lastAt) / 1e9;
    lastAt = now;
    struct Line { std::string thread; int id; std::uint64_t count, blocked, ns, max, slow; };
    std::vector<Line> lines;
    std::vector<Row*> rows;
    for (auto* row = head.load(std::memory_order_acquire); row != nullptr; row = row->next) rows.push_back(row);
    std::reverse(rows.begin(), rows.end());
    if (previous.size() < rows.size()) previous.resize(rows.size(), std::vector<std::uint64_t>(Total * 4, 0));
    for (std::size_t r = 0; r < rows.size(); ++r) {
        for (int id = 0; id < Total; ++id) {
            auto& cell = rows[r]->cells[id];
            const auto count = cell.count.load(std::memory_order_relaxed), blocked = cell.blocked.load(std::memory_order_relaxed), ns = cell.ns.load(std::memory_order_relaxed), slow = cell.slow.load(std::memory_order_relaxed);
            auto* before = &previous[r][static_cast<std::size_t>(id) * 4];
            if (count != before[0]) lines.push_back({rows[r]->name, id, count - before[0], blocked - before[1], ns - before[2], cell.max.exchange(0, std::memory_order_relaxed), slow - before[3]});
            before[0] = count;
            before[1] = blocked;
            before[2] = ns;
            before[3] = slow;
        }
    }
    std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.ns > b.ns; });
    std::fprintf(stderr, "[waits] interval %.2f s, %zu rows, t=%.1f s\n", interval, rows.size(), static_cast<double>(now) / 1e9);
    for (const auto& l : lines) {
        const char* name = l.id < SiteCount ? Names[l.id] : (gpuNames.load(std::memory_order_relaxed) != nullptr ? gpuNames.load(std::memory_order_relaxed)[l.id - GpuBase] : "GpuMutex.site");
        const char kind = l.id < SiteCount ? Kinds[l.id] : 'W';
        std::fprintf(stderr, "[waits] %c %-34s thread=%-28s n=%llu blocked=%llu total_ms=%.3f max_us=%.1f slow20=%llu\n", kind, l.id < SiteCount ? name : name, l.thread.c_str(), static_cast<unsigned long long>(l.count), static_cast<unsigned long long>(l.blocked), static_cast<double>(l.ns) / 1e6, static_cast<double>(l.max) / 1e3, static_cast<unsigned long long>(l.slow));
    }
    std::fflush(stderr);
}

}

#define WC_CAT2(a, b) a##b
#define WC_CAT(a, b) WC_CAT2(a, b)
#define WC_SCOPE(id) ::WaitCensus::Scope WC_CAT(wcScope, __LINE__)(::WaitCensus::id)
