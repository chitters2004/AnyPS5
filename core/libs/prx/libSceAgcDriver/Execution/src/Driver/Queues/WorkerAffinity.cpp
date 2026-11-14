#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/TracyZone.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include "prx/libc/include/general/ThreadName.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#ifdef _WIN32
#include <tlhelp32.h>
#else
#include <pthread.h>
#ifdef __linux__
#include <dirent.h>
#include <string>
#include <sched.h>
#include <unistd.h>
#endif
#endif

namespace AgcDriver::DriverDetail {

std::uint64_t WorkerAffinityMask() {
    static const std::uint64_t mask = [] {
        if (std::getenv("APS5_NO_WORKER_AFFINITY") != nullptr) return std::uint64_t{0};
        const auto requested = CpuTopology::MaskFromEnvironment("APS5_WORKER_AFFINITY_MASK");
        if (requested != 0) return requested;
#ifndef __linux__
        if (std::getenv("APS5_WORKER_AFFINITY") == nullptr) return std::uint64_t{0};
#endif
        const auto& layout = CpuTopology::Get();
        return layout.hybrid ? layout.performant : std::uint64_t{0};
    }();
    return mask;
}

void NameDriverThread(const char* name) {
#ifdef _WIN32
    const HRESULT nameResult = ThreadName::SetCurrent(name);
    if (FAILED(nameResult)) std::fprintf(stderr, "[thread-name] could not name %s (0x%08lx)\n", name, static_cast<unsigned long>(nameResult));
#elif defined(__APPLE__)
    const int nameResult = pthread_setname_np(name);
    if (nameResult != 0) std::fprintf(stderr, "[thread-name] could not name %s (%s)\n", name, std::strerror(nameResult));
#else
    const int nameResult = pthread_setname_np(pthread_self(), name);
    if (nameResult != 0) std::fprintf(stderr, "[thread-name] could not name %s (%s)\n", name, std::strerror(nameResult));
#endif
}

#ifdef __linux__
namespace {
void NoteWorkerThread();
}
#endif

void PinWorkerThread(const char* role) {
    TracyZone::NameThread(role);
#ifdef __linux__
    NoteWorkerThread();
#endif
    const auto mask = WorkerAffinityMask();
    if (mask == 0) return;
    static std::once_flag summary;
    std::call_once(summary, [mask] {
        const auto& layout = CpuTopology::Get();
        std::fprintf(stderr, "[affinity] queue workers and presenter -> 0x%llx (hybrid=%d efficient=0x%llx performant=0x%llx process=0x%llx)\n", static_cast<unsigned long long>(mask), layout.hybrid ? 1 : 0, static_cast<unsigned long long>(layout.efficient), static_cast<unsigned long long>(layout.performant), static_cast<unsigned long long>(layout.process));
    });
    CpuTopology::PinTraced(role, nullptr, mask);
}

#if defined(_WIN32) || defined(__linux__)
namespace {

struct CoreSwitches {
    bool committer = false;
    bool finisher = false;
    bool worker = false;
    std::optional<unsigned> logical;
};

struct Reservation {
    CpuTopology::CpuSet set;
    unsigned long thread = 0;
    bool active = true;
};

constexpr std::size_t CommitterSlot = 0;
constexpr std::size_t FinisherSlot = 1;
constexpr std::size_t WorkerSlot = 2;

std::mutex reservationMutex;
std::array<std::optional<Reservation>, 3> reservations;
std::uint64_t workerSwitches = 0;
#ifdef __linux__
std::vector<unsigned long> workerThreads;
std::vector<std::uint32_t> pipelineCpus;
#endif

const CoreSwitches& Switches() {
    static const CoreSwitches switches = [] {
        CoreSwitches result;
        if (std::getenv("APS5_NO_COMMITTER_CORE") != nullptr) return result;
        const char* text = std::getenv("APS5_COMMITTER_CPU");
        if (text != nullptr && *text != '\0') result.logical = static_cast<unsigned>(std::strtoul(text, nullptr, 10));
        const bool workersPinned = WorkerAffinityMask() != 0;
        result.committer = result.logical.has_value() || !workersPinned;
        result.finisher = !workersPinned && std::getenv("APS5_NO_FINISHER_CORE") == nullptr;
        result.worker = !workersPinned;
        return result;
    }();
    return switches;
}

bool SlotEnabled(std::size_t slot) {
    if (slot == CommitterSlot) return Switches().committer;
    if (slot == FinisherSlot) return Switches().finisher;
    return Switches().worker;
}

#ifdef _WIN32
std::vector<CpuTopology::CpuSet> SystemCpuSets() {
    using GetInformation = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    static const auto getInformation = ThreadName::Resolve<GetInformation>("GetSystemCpuSetInformation");
    std::vector<CpuTopology::CpuSet> sets;
    if (getInformation == nullptr) return sets;
    ULONG bytes = 0;
    getInformation(nullptr, 0, &bytes, GetCurrentProcess(), 0);
    std::vector<unsigned char> buffer(bytes);
    if (bytes == 0 || !getInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), bytes, &bytes, GetCurrentProcess(), 0)) return sets;
    const auto process = CpuTopology::Get().process;
    for (ULONG offset = 0; offset + sizeof(SYSTEM_CPU_SET_INFORMATION) <= bytes;) {
        const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        if (entry->Size == 0 || offset + entry->Size > bytes) break;
        offset += entry->Size;
        if (entry->Type != CpuSetInformation) continue;
        const auto& set = entry->CpuSet;
        if (set.Group == 0 && process != 0 && (set.LogicalProcessorIndex >= 64 || ((process >> set.LogicalProcessorIndex) & 1u) == 0)) continue;
        sets.push_back({set.Id, set.Group, set.LogicalProcessorIndex, set.CoreIndex, set.EfficiencyClass, set.LastLevelCacheIndex, set.Parked != 0});
    }
    return sets;
}
#else
std::vector<CpuTopology::CpuSet> SystemCpuSets() {
    const auto& layout = CpuTopology::Get();
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0 || CPU_COUNT(&allowed) > __builtin_popcountll(layout.process)) return {};
    return CpuTopology::SysfsCpuSets("/sys/devices/system/cpu", layout.process, layout.hybrid ? layout.efficient : 0);
}

