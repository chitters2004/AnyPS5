#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>
#include <x86intrin.h>
#include <time.h>

using u64 = std::uint64_t;
constexpr u64 Max = std::numeric_limits<u64>::max();
constexpr u64 Base = 0x7c0000000000ull;
constexpr u64 Stride = 1ull << 20;
constexpr u64 GuardBytes = 0x10000;
constexpr u64 ChurnBase = Base + 8192 * Stride;

enum Bits : unsigned { ColorBit = 1, DepthBit = 2 };

struct Range { u64 a, b; };
struct Registry {
    std::shared_mutex mutex;
    std::vector<Range> ranges;
};
Registry color, depth;
std::atomic<u64> growth{1};
volatile u64 sink;

bool overlaps(const Range& r, u64 a, u64 e) { return a < r.b && r.a < e; }

unsigned exactOne(Registry& reg, u64 a, u64 e) {
    std::shared_lock lock(reg.mutex);
    for (const auto& r : reg.ranges) if (overlaps(r, a, e)) return 1;
    return 0;
}

unsigned checkMain(u64 a, u64 n) {
    const u64 e = a + n;
    unsigned bits = 0;
    if (exactOne(depth, a, e)) bits |= DepthBit;
    if (exactOne(color, a, e)) bits |= ColorBit;
    return bits;
}

struct Gap { u64 begin, end; };
bool narrow(Gap& g, u64 a, u64 e, const Range& r) {
    if (r.a < e && a < r.b) return false;
    if (r.b <= a) g.begin = std::max(g.begin, r.b);
    else g.end = std::min(g.end, r.a);
    return true;
}
bool gapOf(Registry& reg, u64 a, u64 e, Gap& g) {
    std::shared_lock lock(reg.mutex);
    return std::all_of(reg.ranges.begin(), reg.ranges.end(), [&](const Range& r) { return narrow(g, a, e, r); });
}
unsigned classifyOf(Registry& reg, u64 a, u64 e, Gap& g) {
    std::shared_lock lock(reg.mutex);
    unsigned hit = 0;
    for (const auto& r : reg.ranges) hit |= !narrow(g, a, e, r);
    return hit;
}

struct Certificate {
    u64 growth = 0;
    std::array<Gap, 4> gaps{};
    std::size_t next = 0;
};
thread_local Certificate certificate;
thread_local u64 certMisses;

bool certHit(u64 g, u64 a, u64 n) {
    if (certificate.growth != g) return false;
    for (const auto& gap : certificate.gaps) if (gap.begin <= a && a < gap.end && n <= gap.end - a) return true;
    return false;
}
void remember(u64 g, const Gap& gap) {
    if (certificate.growth != g) certificate = Certificate{g};
    certificate.gaps[certificate.next++ % 4] = gap;
}

bool certClear(u64 a, u64 n) {
    const auto g = growth.load(std::memory_order_acquire);
    if (certHit(g, a, n)) return true;
    ++certMisses;
    Gap gap{0, Max};
    const u64 e = a + n;
    if (!gapOf(color, a, e, gap) || !gapOf(depth, a, e, gap)) return false;
    remember(g, gap);
    return true;
}

unsigned checkCertDraft(u64 a, u64 n) {
    if (certClear(a, n)) return 0;
    unsigned bits = 0;
    if (!certClear(a, n) && exactOne(depth, a, a + n)) bits |= DepthBit;
    if (!certClear(a, n) && exactOne(color, a, a + n)) bits |= ColorBit;
    return bits;
}

unsigned checkCertFixed(u64 a, u64 n) {
    const auto g = growth.load(std::memory_order_acquire);
    if (certHit(g, a, n)) return 0;
    ++certMisses;
    Gap gap{0, Max};
    const u64 e = a + n;
    unsigned hit = classifyOf(color, a, e, gap) ? ColorBit : 0u;
    hit |= classifyOf(depth, a, e, gap) ? DepthBit : 0u;
    if (hit == 0) {
        remember(g, gap);
        return 0;
    }
    unsigned bits = 0;
    if ((hit & DepthBit) && exactOne(depth, a, e)) bits |= DepthBit;
    if ((hit & ColorBit) && exactOne(color, a, e)) bits |= ColorBit;
    return bits;
}

