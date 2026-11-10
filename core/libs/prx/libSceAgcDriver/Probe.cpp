// Probe.cpp: attribution only, never in a PR. Link-time wraps of pthread locks and emutls, plus guard simulation counters.
// Each DLL holds its own table and appends a snapshot every 5 s to $APS5_PROBE_DIR/<pid>-<module>.txt
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

namespace {
constexpr int Slots = 8192;
enum Op : std::uint64_t { MutexOp = 1, ReadOp = 2, WriteOp = 3, TlsOp = 4 };
struct Slot {
    std::atomic<std::uint64_t> key{0};
    std::atomic<std::uint64_t> lock{0};
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> waited{0};
    std::atomic<std::uint64_t> worst{0};
    std::atomic<std::uint64_t> slow{0};
};
Slot slots[Slots];
constexpr int HoldSlots = 4096;
struct Hold {
    std::atomic<std::uint64_t> lock{0};
    std::atomic<std::uint64_t> since{0};
    std::atomic<std::uint64_t> total{0};
    std::atomic<std::uint64_t> worst{0};
    std::atomic<std::uint64_t> count{0};
};
Hold holds[HoldSlots];
std::atomic<int> started{0};
std::atomic<std::uint64_t> tscPerUs{0};

constexpr int Named = 16;
std::atomic<std::uint64_t> named[Named];
constexpr std::uint64_t BitmapShift = 24;
constexpr std::uint64_t BitmapWords = 1024;
std::atomic<std::uint64_t> bitmap[BitmapWords];
std::atomic<std::uint64_t> hullMin{~0ull}, hullMax{0}, markGeneration{0}, packet{0};
struct Seen {
    std::atomic<std::uint64_t> tag{0}, op{0}, generation{0};
};
constexpr int SeenSlots = 65536;
Seen seen[SeenSlots];
struct GuardStat {
    std::atomic<std::uint64_t> calls{0}, bitmapPass{0}, hullPass{0}, samePacket{0}, sameGeneration{0}, exact{0}, retired{0};
};
GuardStat guards[8];

Slot* slotFor(std::uint64_t key) {
    auto index = (key * 0x9e3779b97f4a7c15ull) >> 51;
    for (int probe = 0; probe < 64; ++probe) {
        auto& slot = slots[(index + probe) % Slots];
        auto current = slot.key.load(std::memory_order_relaxed);
        if (current == key) return &slot;
        if (current == 0) {
            std::uint64_t expected = 0;
            if (slot.key.compare_exchange_strong(expected, key) || expected == key) return &slot;
        }
    }
    return nullptr;
}

Hold* holdFor(void* lock) {
    const auto key = reinterpret_cast<std::uint64_t>(lock);
    auto index = (key * 0x9e3779b97f4a7c15ull) >> 52;
    for (int probe = 0; probe < 32; ++probe) {
        auto& hold = holds[(index + probe) % HoldSlots];
        auto current = hold.lock.load(std::memory_order_relaxed);
        if (current == key) return &hold;
        if (current == 0) {
            std::uint64_t expected = 0;
            if (hold.lock.compare_exchange_strong(expected, key) || expected == key) return &hold;
        }
    }
    return nullptr;
}

void raise(std::atomic<std::uint64_t>& value, std::uint64_t seen) {
    auto current = value.load(std::memory_order_relaxed);
    while (current < seen && !value.compare_exchange_weak(current, seen, std::memory_order_relaxed)) {}
}

void lower(std::atomic<std::uint64_t>& value, std::uint64_t seen) {
    auto current = value.load(std::memory_order_relaxed);
    while (seen < current && !value.compare_exchange_weak(current, seen, std::memory_order_relaxed)) {}
}

DWORD WINAPI dumper(void*) {
    LARGE_INTEGER frequency, a, b;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&a);
    const auto t0 = __rdtsc();
    Sleep(200);
    QueryPerformanceCounter(&b);
    tscPerUs = (__rdtsc() - t0) * frequency.QuadPart / (b.QuadPart - a.QuadPart) / 1000000;
    const auto us = tscPerUs.load() ? tscPerUs.load() : 1;
    char module[MAX_PATH] = "unknown";
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&dumper), &self);
    GetModuleFileNameA(self, module, sizeof(module));
    const char* base = strrchr(module, '\\');
    base = base ? base + 1 : module;
    char path[MAX_PATH * 2];
    const char* dir = getenv("APS5_PROBE_DIR");
    snprintf(path, sizeof(path), "%s/%lu-%s.txt", dir ? dir : ".", GetCurrentProcessId(), base);
    auto file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    static char text[1 << 20];
    const auto start = GetTickCount64();
    for (;;) {
        Sleep(5000);
        int used = snprintf(text, sizeof(text), "T %llu base %llx\n", (unsigned long long)(GetTickCount64() - start), (unsigned long long)self);
        for (auto& slot : slots) {
            const auto key = slot.key.load(std::memory_order_relaxed);
            if (key == 0 || used > (int)sizeof(text) - 256) continue;
            used += snprintf(text + used, sizeof(text) - used, "L %llu %llx %llx %llu %llu %llu %llu\n", (unsigned long long)(key & 7), (unsigned long long)(key >> 3), (unsigned long long)slot.lock.load(), (unsigned long long)slot.count.load(),
                (unsigned long long)(slot.waited.load() / us), (unsigned long long)(slot.worst.load() / us), (unsigned long long)slot.slow.load());
        }
        for (auto& hold : holds) {
            const auto lock = hold.lock.load(std::memory_order_relaxed);
            if (lock == 0 || hold.count.load() == 0 || used > (int)sizeof(text) - 256) continue;
            used += snprintf(text + used, sizeof(text) - used, "H %llx %llu %llu %llu\n", (unsigned long long)lock, (unsigned long long)hold.count.load(), (unsigned long long)(hold.total.load() / us), (unsigned long long)(hold.worst.load() / us));
        }
        for (int kind = 0; kind < 8; ++kind) {
            auto& guard = guards[kind];
            if (guard.calls.load() == 0) continue;
            used += snprintf(text + used, sizeof(text) - used, "G %d %llu %llu %llu %llu %llu %llu %llu\n", kind, (unsigned long long)guard.calls.load(), (unsigned long long)guard.bitmapPass.load(), (unsigned long long)guard.hullPass.load(),
                (unsigned long long)guard.samePacket.load(), (unsigned long long)guard.sameGeneration.load(), (unsigned long long)guard.exact.load(), (unsigned long long)guard.retired.load());
        }
        for (int id = 0; id < Named; ++id) {
            if (named[id].load() != 0) used += snprintf(text + used, sizeof(text) - used, "N %d %llu\n", id, (unsigned long long)named[id].load());
        }
        std::uint64_t marked = 0;
        for (auto& word : bitmap) marked += __popcnt64(word.load(std::memory_order_relaxed));
        used += snprintf(text + used, sizeof(text) - used, "B %llu %llx %llx %llu\n", (unsigned long long)marked, (unsigned long long)hullMin.load(), (unsigned long long)hullMax.load(), (unsigned long long)markGeneration.load());
        DWORD wrote = 0;
        WriteFile(file, text, used, &wrote, nullptr);
    }
}

