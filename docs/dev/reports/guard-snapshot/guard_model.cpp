#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#ifndef MUTANT
#define MUTANT 0
#endif

using u64 = std::uint64_t;
constexpr u64 Max = std::numeric_limits<u64>::max();
constexpr u64 Base = 0x7c0000000000ull;
constexpr u64 Stride = 0x40000;
constexpr u64 Bytes = 0x10000;

struct Range { u64 a, b; };
struct Registry { std::shared_mutex mutex; std::vector<Range> ranges; };
Registry color, depth;
std::atomic<u64> growth{1};

struct Gap { u64 begin, end; };
bool narrow(Gap& g, u64 a, u64 e, const Range& r) {
    if (r.a < e && a < r.b) return false;
    if (r.b <= a) g.begin = std::max(g.begin, r.b);
    else g.end = std::min(g.end, r.a);
    return true;
}
unsigned classify(Registry& reg, u64 a, u64 e, Gap& g) {
    std::shared_lock lock(reg.mutex);
    unsigned hit = 0;
    for (const auto& r : reg.ranges) hit |= !narrow(g, a, e, r);
    return hit;
}
bool exact(Registry& reg, u64 a, u64 e) {
    std::shared_lock lock(reg.mutex);
    return std::any_of(reg.ranges.begin(), reg.ranges.end(), [&](const Range& r) { return r.a < e && a < r.b; });
}

struct Certificate { u64 growth = 0; std::array<Gap, 4> gaps{}; std::size_t next = 0; };
thread_local Certificate certificate;

bool checkCertificate(u64 a, u64 n) {
    const u64 e = a + n;
    const auto g = growth.load(std::memory_order_acquire);
    if (certificate.growth == g) {
        for (const auto& gap : certificate.gaps) if (gap.begin <= a && a < gap.end && n <= gap.end - a) return false;
    }
    Gap gap{0, Max};
    const bool hitColor = classify(color, a, e, gap);
    const bool hitDepth = classify(depth, a, e, gap);
    if (!hitColor && !hitDepth) {
        if (certificate.growth != g) certificate = Certificate{g};
        certificate.gaps[certificate.next++ % 4] = gap;
        return false;
    }
    return (hitDepth && exact(depth, a, e)) | (hitColor && exact(color, a, e));
}

constexpr std::size_t Capacity = 64;
constexpr u64 Overflow = Max;
struct Snapshot {
    std::atomic<u64> seq{0};
    std::atomic<u64> count{0};
#if MUTANT == 4
    std::array<u64, Capacity> begin{}, end{};
#else
    std::array<std::atomic<u64>, Capacity> begin{}, end{};
#endif
    std::mutex writer;
    std::atomic<unsigned> stale{0};
};
Snapshot snapshot;
struct Entry { u64 a, b; };

#if MUTANT == 4
#define LOAD(x) (x)
#define STORE(x, v) ((x) = (v))
#else
#define LOAD(x) (x).load(std::memory_order_relaxed)
#define STORE(x, v) (x).store((v), std::memory_order_relaxed)
#endif

void publish(const std::vector<Entry>& entries) {
    const u64 s = snapshot.seq.load(std::memory_order_relaxed);
    snapshot.seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    if (entries.size() > Capacity) {
        snapshot.count.store(Overflow, std::memory_order_relaxed);
    } else {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            STORE(snapshot.begin[i], entries[i].a);
            STORE(snapshot.end[i], entries[i].b);
        }
        snapshot.count.store(entries.size(), std::memory_order_relaxed);
    }
    snapshot.seq.store(s + 2, std::memory_order_release);
}

std::vector<Entry> merged(std::vector<Entry> entries) {
    std::sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) { return x.a < y.a; });
    std::vector<Entry> out;
    for (const auto& entry : entries) {
        if (!out.empty() && entry.a <= out.back().b) out.back().b = std::max(out.back().b, entry.b);
        else out.push_back(entry);
    }
    return out;
}

bool snapshotInsert(u64 a, u64 b) {
#if MUTANT == 2
    return true;
#endif
    std::lock_guard lock(snapshot.writer);
    const auto n = snapshot.count.load(std::memory_order_relaxed);
    if (n == Overflow) return false;
    std::vector<Entry> entries;
    for (std::size_t i = 0; i < n; ++i) entries.push_back({LOAD(snapshot.begin[i]), LOAD(snapshot.end[i])});
    entries.push_back({a, b});
    entries = merged(std::move(entries));
    publish(entries);
    return entries.size() <= Capacity;
}

std::atomic<u64> rebuilds{0};
void rebuild() {
    const auto start = growth.load(std::memory_order_acquire);
    std::vector<Entry> entries;
    {
        std::shared_lock lock(color.mutex);
        for (const auto& r : color.ranges) entries.push_back({r.a, r.b});
    }
    std::this_thread::yield();
    {
        std::shared_lock lock(depth.mutex);
        for (const auto& r : depth.ranges) entries.push_back({r.a, r.b});
    }
    entries = merged(std::move(entries));
    std::this_thread::yield();
    std::lock_guard lock(snapshot.writer);
    snapshot.stale.store(0, std::memory_order_relaxed);
#if MUTANT != 3
    if (growth.load(std::memory_order_relaxed) != start) return;
#endif
    publish(entries);
    rebuilds.fetch_add(1, std::memory_order_relaxed);
}

