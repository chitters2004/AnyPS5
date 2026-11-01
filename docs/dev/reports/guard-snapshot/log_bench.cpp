#define main guard_main
#include "guard_bench.cpp"
#undef main

struct LogEntry { u64 finish, a, b; };
std::mutex logMutex;
std::atomic<u64> logSeq{0}, logCount{0};
std::array<std::atomic<u64>, 64 * 3> logData{};

void logRange(u64 finish, u64 a, u64 b) {
    std::lock_guard lock(logMutex);
    const auto s = logSeq.load(std::memory_order_relaxed);
    logSeq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    const auto n = logCount.load(std::memory_order_relaxed);
    const auto slot = (n % 64) * 3;
    logData[slot].store(finish, std::memory_order_relaxed);
    logData[slot + 1].store(a, std::memory_order_relaxed);
    logData[slot + 2].store(b, std::memory_order_relaxed);
    logCount.store(n + 1, std::memory_order_relaxed);
    logSeq.store(s + 2, std::memory_order_release);
}

bool unchangedSince(u64 proved, u64 a, u64 e, u64 g) {
    if (proved == g) return true;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto s = logSeq.load(std::memory_order_acquire);
        if (s & 1u) continue;
        const auto n = logCount.load(std::memory_order_relaxed);
        const auto first = n > 64 ? n - 64 : 0;
        bool covered = first == 0, clear = true;
        for (auto i = n; i > first; --i) {
            const auto slot = ((i - 1) % 64) * 3;
            const auto f = logData[slot].load(std::memory_order_relaxed);
            if (f <= proved) { covered = true; break; }
            if (f > g) continue;
            if (logData[slot + 1].load(std::memory_order_relaxed) < e && a < logData[slot + 2].load(std::memory_order_relaxed)) clear = false;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (logSeq.load(std::memory_order_relaxed) == s) return covered && clear;
    }
    return false;
}

struct DrawProof { std::array<Range, 3> ranges; u64 growth = 0; };

int main() {
    calibrate();
    design = Design::Snap;
    std::printf("guards,threads,rate,variant,ns_per_draw,full_checks_per_draw\n");
    for (int guards : {12}) {
        for (int threads : {1, 4}) {
            for (int rate : {0, 1000, 10000, 50000}) {
                for (int variant = 0; variant < 3; ++variant) {
                    setup(guards);
                    std::atomic<bool> go{false}, stop{false};
                    std::vector<double> ns(threads), full(threads);
                    std::vector<std::thread> pool;
                    for (int t = 0; t < threads; ++t) {
                        pool.emplace_back([&, t] {
                            std::vector<DrawProof> objects(1000);
                            for (int d = 0; d < 1000; ++d) for (int k = 0; k < 3; ++k) objects[d].ranges[k] = {gapAddress(int((d * 3 + k + t * 7) % (guards + 1)) - 1) + u64((d * 3 + k) % 1024) * 64, 4096u};
                            while (!go.load(std::memory_order_acquire)) {}
                            u64 draws = 0, checks = 0;
                            const auto t0 = std::chrono::steady_clock::now();
                            while (!stop.load(std::memory_order_relaxed)) {
                                for (auto& object : objects) {
                                    ++draws;
                                    const auto g = growth.load(std::memory_order_acquire);
                                    if (variant == 0) {
                                        for (const auto& r : object.ranges) sink = checkSnapshot(r.a, r.b);
                                        checks += 3;
                                        continue;
                                    }
                                    if (object.growth == g) continue;
                                    if (variant == 2 && object.growth != 0 && std::all_of(object.ranges.begin(), object.ranges.end(), [&](const Range& r) { return unchangedSince(object.growth, r.a, r.a + r.b, g); })) {
                                        object.growth = g;
                                        continue;
                                    }
                                    bool clear = true;
                                    for (const auto& r : object.ranges) clear &= checkSnapshot(r.a, r.b) == 0;
                                    checks += 3;
                                    object.growth = clear ? g : 0;
                                }
                            }
                            ns[t] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / double(draws);
                            full[t] = double(checks) / 3.0 / double(draws);
                        });
                    }
                    std::thread writer;
                    if (rate > 0) {
                        writer = std::thread([&] {
                            while (!go.load(std::memory_order_acquire)) {}
                            const auto period = std::chrono::nanoseconds(1000000000ll / rate);
                            auto next = std::chrono::steady_clock::now();
                            u64 slot = 64;
                            while (!stop.load(std::memory_order_relaxed)) {
                                next += period;
                                while (std::chrono::steady_clock::now() < next) {}
                                const u64 a = ChurnBase + (slot % 4096) * Stride;
                                insertGuard(color, ColorBit, a, a + GuardBytes);
                                logRange(growth.load(), a, a + GuardBytes);
                                if (slot >= 68) removeGuard(color, ChurnBase + ((slot - 4) % 4096) * Stride);
                                ++slot;
                            }
                        });
                    }
                    go.store(true, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    stop.store(true);
                    for (auto& p : pool) p.join();
                    if (writer.joinable()) writer.join();
                    double avg = 0, fullAvg = 0;
                    for (int t = 0; t < threads; ++t) { avg += ns[t]; fullAvg += full[t]; }
                    static const char* names[] = {"snapshot-3-lookups", "proof-full-recheck", "proof-log-revalidate"};
                    std::printf("%d,%d,%d,%s,%.1f,%.4f\n", guards, threads, rate, names[variant], avg / threads, fullAvg / threads);
                }
            }
        }
    }
}