void begin() {
    int expected = 0;
    if (started.load(std::memory_order_relaxed) == 0 && started.compare_exchange_strong(expected, 1)) {
        if (getenv("APS5_PROBE_DIR")) CloseHandle(CreateThread(nullptr, 0, dumper, nullptr, 0, nullptr));
    }
}

void note(Op op, std::uint64_t site, void* lock, std::uint64_t ticks) {
    auto* slot = slotFor((site << 3) | op);
    if (slot == nullptr) return;
    slot->lock.store(reinterpret_cast<std::uint64_t>(lock), std::memory_order_relaxed);
    slot->count.fetch_add(1, std::memory_order_relaxed);
    slot->waited.fetch_add(ticks, std::memory_order_relaxed);
    raise(slot->worst, ticks);
    const auto us = tscPerUs.load(std::memory_order_relaxed);
    if (us != 0 && ticks > 2 * us) slot->slow.fetch_add(1, std::memory_order_relaxed);
}
}

extern "C" {
int __real_pthread_mutex_lock(pthread_mutex_t* lock);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t* lock);
int __real_pthread_rwlock_wrlock(pthread_rwlock_t* lock);
int __real_pthread_rwlock_unlock(pthread_rwlock_t* lock);
void* __real___emutls_get_address(void* control);

int __wrap_pthread_mutex_lock(pthread_mutex_t* lock) {
    begin();
    const auto site = reinterpret_cast<std::uint64_t>(__builtin_return_address(0));
    const auto t = __rdtsc();
    const auto result = __real_pthread_mutex_lock(lock);
    note(MutexOp, site, lock, __rdtsc() - t);
    return result;
}

