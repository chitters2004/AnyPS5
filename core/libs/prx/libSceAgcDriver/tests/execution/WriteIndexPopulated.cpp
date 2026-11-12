#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace {

using namespace AgcDriver::GuestMemory;

constexpr std::uint64_t KiB = 1024;
constexpr std::uint64_t MiB = 1024 * KiB;
constexpr std::uint64_t TextureBase = std::uint64_t{0x200000000};
constexpr std::uint64_t TargetBase = std::uint64_t{0x600000000};
constexpr std::uint64_t HeapBase = std::uint64_t{0x700000000};
constexpr std::uint64_t HeapBytes = 256 * MiB;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint64_t Ticks() {
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
    _mm_lfence();
    const auto ticks = __rdtsc();
    _mm_lfence();
    return ticks;
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

struct Clock {
    double ticksPerNanosecond = 1.0;
    std::uint64_t overhead = 0;
};

Clock Calibrate() {
    Clock clock;
    const auto wallStart = std::chrono::steady_clock::now();
    const auto tickStart = Ticks();
    while (std::chrono::steady_clock::now() - wallStart < std::chrono::milliseconds(50)) {
    }
    const auto wall = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - wallStart).count();
    clock.ticksPerNanosecond = static_cast<double>(Ticks() - tickStart) / wall;
    std::vector<std::uint64_t> empty(10000);
    for (auto& sample : empty) {
        const auto start = Ticks();
        sample = Ticks() - start;
    }
    std::sort(empty.begin(), empty.end());
    clock.overhead = empty[empty.size() / 2];
    return clock;
}

struct Summary {
    double median = 0;
    double p99 = 0;
    double mean = 0;
    std::size_t calls = 0;
};

class Samples {
public:
    explicit Samples(const Clock& clock) : clock(clock) {}
    template<typename TCall>
    void Time(TCall&& call) {
        const auto start = Ticks();
        call();
        const auto elapsed = Ticks() - start;
        ticks.push_back(elapsed > clock.overhead ? elapsed - clock.overhead : 0);
    }
    Summary Summarize() const {
        Summary summary;
        summary.calls = ticks.size();
        if (ticks.empty()) return summary;
        auto sorted = ticks;
        std::sort(sorted.begin(), sorted.end());
        const auto ns = [&](std::uint64_t value) { return static_cast<double>(value) / clock.ticksPerNanosecond; };
        summary.median = ns(sorted[sorted.size() / 2]);
        summary.p99 = ns(sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)]);
        double total = 0;
        for (const auto value : sorted) total += ns(value);
        summary.mean = total / static_cast<double>(sorted.size());
        return summary;
    }

private:
    const Clock& clock;
    std::vector<std::uint64_t> ticks;
};

void Print(const char* shape, const char* call, const Summary& summary) {
    std::printf("write-index populated: %-9s %-22s calls %7zu  median %8.1f ns  p99 %8.1f ns  mean %8.1f ns\n", shape, call, summary.calls, summary.median, summary.p99, summary.mean);
}

using Range = std::pair<std::uint64_t, std::uint64_t>;

struct Sizes {
    std::size_t textures = 600;
    std::size_t templates = 3100;
    std::size_t rangesPerTemplate = 4;
    std::size_t colorTargets = 24;
    std::size_t depthTargets = 8;
    std::size_t targetSets = 64;
    std::size_t captures = 3100;
    std::size_t dependenciesPerCapture = 16;
    std::size_t bumps = 4000;
    std::size_t holdsPasses = 2;
};

struct Subscribed {
    std::shared_ptr<WriteSubscription> subscription;
    WriteCertificate certificate;
};

struct Shape {
    const char* name;
    std::uint32_t domains;
    std::vector<Subscribed> entries;
};

constexpr std::uint32_t TemplateDomains = WriteDomainBit(WriteDomain::Proof) | WriteDomainBit(WriteDomain::Unwatch) | WriteDomainBit(WriteDomain::Mapping) | WriteDomainBit(WriteDomain::CertifiedProof);
constexpr std::uint32_t TargetDomains = TemplateDomains | WriteDomainBit(WriteDomain::Depth);
constexpr std::uint32_t CaptureDomains = WriteDomainBit(WriteDomain::Proof) | WriteDomainBit(WriteDomain::Unwatch) | WriteDomainBit(WriteDomain::Mapping);