unsigned long CurrentThread() {
    return static_cast<unsigned long>(gettid());
}

bool PinTo(pid_t thread, const std::vector<std::uint32_t>& cpus) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const auto cpu : cpus) CPU_SET(cpu, &set);
    return sched_setaffinity(thread, sizeof(set), &set) == 0;
}

bool OwnThread(unsigned long tid) {
    return access(("/proc/self/task/" + std::to_string(tid)).c_str(), F_OK) == 0;
}

void NoteWorkerThread() {
    std::lock_guard lock(reservationMutex);
    workerThreads.push_back(CurrentThread());
    if (!pipelineCpus.empty()) static_cast<void>(PinTo(0, pipelineCpus));
}
#endif

bool WorkerCoreWanted() {
    return !Graphics::RuntimeSwitches::Set(Graphics::RuntimeSwitch::NoWorkerCore);
}

#ifdef _WIN32
using SetCpuSets = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);

SetCpuSets ProcessDefaultSetter() {
    static const auto setter = ThreadName::Resolve<SetCpuSets>("SetProcessDefaultCpuSets");
    return setter;
}

SetCpuSets ThreadSetter() {
    static const auto setter = ThreadName::Resolve<SetCpuSets>("SetThreadSelectedCpuSets");
    return setter;
}

unsigned long CurrentThread() {
    return GetCurrentThreadId();
}
#endif

bool HoldsReservation(unsigned long thread) {
    for (const auto& held : reservations) {
        if (held.has_value() && held->active && held->thread == thread) return true;
    }
    return false;
}

std::vector<CpuTopology::CpuSet> AheadOf(const std::vector<CpuTopology::CpuSet>& sets, std::size_t slot, std::vector<CpuTopology::CpuSet> taken) {
    std::vector<CpuTopology::CpuSet> ahead;
    for (std::size_t earlier = 0; earlier < slot; ++earlier) {
        if (reservations[earlier].has_value() || !SlotEnabled(earlier)) continue;
        if (const auto choice = CpuTopology::ChooseReservedCore(sets, taken, earlier == CommitterSlot ? Switches().logical : std::nullopt)) {
            taken.push_back(choice->chosen);
            ahead.push_back(choice->chosen);
        }
    }
    return ahead;
}

std::vector<CpuTopology::CpuSet> AllReserved() {
    std::vector<CpuTopology::CpuSet> sets;
    for (const auto& held : reservations) {
        if (held.has_value()) sets.push_back(held->set);
    }
    return sets;
}

std::vector<CpuTopology::CpuSet> ActiveReserved() {
    std::vector<CpuTopology::CpuSet> sets;
    for (const auto& held : reservations) {
        if (held.has_value() && held->active) sets.push_back(held->set);
    }
    return sets;
}