int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t* lock) {
    begin();
    const auto site = reinterpret_cast<std::uint64_t>(__builtin_return_address(0));
    const auto t = __rdtsc();
    const auto result = __real_pthread_rwlock_rdlock(lock);
    note(ReadOp, site, lock, __rdtsc() - t);
    return result;
}

int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t* lock) {
    begin();
    const auto site = reinterpret_cast<std::uint64_t>(__builtin_return_address(0));
    const auto t = __rdtsc();
    const auto result = __real_pthread_rwlock_wrlock(lock);
    const auto got = __rdtsc();
    note(WriteOp, site, lock, got - t);
    if (auto* hold = holdFor(lock)) hold->since.store(got, std::memory_order_relaxed);
    return result;
}

int __wrap_pthread_rwlock_unlock(pthread_rwlock_t* lock) {
    if (auto* hold = holdFor(lock)) {
        const auto since = hold->since.exchange(0, std::memory_order_relaxed);
        if (since != 0) {
            const auto ticks = __rdtsc() - since;
            hold->count.fetch_add(1, std::memory_order_relaxed);
            hold->total.fetch_add(ticks, std::memory_order_relaxed);
            raise(hold->worst, ticks);
        }
    }
    return __real_pthread_rwlock_unlock(lock);
}

void* __wrap___emutls_get_address(void* control) {
    auto* slot = slotFor((reinterpret_cast<std::uint64_t>(control) << 3) | TlsOp);
    if (slot != nullptr) slot->count.fetch_add(1, std::memory_order_relaxed);
    return __real___emutls_get_address(control);
}

void ProbeCount(int id) {
    begin();
    named[id].fetch_add(1, std::memory_order_relaxed);
}

void ProbePacket() {
    packet.fetch_add(1, std::memory_order_relaxed);
}

void ProbeMark(std::uint64_t address, std::uint64_t bytes) {
    if (bytes == 0) return;
    for (auto bit = address >> BitmapShift; bit <= (address + bytes - 1) >> BitmapShift; ++bit) {
        if ((bit >> 6) < BitmapWords) bitmap[bit >> 6].fetch_or(1ull << (bit & 63), std::memory_order_relaxed);
    }
    lower(hullMin, address);
    raise(hullMax, address + bytes);
    markGeneration.fetch_add(1, std::memory_order_relaxed);
}

void ProbeRemoved() {
    markGeneration.fetch_add(1, std::memory_order_relaxed);
}

void ProbeGuard(int kind, std::uint64_t address, std::uint64_t bytes) {
    begin();
    auto& guard = guards[kind];
    guard.calls.fetch_add(1, std::memory_order_relaxed);
    if (address >= hullMax.load(std::memory_order_relaxed) || address + bytes <= hullMin.load(std::memory_order_relaxed)) guard.hullPass.fetch_add(1, std::memory_order_relaxed);
    bool hit = false;
    if (bytes != 0) {
        for (auto bit = address >> BitmapShift; bit <= (address + bytes - 1) >> BitmapShift && !hit; ++bit) {
            hit = (bit >> 6) >= BitmapWords || (bitmap[bit >> 6].load(std::memory_order_relaxed) >> (bit & 63)) & 1;
        }
    }
    if (!hit) guard.bitmapPass.fetch_add(1, std::memory_order_relaxed);
    const auto tag = (address * 31 + bytes + kind) | 1;
    auto& entry = seen[(tag * 0x9e3779b97f4a7c15ull) >> 48];
    const auto op = packet.load(std::memory_order_relaxed);
    const auto generation = markGeneration.load(std::memory_order_relaxed);
    if (entry.tag.load(std::memory_order_relaxed) == tag) {
        if (entry.op.load(std::memory_order_relaxed) == op) guard.samePacket.fetch_add(1, std::memory_order_relaxed);
        if (entry.generation.load(std::memory_order_relaxed) == generation) guard.sameGeneration.fetch_add(1, std::memory_order_relaxed);
    }
    entry.tag.store(tag, std::memory_order_relaxed);
    entry.op.store(op, std::memory_order_relaxed);
    entry.generation.store(generation, std::memory_order_relaxed);
}

void ProbeExact(int kind) {
    guards[kind].exact.fetch_add(1, std::memory_order_relaxed);
}

void ProbeRetired(int kind) {
    guards[kind].retired.fetch_add(1, std::memory_order_relaxed);
}
}
