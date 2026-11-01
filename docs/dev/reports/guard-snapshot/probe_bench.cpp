#define main guard_main
#include "guard_bench.cpp"
#undef main

struct DrawRanges { std::array<Range, 3> ranges; u64 growth = 0; };
struct CacheEntry { u64 a = 0, n = 0, growth = 0; };
thread_local std::array<CacheEntry, 512> rangeCache;

unsigned cachedCheck(u64 g, u64 a, u64 n) {
    auto& e = rangeCache[((a >> 6) ^ (a >> 20) ^ n) & 511u];
    if (e.growth == g && e.a == a && e.n == n) return 0;
    const auto r = checkSnapshot(a, n);
    if (r == 0) e = {a, n, g};
    return r;
}

int main() {
    calibrate();
    design = Design::Snap;
    std::printf("guards,threads,draws,variant,ns_per_draw\n");
    for (int guards : {12, 256}) {
        setup(guards);
        for (int threads : {1, 4}) {
            for (int draws : {100, 1000, 5000}) {
                for (int variant = 0; variant < 3; ++variant) {
                    std::atomic<bool> go{false};
                    std::vector<double> ns(threads);
                    std::vector<std::thread> pool;
                    for (int t = 0; t < threads; ++t) {
                        pool.emplace_back([&, t] {
                            std::vector<DrawRanges> objects(draws);
                            for (int d = 0; d < draws; ++d) {
                                for (int k = 0; k < 3; ++k) {
                                    const u64 a = gapAddress(int((d * 3 + k + t * 7) % (guards + 1)) - 1) + u64((d * 3 + k) % 1024) * 64;
                                    objects[d].ranges[k] = {a, 4096u + u64(k) * 256u};
                                }
                            }
                            rangeCache = {};
                            while (!go.load(std::memory_order_acquire)) {}
                            const int passes = std::max(20, 400000 / draws);
                            const auto t0 = std::chrono::steady_clock::now();
                            for (int pass = 0; pass < passes; ++pass) {
                                for (auto& object : objects) {
                                    const auto g = growth.load(std::memory_order_acquire);
                                    if (variant == 0) {
                                        for (const auto& r : object.ranges) sink = checkSnapshot(r.a, r.b);
                                    } else if (variant == 1) {
                                        if (object.growth == g) continue;
                                        bool clear = true;
                                        for (const auto& r : object.ranges) clear &= checkSnapshot(r.a, r.b) == 0;
                                        if (clear) object.growth = g;
                                    } else {
                                        for (const auto& r : object.ranges) sink = cachedCheck(g, r.a, r.b);
                                    }
                                }
                            }
                            ns[t] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (double(passes) * draws);
                        });
                    }
                    go.store(true, std::memory_order_release);
                    for (auto& p : pool) p.join();
                    double avg = 0;
                    for (auto v : ns) avg += v;
                    static const char* names[] = {"snapshot-3-lookups", "owner-proof", "thread-local-range-cache"};
                    std::printf("%d,%d,%d,%s,%.1f\n", guards, threads, draws, names[variant], avg / threads);
                }
            }
        }
    }
}