bool checkSnapshot(u64 a, u64 n) {
    const u64 e = a + n;
    bool maybe = true;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const auto s = snapshot.seq.load(std::memory_order_acquire);
        if (s & 1u) continue;
        const auto count = snapshot.count.load(std::memory_order_relaxed);
        bool hit = true;
        if (count != Overflow) {
            const auto limit = std::min<u64>(count, Capacity);
            std::size_t lo = 0, hi = limit;
            while (lo < hi) {
                const auto mid = (lo + hi) / 2;
                if (LOAD(snapshot.end[mid]) <= a) lo = mid + 1;
                else hi = mid;
            }
            hit = lo < limit && LOAD(snapshot.begin[lo]) < e;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (snapshot.seq.load(std::memory_order_relaxed) == s) {
            maybe = hit;
            break;
        }
    }
    if (!maybe) return false;
    const bool found = exact(depth, a, e) || exact(color, a, e);
    if (!found && snapshot.stale.fetch_add(1, std::memory_order_relaxed) + 1 == 8) rebuild();
    return found;
}

bool snapshotDesign = false;
std::atomic<bool> failInsert{false};

void insert(Registry& reg, std::initializer_list<Range> ranges) {
    bool overflow = false;
    {
        std::unique_lock lock(reg.mutex);
#if MUTANT != 5
#if MUTANT != 1
        growth.fetch_add(1, std::memory_order_release);
#endif
        if (snapshotDesign) for (const auto& r : ranges) overflow |= !snapshotInsert(r.a, r.b);
#endif
        bool first = true;
        for (const auto& r : ranges) {
            if (!first && failInsert.load(std::memory_order_relaxed)) {
                lock.unlock();
                throw std::bad_alloc();
            }
            reg.ranges.push_back(r);
            first = false;
        }
#if MUTANT == 5
        growth.fetch_add(1, std::memory_order_release);
        if (snapshotDesign) for (const auto& r : ranges) overflow |= !snapshotInsert(r.a, r.b);
#endif
    }
    if (overflow) rebuild();
}

void removeBelow(Registry& reg, u64 limit) {
    std::unique_lock lock(reg.mutex);
    std::erase_if(reg.ranges, [&](const Range& r) { return r.a < limit; });
}

bool check(u64 a, u64 n) { return snapshotDesign ? checkSnapshot(a, n) : checkCertificate(a, n); }

u64 slot(u64 i) { return Base + i * Stride; }

int main(int argc, char** argv) {
    snapshotDesign = argc > 1 && argv[1][0] == 's';
    const unsigned rounds = argc > 2 ? unsigned(std::atoi(argv[2])) : 4000;
    constexpr unsigned Readers = 3;
    std::atomic<u64> published{0}, removed{0};
    std::atomic<bool> done{false};
    std::atomic<u64> violations{0}, checks{0};
    std::vector<std::thread> readers;
    for (unsigned t = 0; t < Readers; ++t) {
        readers.emplace_back([&, t] {
            while (!done.load(std::memory_order_acquire)) {
                const auto p = published.load(std::memory_order_acquire);
                if (p == 0) continue;
                check(slot(p + 1) + 0x100 * t, 64);
                check(slot(p + 2) + 0x100 * t, 64);
                if (p > 24) check(slot(p - 20) + 0x100 * t, 64);
                const bool seen = check(slot(p) + 0x80, 64);
                checks.fetch_add(1, std::memory_order_relaxed);
                if (!seen && removed.load(std::memory_order_acquire) < p) {
                    violations.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (u64 i = 1; i <= rounds; ++i) {
        auto& reg = i % 3 == 0 ? depth : color;
#if MUTANT == 5
        failInsert.store(i % 5 == 0, std::memory_order_relaxed);
        try {
            insert(reg, {{slot(i), slot(i) + Bytes}, {slot(i) + Bytes, slot(i) + 2 * Bytes}});
        } catch (const std::bad_alloc&) {
        }
#else
        if (i % 5 == 0) {
            failInsert.store(true, std::memory_order_relaxed);
            try {
                insert(reg, {{slot(i), slot(i) + Bytes}, {slot(i) + Bytes, slot(i) + 2 * Bytes}});
            } catch (const std::bad_alloc&) {
            }
            failInsert.store(false, std::memory_order_relaxed);
        } else {
            insert(reg, {{slot(i), slot(i) + Bytes}});
        }
#endif
        published.store(i, std::memory_order_release);
        for (int spin = 0; spin < 200; ++spin) std::this_thread::yield();
        if (i > 16 && i % 4 == 0) {
            removed.store(i - 16, std::memory_order_release);
            removeBelow(color, slot(i - 16));
            removeBelow(depth, slot(i - 16));
        }
    }
    done.store(true, std::memory_order_release);
    for (auto& r : readers) r.join();
    std::printf("%s mutant=%d rounds=%u checks=%llu violations=%llu rebuilds=%llu\n", snapshotDesign ? "snapshot" : "certificate", MUTANT, rounds, (unsigned long long)checks.load(), (unsigned long long)violations.load(), (unsigned long long)rebuilds.load());
    return violations.load() == 0 ? 0 : 1;
}