std::vector<std::uint32_t> OthersExcept(const std::vector<CpuTopology::CpuSet>& sets, const std::vector<CpuTopology::CpuSet>& excluded) {
    std::vector<std::uint32_t> others;
    for (const auto& set : sets) {
        bool held = false;
        for (const auto& other : excluded) held = held || (other.group == set.group && other.core == set.core);
        if (!held) others.push_back(set.id);
    }
    return others;
}

bool PinHolder(const Reservation& held) {
#ifdef _WIN32
    const ULONG id = held.set.id;
    if (held.thread == GetCurrentThreadId()) return ThreadSetter()(GetCurrentThread(), &id, 1) != 0;
    const HANDLE thread = OpenThread(THREAD_SET_LIMITED_INFORMATION, FALSE, held.thread);
    if (thread == nullptr) return false;
    const bool set = ThreadSetter()(thread, &id, 1) != 0;
    CloseHandle(thread);
    return set;
#else
    return PinTo(static_cast<pid_t>(held.thread), {held.set.id});
#endif
}

std::size_t MoveThreads(const std::vector<std::uint32_t>& others) {
    std::size_t moved = 0;
#ifdef _WIN32
    const std::vector<ULONG> ids(others.begin(), others.end());
    if (const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD processId = GetCurrentProcessId();
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != processId || HoldsReservation(entry.th32ThreadID)) continue;
            const HANDLE other = OpenThread(THREAD_SET_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (other == nullptr) continue;
            if (ThreadSetter()(other, ids.data(), static_cast<ULONG>(ids.size())) != 0) ++moved;
            CloseHandle(other);
        }
        CloseHandle(snapshot);
    }
#else
    if (DIR* tasks = opendir("/proc/self/task"); tasks != nullptr) {
        while (const dirent* entry = readdir(tasks)) {
            char* end = nullptr;
            const unsigned long tid = std::strtoul(entry->d_name, &end, 10);
            if (end == entry->d_name || *end != '\0' || HoldsReservation(tid)) continue;
            if (PinTo(static_cast<pid_t>(tid), others)) ++moved;
        }
        closedir(tasks);
    }
#endif
    return moved;
}

const char* SetProcessDefault(const std::vector<std::uint32_t>& others) {
#ifdef _WIN32
    const std::vector<ULONG> ids(others.begin(), others.end());
    return ProcessDefaultSetter()(GetCurrentProcess(), ids.data(), static_cast<ULONG>(ids.size())) != 0 ? "set" : "failed";
#else
    std::uint64_t unreserved = 0;
    for (const auto cpu : others) unreserved |= cpu < 64 ? std::uint64_t{1} << cpu : 0;
    CpuTopology::UnreservedMask().store(unreserved, std::memory_order_release);
    return "set for helper threads";
#endif
}

void KeepWorkersOnCache(const std::vector<CpuTopology::CpuSet>& sets) {
#ifdef __linux__
    pipelineCpus = WorkerAffinityMask() != 0 ? std::vector<std::uint32_t>{} : CpuTopology::PipelineCacheSet(sets, ActiveReserved());
    if (pipelineCpus.empty()) return;
    std::size_t kept = 0;
    for (const auto worker : workerThreads) {
        if (!OwnThread(worker) || HoldsReservation(worker)) continue;
        kept += PinTo(static_cast<pid_t>(worker), pipelineCpus) ? 1u : 0u;
    }
    std::fprintf(stderr, "[affinity] %zu queue workers kept on the %zu other CPUs of the reserved cores' last-level cache\n", kept, pipelineCpus.size());
#else
    static_cast<void>(sets);
#endif
}

bool CpuSetsAvailable() {
#ifdef _WIN32
    return ProcessDefaultSetter() != nullptr && ThreadSetter() != nullptr;
#else
    return true;
#endif
}