struct Layout {
    std::vector<Range> textures;
    std::vector<Range> colors;
    std::vector<Range> depths;
    std::vector<Range> htiles;
};

Layout Lay(const Sizes& sizes) {
    Layout layout;
    constexpr std::array<std::uint64_t, 4> textureBytes{8 * MiB, 2 * MiB, 512 * KiB, 128 * KiB};
    auto cursor = TextureBase;
    for (std::size_t i = 0; i < sizes.textures; ++i) {
        const auto bytes = textureBytes[i % textureBytes.size()];
        layout.textures.emplace_back(cursor, cursor + bytes);
        cursor += bytes;
    }
    cursor = TargetBase;
    for (std::size_t i = 0; i < sizes.colorTargets; ++i, cursor += 8 * MiB) layout.colors.emplace_back(cursor, cursor + 8 * MiB);
    for (std::size_t i = 0; i < sizes.depthTargets; ++i, cursor += 8 * MiB) layout.depths.emplace_back(cursor, cursor + 8 * MiB);
    for (std::size_t i = 0; i < sizes.depthTargets; ++i, cursor += 256 * KiB) layout.htiles.emplace_back(cursor, cursor + 256 * KiB);
    return layout;
}

struct Populated {
    Summary templateSubscribe;
    Summary captureSubscribe;
    Shape templates{"templates", TemplateDomains, {}};
    Shape targets{"targets", TargetDomains, {}};
    Shape captures{"captures", CaptureDomains, {}};
};

Populated Populate(const Sizes& sizes, const Layout& layout, const Clock& clock, std::mt19937_64& random, bool print) {
    Populated populated;
    std::vector<Range> ranges;
    const auto subscribe = [&](Shape& shape, Samples& subscribeSamples) {
        Subscribed entry;
        subscribeSamples.Time([&] { entry.subscription = SubscribeWrites(ranges); });
        shape.entries.push_back(std::move(entry));
    };
    Samples templateSubscribe(clock);
    for (std::size_t i = 0; i < sizes.templates; ++i) {
        ranges.clear();
        for (std::size_t r = 0; r < sizes.rangesPerTemplate; ++r) ranges.push_back(layout.textures[random() % layout.textures.size()]);
        subscribe(populated.templates, templateSubscribe);
    }
    Samples targetSubscribe(clock);
    for (std::size_t i = 0; i < sizes.targetSets; ++i) {
        ranges.clear();
        const auto depth = random() % layout.depths.size();
        ranges.push_back(layout.colors[random() % layout.colors.size()]);
        ranges.push_back(layout.depths[depth]);
        ranges.push_back(layout.htiles[depth]);
        subscribe(populated.targets, targetSubscribe);
    }
    Samples captureSubscribe(clock);
    for (std::size_t i = 0; i < sizes.captures * sizes.dependenciesPerCapture; ++i) {
        ranges.clear();
        const auto bytes = 256 + (random() % 16) * 256;
        const auto begin = HeapBase + ((random() % (HeapBytes - bytes)) & ~std::uint64_t{255});
        ranges.emplace_back(begin, begin + bytes);
        subscribe(populated.captures, captureSubscribe);
    }
    populated.templateSubscribe = templateSubscribe.Summarize();
    populated.captureSubscribe = captureSubscribe.Summarize();
    if (print) {
        Print("templates", "subscribe", templateSubscribe.Summarize());
        Print("targets", "subscribe", targetSubscribe.Summarize());
        Print("captures", "subscribe (per dep)", captureSubscribe.Summarize());
    }
    return populated;
}

Summary Certify(Shape& shape, const Clock& clock) {
    Samples samples(clock);
    for (auto& entry : shape.entries) samples.Time([&] { entry.certificate = CertifySubscription(*entry.subscription, shape.domains); });
    return samples.Summarize();
}