constexpr std::size_t Capacity = 512;
constexpr unsigned StaleRebuild = 64;
constexpr unsigned Retries = 4;
struct Snapshot {
    std::atomic<u64> seq{0};
    std::atomic<u64> count{0};
    std::array<std::atomic<u64>, Capacity> begin{};
    std::array<std::atomic<u64>, Capacity> end{};
    std::mutex writer;
    std::atomic<unsigned> stale{0};
};
Snapshot snapshot;
struct Entry { u64 a, b; unsigned tags; };
thread_local u64 snapRetries, snapFallbacks, snapStale;
std::atomic<u64> rebuilds{0};

constexpr u64 OverflowCount = Max;

void publish(const std::vector<Entry>& entries) {
    const u64 s = snapshot.seq.load(std::memory_order_relaxed);
    snapshot.seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    if (entries.size() > Capacity) {
        snapshot.count.store(OverflowCount, std::memory_order_relaxed);
    } else {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            snapshot.begin[i].store(entries[i].a, std::memory_order_relaxed);
            snapshot.end[i].store(entries[i].b | entries[i].tags, std::memory_order_relaxed);
        }
        snapshot.count.store(entries.size(), std::memory_order_relaxed);
    }
    snapshot.seq.store(s + 2, std::memory_order_release);
}

std::vector<Entry> merged(std::vector<Entry> entries) {
    std::sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) { return x.a < y.a; });
    std::vector<Entry> out;
    for (const auto& entry : entries) {
        if (!out.empty() && entry.a <= out.back().b) {
            out.back().b = std::max(out.back().b, entry.b);
            out.back().tags |= entry.tags;
        } else {
            out.push_back(entry);
        }
    }
    return out;
}

std::vector<Entry> current() {
    std::vector<Entry> entries;
    const auto n = snapshot.count.load(std::memory_order_relaxed);
    if (n == OverflowCount) return entries;
    for (std::size_t i = 0; i < n; ++i) {
        const auto b = snapshot.end[i].load(std::memory_order_relaxed);
        entries.push_back({snapshot.begin[i].load(std::memory_order_relaxed), b & ~u64{3}, unsigned(b & 3)});
    }
    return entries;
}

bool snapshotInsert(u64 a, u64 b, unsigned tag) {
    std::lock_guard lock(snapshot.writer);
    if (snapshot.count.load(std::memory_order_relaxed) == OverflowCount) return false;
    auto entries = current();
    entries.push_back({a & ~u64{3}, (b + 3) & ~u64{3}, tag});
    entries = merged(std::move(entries));
    publish(entries);
    return entries.size() <= Capacity;
}

void rebuild() {
    const auto start = growth.load(std::memory_order_acquire);
    std::vector<Entry> entries;
    {
        std::shared_lock lock(color.mutex);
        for (const auto& r : color.ranges) entries.push_back({r.a & ~u64{3}, (r.b + 3) & ~u64{3}, ColorBit});
    }
    {
        std::shared_lock lock(depth.mutex);
        for (const auto& r : depth.ranges) entries.push_back({r.a & ~u64{3}, (r.b + 3) & ~u64{3}, DepthBit});
    }
    entries = merged(std::move(entries));
    std::lock_guard lock(snapshot.writer);
    snapshot.stale.store(0, std::memory_order_relaxed);
    if (growth.load(std::memory_order_relaxed) != start) return;
    publish(entries);
    rebuilds.fetch_add(1, std::memory_order_relaxed);
}