void Reserve(std::size_t slot, const char* role, std::optional<unsigned> logical) {
    if (!CpuSetsAvailable()) return;
    std::lock_guard lock(reservationMutex);
    auto& own = reservations[slot];
    if (own.has_value()) {
        own->thread = CurrentThread();
        const bool selected = !own->active || PinHolder(*own);
        std::fprintf(stderr, "[affinity] %s tid=%lu -> core %u logical %u, kept from its previous thread (thread %s)\n", role, CpuTopology::ThreadId(nullptr), static_cast<unsigned>(own->set.core), static_cast<unsigned>(own->set.logical), own->active ? (selected ? "set" : "failed") : "released by APS5_NO_WORKER_CORE");
        return;
    }
    const auto sets = SystemCpuSets();
    auto taken = AllReserved();
    const auto ahead = AheadOf(sets, slot, taken);
    taken.insert(taken.end(), ahead.begin(), ahead.end());
    const auto reservation = CpuTopology::ChooseReservedCore(sets, taken, logical);
    if (!reservation.has_value()) {
        if (logical.has_value()) std::fprintf(stderr, "[affinity] %s: APS5_COMMITTER_CPU=%u names no free logical processor of the process, no core reserved\n", role, *logical);
        else if (slot == WorkerSlot) std::fprintf(stderr, "[affinity] %s: no core reserved, the host has no free core that leaves four to the other threads beside %zu reserved\n", role, taken.size());
        return;
    }
    own = Reservation{reservation->chosen, CurrentThread(), slot != WorkerSlot || WorkerCoreWanted()};
    auto excluded = ActiveReserved();
    excluded.insert(excluded.end(), ahead.begin(), ahead.end());
    const auto others = OthersExcept(sets, excluded);
    const char* processState = SetProcessDefault(others);
    const bool thread = !own->active || PinHolder(*own);
    const std::size_t moved = MoveThreads(others);
    KeepWorkersOnCache(sets);
    if (own->active) std::fprintf(stderr, "[affinity] %s tid=%lu -> core %u logical %u reserved (process default %s, thread %s, %zu running threads moved to the %zu other CPU sets)\n", role, CpuTopology::ThreadId(nullptr), static_cast<unsigned>(reservation->chosen.core), static_cast<unsigned>(reservation->chosen.logical), processState, thread ? "set" : "failed", moved, others.size());
    else std::fprintf(stderr, "[affinity] %s tid=%lu -> core %u logical %u chosen but released while APS5_NO_WORKER_CORE is set (process default %s, %zu running threads moved to the %zu other CPU sets)\n", role, CpuTopology::ThreadId(nullptr), static_cast<unsigned>(reservation->chosen.core), static_cast<unsigned>(reservation->chosen.logical), processState, moved, others.size());
}

void ApplyWorkerSwitch() {
    if (!Switches().worker || !CpuSetsAvailable()) return;
    const bool wanted = WorkerCoreWanted();
    std::lock_guard lock(reservationMutex);
    auto& worker = reservations[WorkerSlot];
    if (!worker.has_value() || worker->active == wanted) return;
    const auto sets = SystemCpuSets();
    if (sets.empty()) return;
    worker->active = wanted;
    auto excluded = ActiveReserved();
    const auto ahead = AheadOf(sets, WorkerSlot, AllReserved());
    excluded.insert(excluded.end(), ahead.begin(), ahead.end());
    const auto others = OthersExcept(sets, excluded);
    const char* processState = SetProcessDefault(others);
    const bool thread = !wanted || PinHolder(*worker);
    const std::size_t moved = MoveThreads(others);
    KeepWorkersOnCache(sets);
    ++workerSwitches;
    std::fprintf(stderr, "[affinity] queue worker 0 tid=%lu core %u logical %u %s by APS5_NO_WORKER_CORE (switch %llu; process default %s, thread %s, %zu running threads moved to the %zu other CPU sets)\n", worker->thread, static_cast<unsigned>(worker->set.core), static_cast<unsigned>(worker->set.logical), wanted ? "reserved" : "released", static_cast<unsigned long long>(workerSwitches), processState, thread ? "set" : "failed", moved, others.size());
}

}
#endif

void ReserveCommitterCore() {
#if defined(_WIN32) || defined(__linux__)
    if (Switches().committer) Reserve(CommitterSlot, "draw committer", Switches().logical);
#endif
}

void ReserveFinisherCore() {
#if defined(_WIN32) || defined(__linux__)
    if (Switches().finisher) Reserve(FinisherSlot, "draw finisher", std::nullopt);
#endif
}

void ReserveWorkerCore(const char* role) {
#if defined(_WIN32) || defined(__linux__)
    if (Switches().worker) Reserve(WorkerSlot, role, std::nullopt);
#else
    static_cast<void>(role);
#endif
}

void ApplyWorkerCoreSwitch() {
#if defined(_WIN32) || defined(__linux__)
    ApplyWorkerSwitch();
#endif
}

std::uint64_t WorkerCoreSwitches() {
#if defined(_WIN32) || defined(__linux__)
    std::lock_guard lock(reservationMutex);
    return workerSwitches;
#else
    return 0;
#endif
}

}