Summary Holds(const Shape& shape, const Clock& clock, std::mt19937_64& random, std::size_t passes, std::size_t& held) {
    std::vector<std::size_t> order(shape.entries.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    Samples samples(clock);
    held = 0;
    for (std::size_t pass = 0; pass < passes; ++pass) {
        std::shuffle(order.begin(), order.end(), random);
        for (const auto i : order) {
            const auto& entry = shape.entries[i];
            bool holds = false;
            samples.Time([&] { holds = CertificateHolds(*entry.subscription, entry.certificate, shape.domains); });
            held += holds ? 1 : 0;
        }
    }
    return samples.Summarize();
}

struct BumpKind {
    const char* name;
    double perFrame;
};

struct BumpResult {
    Summary summary;
    double lookupsPerCall = 0;
    double notificationsPerCall = 0;
};

template<typename TBump>
BumpResult Bump(std::size_t calls, const Clock& clock, TBump&& bump) {
    BumpResult result;
    Samples samples(clock);
    SetWriteIndexCountingForTest(false);
    for (std::size_t i = 0; i < calls; ++i) samples.Time([&] { bump(i); });
    SetWriteIndexCountingForTest(true);
    result.summary = samples.Summarize();
    const auto counted = std::min<std::size_t>(calls, 32);
    const auto before = WriteIndexCounted();
    for (std::size_t i = 0; i < counted; ++i) bump(i);
    const auto after = WriteIndexCounted();
    result.lookupsPerCall = static_cast<double>(after.lookups - before.lookups) / static_cast<double>(counted);
    result.notificationsPerCall = static_cast<double>(after.bumps - before.bumps) / static_cast<double>(counted);
    return result;
}

void PrintBump(const char* name, const BumpResult& result) {
    Print("all", name, result.summary);
    std::printf("write-index populated: %-9s %-22s fan-out per call: %.1f block lookups, %.1f handle notifications (one per subscribed block the bump covers)\n", "all", name, result.lookupsPerCall, result.notificationsPerCall);
}

struct Measured {
    Summary templateHolds, targetHolds, captureHolds;
    BumpResult smallDepth, largeDepth, largePending, smallDriver;
};

Measured Measure(const Sizes& sizes, const Layout& layout, Populated& populated, const Clock& clock, std::mt19937_64& random, bool print) {
    Measured measured;
    const auto templateCertify = Certify(populated.templates, clock);
    const auto targetCertify = Certify(populated.targets, clock);
    const auto captureCertify = Certify(populated.captures, clock);
    std::size_t held = 0;
    measured.templateHolds = Holds(populated.templates, clock, random, sizes.holdsPasses, held);
    Require(held == populated.templates.entries.size() * sizes.holdsPasses, "a template certificate failed with no bump");
    measured.targetHolds = Holds(populated.targets, clock, random, sizes.holdsPasses, held);
    Require(held == populated.targets.entries.size() * sizes.holdsPasses, "a target-set certificate failed with no bump");
    measured.captureHolds = Holds(populated.captures, clock, random, sizes.holdsPasses, held);
    Require(held == populated.captures.entries.size() * sizes.holdsPasses, "a capture dependency certificate failed with no bump");
    if (print) {
        Print("templates", "certify", templateCertify);
        Print("targets", "certify", targetCertify);
        Print("captures", "certify (per dep)", captureCertify);
        Print("templates", "holds", measured.templateHolds);
        Print("targets", "holds", measured.targetHolds);
        Print("captures", "holds (per dep)", measured.captureHolds);
    }
    measured.smallDepth = Bump(sizes.bumps, clock, [&](std::size_t) {
        const auto address = HeapBase + ((random() % (HeapBytes - 64 * KiB)) & ~(64 * KiB - 1));
        BumpWrites(address, 64 * KiB, WriteBumpSource::Depth);
    });
    measured.largeDepth = Bump(sizes.bumps, clock, [&](std::size_t) {
        const auto& texture = layout.textures[(random() % (layout.textures.size() / 4)) * 4];
        BumpWrites(texture.first, texture.second - texture.first, WriteBumpSource::Depth);
    });
    measured.largePending = Bump(sizes.bumps, clock, [&](std::size_t) {
        const auto& texture = layout.textures[(random() % (layout.textures.size() / 4)) * 4];
        NoteGpuPending(texture.first, static_cast<std::size_t>(texture.second - texture.first));
    });
    measured.smallDriver = Bump(sizes.bumps, clock, [&](std::size_t) {
        const auto address = HeapBase + ((random() % (HeapBytes - 4 * KiB)) & ~(4 * KiB - 1));
        static_cast<void>(MarkWritten(address, 4 * KiB));
    });
    if (print) {
        PrintBump("bump 64 KiB (Depth)", measured.smallDepth);
        PrintBump("bump 8 MiB (Depth)", measured.largeDepth);
        PrintBump("NoteGpuPending 8 MiB", measured.largePending);
        PrintBump("MarkWritten 4 KiB", measured.smallDriver);
    }
    return measured;
}

void Contended(const Sizes& sizes, const Layout& layout, Populated& populated, const Clock& clock, std::size_t threads) {
    for (auto* shape : {&populated.templates, &populated.targets, &populated.captures}) static_cast<void>(Certify(*shape, clock));
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    std::vector<Summary> readerHolds(threads);
    for (std::size_t t = 0; t < threads; ++t) {
        readers.emplace_back([&, t] {
            std::mt19937_64 random(1000 + t);
            Samples samples(clock);
            const auto& shape = populated.captures;
            while (!stop.load(std::memory_order_relaxed)) {
                const auto& entry = shape.entries[random() % shape.entries.size()];
                samples.Time([&] { static_cast<void>(CertificateHolds(*entry.subscription, entry.certificate, shape.domains)); });
            }
            readerHolds[t] = samples.Summarize();
        });
    }
    std::mt19937_64 random(7);
    const auto pending = Bump(sizes.bumps, clock, [&](std::size_t) {
        const auto& texture = layout.textures[(random() % (layout.textures.size() / 4)) * 4];
        NoteGpuPending(texture.first, static_cast<std::size_t>(texture.second - texture.first));
    });
    const auto small = Bump(sizes.bumps, clock, [&](std::size_t) {
        const auto address = HeapBase + ((random() % (HeapBytes - 64 * KiB)) & ~(64 * KiB - 1));
        BumpWrites(address, 64 * KiB, WriteBumpSource::Depth);
    });
    stop.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) reader.join();
    std::printf("write-index populated: contended, %zu Holds threads on the captures beside one bump thread\n", threads);
    PrintBump("NoteGpuPending 8 MiB", pending);
    PrintBump("bump 64 KiB (Depth)", small);
    for (std::size_t t = 0; t < threads; ++t) Print("captures", ("holds, thread " + std::to_string(t)).c_str(), readerHolds[t]);
}