unsigned snapshotTags(u64 a, u64 e) {
    for (unsigned attempt = 0; attempt < Retries; ++attempt) {
        const auto s = snapshot.seq.load(std::memory_order_acquire);
        if (s & 1u) {
            ++snapRetries;
            _mm_pause();
            continue;
        }
        const auto n = snapshot.count.load(std::memory_order_relaxed);
        unsigned tags = 3;
        if (n != OverflowCount) {
            std::size_t lo = 0, hi = std::min<u64>(n, Capacity);
            while (lo < hi) {
                const auto mid = (lo + hi) / 2;
                if ((snapshot.end[mid].load(std::memory_order_relaxed) & ~u64{3}) <= a) lo = mid + 1;
                else hi = mid;
            }
            tags = 0;
            if (lo < std::min<u64>(n, Capacity)) {
                const auto b = snapshot.end[lo].load(std::memory_order_relaxed);
                if (snapshot.begin[lo].load(std::memory_order_relaxed) < e) tags = unsigned(b & 3);
            }
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (snapshot.seq.load(std::memory_order_relaxed) == s) return tags;
        ++snapRetries;
    }
    return 3;
}

unsigned checkSnapshot(u64 a, u64 n) {
    const u64 e = a + n;
    const auto tags = snapshotTags(a, e);
    if (tags == 0) return 0;
    ++snapFallbacks;
    unsigned bits = 0;
    if ((tags & DepthBit) && exactOne(depth, a, e)) bits |= DepthBit;
    if ((tags & ColorBit) && exactOne(color, a, e)) bits |= ColorBit;
    if (bits == 0) {
        ++snapStale;
        if (snapshot.stale.fetch_add(1, std::memory_order_relaxed) + 1 == StaleRebuild) rebuild();
    }
    return bits;
}

enum class Design { Null, Main, CertDraft, CertFixed, Snap };
Design design;

void insertGuard(Registry& reg, unsigned tag, u64 a, u64 b) {
    bool overflow = false;
    {
        std::unique_lock lock(reg.mutex);
        growth.fetch_add(1, std::memory_order_release);
        if (design == Design::Snap) overflow = !snapshotInsert(a, b, tag);
        reg.ranges.push_back({a, b});
    }
    if (overflow) rebuild();
}

void removeGuard(Registry& reg, u64 a) {
    std::unique_lock lock(reg.mutex);
    std::erase_if(reg.ranges, [&](const Range& r) { return r.a == a; });
}

u64 guardStart(int k) { return Base + u64(k) * Stride; }
u64 gapAddress(int j) { return j < 0 ? Base - Stride / 2 : guardStart(j) + GuardBytes + 0x8000; }

enum class Pattern { Local, Spread, Near, Cyclic5, Cyclic8, Overlap, Retired };
const char* name(Pattern p) {
    switch (p) {
    case Pattern::Local: return "disjoint-local";
    case Pattern::Spread: return "disjoint-spread";
    case Pattern::Near: return "near-neighbour";
    case Pattern::Cyclic5: return "cyclic-5gaps";
    case Pattern::Cyclic8: return "cyclic-8gaps";
    case Pattern::Overlap: return "overlap";
    case Pattern::Retired: return "retired-stale";
    }
    return "";
}
const char* name(Design d) {
    switch (d) {
    case Design::Null: return "null";
    case Design::Main: return "main";
    case Design::CertDraft: return "cert-draft";
    case Design::CertFixed: return "cert-fixed";
    case Design::Snap: return "snapshot";
    }
    return "";
}

struct ReaderStats {
    u64 ops = 0, ns = 0, worst = 0, misses = 0, retries = 0, fallbacks = 0, stale = 0;
    std::vector<std::uint32_t> samples;
};

double tscPerNs;
void calibrate() {
    const auto t0 = std::chrono::steady_clock::now();
    const auto c0 = __rdtsc();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100)) {}
    const auto c1 = __rdtsc();
    tscPerNs = double(c1 - c0) / std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
}