void PrintPerDraw(const Measured& measured, const Sizes& sizes, const Summary& templateSubscribe, const Summary& captureSubscribe) {
    constexpr double FramesPer10Seconds = 10000.0 / 126.1;
    constexpr double DrawsPerFrame = 3100.0;
    constexpr double LargePendingPerFrame = 394123.0 / FramesPer10Seconds;
    constexpr double SmallPendingPerFrame = (405190.0 - 394123.0) / FramesPer10Seconds;
    constexpr double DriverPerFrame = 75223.0 / FramesPer10Seconds;
    constexpr double CpuPerFrame = 71232.0 / FramesPer10Seconds;
    const double dependencies = static_cast<double>(sizes.dependenciesPerCapture);
    const auto perDraw = [&](double Summary::*statistic) {
        const double holds = measured.templateHolds.*statistic + measured.targetHolds.*statistic + dependencies * (measured.captureHolds.*statistic);
        const double bumps = (LargePendingPerFrame * (measured.largePending.summary.*statistic) + SmallPendingPerFrame * (measured.largeDepth.summary.*statistic) + DriverPerFrame * (measured.smallDriver.summary.*statistic) + CpuPerFrame * (measured.smallDepth.summary.*statistic)) / DrawsPerFrame;
        return std::pair{holds, bumps};
    };
    const double bumpsPerDraw = (LargePendingPerFrame + SmallPendingPerFrame + DriverPerFrame + CpuPerFrame) / DrawsPerFrame;
    for (const auto& [label, statistic] : {std::pair{"p99 (upper bound)", &Summary::p99}, std::pair{"mean", &Summary::mean}}) {
        const auto [holds, bumps] = perDraw(statistic);
        std::printf("write-index populated: per draw at %s: Holds %.1f ns (1 template + 1 target set + %.0f capture dependencies), bumps %.1f ns (%.2f per draw: GTA's d2 bump histogram per frame at 126.1 ms over 3,100 draws), total %.1f ns\n", label, holds, dependencies, bumps, bumpsPerDraw, holds + bumps);
    }
    std::printf("write-index populated: per draw if every draw subscribed anew: %.1f ns at p99, %.1f ns mean (1 template + %.0f capture dependencies)\n", templateSubscribe.p99 + dependencies * captureSubscribe.p99, templateSubscribe.mean + dependencies * captureSubscribe.mean, dependencies);
}

int Run(const Sizes& sizes, bool print, std::size_t threads) {
    const auto clock = Calibrate();
    if (print) std::printf("write-index populated: %.3f ticks per ns, %llu ticks timer overhead subtracted; %zu textures, %zu templates x %zu ranges, %zu target sets, %zu captures x %zu dependencies\n", clock.ticksPerNanosecond, static_cast<unsigned long long>(clock.overhead), sizes.textures, sizes.templates, sizes.rangesPerTemplate, sizes.targetSets, sizes.captures, sizes.dependenciesPerCapture);
    std::mt19937_64 random(42);
    const auto layout = Lay(sizes);
    const bool owners = std::getenv("APS5_BENCH_OWNERS") != nullptr;
    if (owners) {
        for (const auto* group : {&layout.textures, &layout.colors, &layout.depths, &layout.htiles}) {
            for (const auto& [begin, end] : *group) static_cast<void>(RegisterWriteOwner(begin, end - begin));
        }
        if (print) std::puts("write-index populated: textures and targets registered as write owners");
    }
    auto populated = Populate(sizes, layout, clock, random, print);
    const auto stats = WriteIndexCounted();
    Require(stats.subscriptions >= populated.templates.entries.size() + populated.targets.entries.size() + populated.captures.entries.size(), "the index does not count every live subscription");
    const auto measured = Measure(sizes, layout, populated, clock, random, print);
    Require(owners || measured.largePending.lookupsPerCall > 0.0, "a pending mark over a shared texture visited no subscribed block");
    Require(measured.smallDriver.lookupsPerCall > 0.0, "a driver store into the capture heap looked up no block");
    std::size_t held = 0;
    static_cast<void>(Holds(populated.templates, clock, random, 1, held));
    Require(held < populated.templates.entries.size(), "no template certificate failed after pending marks over its textures");
    if (print) PrintPerDraw(measured, sizes, populated.templateSubscribe, populated.captureSubscribe);
    if (threads != 0) Contended(sizes, layout, populated, clock, threads);
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "--smoke";
        SetWriteIndexCountingForTest(true);
        if (mode == "--bench") {
            const std::size_t threads = argc > 2 ? static_cast<std::size_t>(std::strtoul(argv[2], nullptr, 10)) : 3;
            return Run(Sizes{}, true, threads);
        }
        if (mode == "--smoke") {
            Sizes sizes;
            sizes.textures = 40;
            sizes.templates = 64;
            sizes.targetSets = 8;
            sizes.captures = 32;
            sizes.bumps = 200;
            sizes.holdsPasses = 1;
            Run(sizes, false, 0);
            std::puts("write-index populated: certificates hold with no bump, and pending marks over shared textures break templates");
            return 0;
        }
        throw std::runtime_error("unknown mode " + mode);
    } catch (const std::exception& error) {
        std::cerr << "write-index populated: " << error.what() << '\n';
        return 1;
    }
}