void setup(int guards) {
    color.ranges.clear();
    depth.ranges.clear();
    snapshot.count.store(0);
    snapshot.stale.store(0);
    for (int k = 0; k < guards; ++k) {
        auto& reg = k % 4 == 3 ? depth : color;
        insertGuard(reg, k % 4 == 3 ? DepthBit : ColorBit, guardStart(k), guardStart(k) + GuardBytes);
    }
    for (int c = 0; c < 64; ++c) insertGuard(color, ColorBit, ChurnBase + u64(c) * Stride, ChurnBase + u64(c) * Stride + GuardBytes);
    for (int c = 0; c < 64; ++c) removeGuard(color, ChurnBase + u64(c) * Stride);
}

unsigned check(u64 a, u64 n) {
    switch (design) {
    case Design::Null: return unsigned(a == 0);
    case Design::Main: return checkMain(a, n);
    case Design::CertDraft: return checkCertDraft(a, n);
    case Design::CertFixed: return checkCertFixed(a, n);
    case Design::Snap: return checkSnapshot(a, n);
    }
    return 0;
}

struct Result {
    double nsPerOp, p999, worst, missRate, retryRate, writerUs, writerCpuShare, totalNsPerOp;
    u64 rebuilds;
    double writerWorstUs;
};

Result run(Design d, int threads, int guards, Pattern pattern, int rate, int millis) {
    design = d;
    setup(guards);
    rebuilds.store(0);
    std::atomic<bool> go{false}, stop{false};
    std::vector<ReaderStats> stats(threads);
    std::vector<std::thread> readers;
    for (int t = 0; t < threads; ++t) {
        readers.emplace_back([&, t] {
            certificate = Certificate{};
            certMisses = snapRetries = snapFallbacks = snapStale = 0;
            auto& s = stats[t];
            s.samples.reserve(1 << 20);
            const int k = guards > 1 ? 1 + (t * 5) % (guards - 1) : 0;
            u64 i = u64(t) * 7919;
            u64 seed = 0x9e3779b97f4a7c15ull * (t + 1);
            while (!go.load(std::memory_order_acquire)) {}
            const auto t0 = std::chrono::steady_clock::now();
            while (!stop.load(std::memory_order_relaxed)) {
                for (int batch = 0; batch < 64; ++batch, ++i) {
                    u64 a = 0, n = 64;
                    switch (pattern) {
                    case Pattern::Local: a = gapAddress(k) + (i * 64) % 0x20000; break;
                    case Pattern::Spread: seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; a = gapAddress(int(seed % (guards + 1)) - 1) + (i * 64) % 0x20000; break;
                    case Pattern::Near: a = (i & 1) ? guardStart(k) - 64 : guardStart(k) + GuardBytes; break;
                    case Pattern::Cyclic5: a = gapAddress(int((i % 5) * std::max(1, guards / 5)) - 1) + (i * 64) % 0x20000; break;
                    case Pattern::Cyclic8: a = gapAddress(int((i % 8) * std::max(1, guards / 8)) - 1) + (i * 64) % 0x20000; break;
                    case Pattern::Overlap: a = guardStart(k) + 0x1000 + (i * 64) % 0x8000; break;
                    case Pattern::Retired: a = ChurnBase + (i % 64) * Stride + 0x1000; break;
                    }
                    const auto c0 = __rdtsc();
                    sink = check(a, n);
                    const auto c1 = __rdtsc();
                    const auto cycles = c1 - c0;
                    if ((i & 15u) == 0 && s.samples.size() < s.samples.capacity()) s.samples.push_back(std::uint32_t(std::min<u64>(cycles, 0xffffffffu)));
                    s.worst = std::max<u64>(s.worst, cycles);
                    ++s.ops;
                }
            }
            s.ns = u64(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count());
            s.misses = certMisses;
            s.retries = snapRetries;
            s.fallbacks = snapFallbacks;
            s.stale = snapStale;
        });
    }
    u64 writerNs = 0, registrations = 0, writerWorst = 0;
    std::thread writer;
    if (rate > 0) {
        writer = std::thread([&] {
            while (!go.load(std::memory_order_acquire)) {}
            const auto period = std::chrono::nanoseconds(1000000000ll / rate);
            auto next = std::chrono::steady_clock::now();
            u64 slot = 64;
            while (!stop.load(std::memory_order_relaxed)) {
                next += period;
                std::this_thread::sleep_until(next);
                timespec c0{}, c1{};
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
                const auto t0 = std::chrono::steady_clock::now();
                const u64 a = ChurnBase + (slot % 4096) * Stride;
                insertGuard(color, ColorBit, a, a + GuardBytes);
                if (slot >= 64 + 4) removeGuard(color, ChurnBase + ((slot - 4) % 4096) * Stride);
                ++slot;
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
                writerNs += u64((c1.tv_sec - c0.tv_sec) * 1000000000ll + (c1.tv_nsec - c0.tv_nsec));
                writerWorst = std::max<u64>(writerWorst, u64(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count()));
                ++registrations;
            }
        });
    }
    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
    stop.store(true);
    for (auto& r : readers) r.join();
    if (writer.joinable()) writer.join();
    u64 ops = 0, ns = 0, worst = 0, misses = 0, retries = 0, fallbacks = 0;
    std::vector<std::uint32_t> all;
    for (auto& s : stats) {
        ops += s.ops;
        ns += s.ns;
        worst = std::max(worst, s.worst);
        misses += d == Design::Snap ? s.fallbacks : s.misses;
        retries += s.retries;
        all.insert(all.end(), s.samples.begin(), s.samples.end());
    }
    std::size_t idx = all.size() * 999 / 1000;
    std::nth_element(all.begin(), all.begin() + idx, all.end());
    Result r{};
    r.nsPerOp = double(ns) / double(ops);
    r.p999 = all[idx] / tscPerNs;
    r.worst = worst / tscPerNs;
    r.missRate = double(misses) / double(ops);
    r.retryRate = double(retries) / double(ops);
    r.writerUs = registrations ? double(writerNs) / registrations / 1000.0 : 0;
    r.writerCpuShare = double(writerNs) / (double(millis) * 1e6);
    r.totalNsPerOp = (double(ns) + double(writerNs)) / double(ops);
    r.rebuilds = rebuilds.load();
    r.writerWorstUs = writerWorst / 1000.0;
    return r;
}

int main(int argc, char** argv) {
    calibrate();
    const int millis = argc > 1 ? std::atoi(argv[1]) : 300;
    const std::string only = argc > 2 ? argv[2] : "";
    std::printf("design,threads,guards,pattern,rate,ns_per_op,p999_ns,worst_ns,miss_or_fallback_rate,retry_rate,writer_us_per_reg,writer_cpu_share,total_ns_per_op,rebuilds,writer_worst_wall_us\n");
    const std::vector<Design> designs{Design::Null, Design::Main, Design::CertDraft, Design::CertFixed, Design::Snap};
    const std::vector<Pattern> patterns{Pattern::Local, Pattern::Spread, Pattern::Near, Pattern::Cyclic5, Pattern::Cyclic8, Pattern::Overlap, Pattern::Retired};
    for (int threads : {1, 2, 4}) {
        for (int guards : {4, 12, 64, 256}) {
            for (auto pattern : patterns) {
                for (int rate : {0, 10, 100, 1000, 5000}) {
                    if (only == "table" && (guards == 12 ? rate == 10 : (rate != 0 && rate != 1000))) continue;
                    if (only == "quick" && (!(threads == 4 && guards == 12) || (rate != 0 && rate != 1000))) continue;
                    for (auto d : designs) {
                        const auto r = run(d, threads, guards, pattern, rate, millis);
                        std::printf("%s,%d,%d,%s,%d,%.1f,%.0f,%.0f,%.4f,%.6f,%.2f,%.5f,%.1f,%llu,%.0f\n", name(d), threads, guards, name(pattern), rate, r.nsPerOp, r.p999, r.worst, r.missRate, r.retryRate, r.writerUs, r.writerCpuShare, r.totalNsPerOp, (unsigned long long)r.rebuilds, r.writerWorstUs);
                        std::fflush(stdout);
                    }
                }
            }
        }
    }
}
