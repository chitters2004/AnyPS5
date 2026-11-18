#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ALLOCATIONCENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ALLOCATIONCENSUS_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace AgcDriver::AllocationCensus {

enum class Source : std::uint8_t { New, Malloc, Calloc, Realloc, Count };

inline constexpr std::array<const char*, static_cast<std::size_t>(Source::Count)> SourceNames{"new", "malloc", "calloc", "realloc"};

struct Site {
    std::uintptr_t caller = 0;
    Source source = Source::New;
    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
};

struct Window {
    std::uint64_t draws = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(Source::Count)> counts{};
    std::array<std::uint64_t, static_cast<std::size_t>(Source::Count)> bytes{};
    std::uint64_t untracked = 0;
    std::size_t listed = 0;
};

template <std::size_t Slots, std::size_t Top>
class Table {
    static_assert((Slots & (Slots - 1)) == 0, "the slot count is a power of two");

public:
    static constexpr std::size_t Probes = 32;

    void Note(Source source, std::uintptr_t caller, std::size_t bytes) noexcept {
        const auto kind = static_cast<std::size_t>(source);
        totals[kind].fetch_add(1, std::memory_order_relaxed);
        totalBytes[kind].fetch_add(bytes, std::memory_order_relaxed);
        const auto key = (static_cast<std::uint64_t>(caller) << 2u) | kind;
        auto index = static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> 40u) & (Slots - 1);
        for (std::size_t probe = 0; probe < Probes; ++probe, index = (index + 1) & (Slots - 1)) {
            auto& slot = slots[index];
            auto held = slot.key.load(std::memory_order_acquire);
            if (held == 0 && slot.key.compare_exchange_strong(held, key, std::memory_order_acq_rel)) held = key;
            if (held != key) continue;
            slot.count.fetch_add(1, std::memory_order_relaxed);
            slot.bytes.fetch_add(bytes, std::memory_order_relaxed);
            return;
        }
        untracked.fetch_add(1, std::memory_order_relaxed);
    }

    void NoteDraw() noexcept {
        draws.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t Total(Source source) const noexcept {
        return totals[static_cast<std::size_t>(source)].load(std::memory_order_relaxed);
    }

    std::uint64_t Draws() const noexcept {
        return draws.load(std::memory_order_relaxed);
    }

    Window Take(std::array<Site, Top>& top) noexcept {
        Window window;
        const auto drawsNow = draws.load(std::memory_order_relaxed);
        window.draws = drawsNow - reported.draws;
        reported.draws = drawsNow;
        for (std::size_t kind = 0; kind < totals.size(); ++kind) {
            const auto count = totals[kind].load(std::memory_order_relaxed);
            const auto bytes = totalBytes[kind].load(std::memory_order_relaxed);
            window.counts[kind] = count - reported.counts[kind];
            window.bytes[kind] = bytes - reported.bytes[kind];
            reported.counts[kind] = count;
            reported.bytes[kind] = bytes;
        }
        const auto untrackedNow = untracked.load(std::memory_order_relaxed);
        window.untracked = untrackedNow - reported.untracked;
        reported.untracked = untrackedNow;
        for (std::size_t index = 0; index < Slots; ++index) {
            auto& slot = slots[index];
            const auto key = slot.key.load(std::memory_order_acquire);
            if (key == 0) continue;
            const auto count = slot.count.load(std::memory_order_relaxed);
            const auto bytes = slot.bytes.load(std::memory_order_relaxed);
            const Site site{static_cast<std::uintptr_t>(key >> 2u), static_cast<Source>(key & 3u), count - slot.reportedCount, bytes - slot.reportedBytes};
            slot.reportedCount = count;
            slot.reportedBytes = bytes;
            if (site.count == 0) continue;
            std::size_t at = window.listed;
            if (at == Top) {
                if (site.count <= top[Top - 1].count) continue;
                at = Top - 1;
            } else {
                ++window.listed;
            }
            while (at > 0 && top[at - 1].count < site.count) {
                top[at] = top[at - 1];
                --at;
            }
            top[at] = site;
        }
        return window;
    }

private:
    struct Slot {
        std::atomic<std::uint64_t> key{0};
        std::atomic<std::uint64_t> count{0};
        std::atomic<std::uint64_t> bytes{0};
        std::uint64_t reportedCount = 0;
        std::uint64_t reportedBytes = 0;
    };
    std::array<Slot, Slots> slots{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Source::Count)> totals{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Source::Count)> totalBytes{};
    std::atomic<std::uint64_t> untracked{0};
    std::atomic<std::uint64_t> draws{0};
    Window reported{};
};

inline int FormatWindow(char* line, std::size_t room, double seconds, const Window& window, const Site* top, std::uintptr_t moduleBase, std::uintptr_t preferredBase) {
    const auto perDraw = [&](std::uint64_t count) { return window.draws != 0 ? static_cast<double>(count) / static_cast<double>(window.draws) : 0.0; };
    std::uint64_t all = 0;
    for (const auto count : window.counts) all += count;
    int n = std::snprintf(line, room, "[alloc-census] %.0f s: %llu draws, %llu allocations (%.2f/draw):", seconds, static_cast<unsigned long long>(window.draws), static_cast<unsigned long long>(all), perDraw(all));
    const auto fits = [&] { return n > 0 && static_cast<std::size_t>(n) < room; };
    for (std::size_t kind = 0; kind < window.counts.size() && fits(); ++kind) {
        n += std::snprintf(line + n, room - static_cast<std::size_t>(n), " %s %llu (%.2f/draw, %llu B)", SourceNames[kind], static_cast<unsigned long long>(window.counts[kind]), perDraw(window.counts[kind]), static_cast<unsigned long long>(window.bytes[kind]));
    }
    if (fits()) n += std::snprintf(line + n, room - static_cast<std::size_t>(n), "; %llu at sites past the table; top sites (address at the preferred base, source, count, per draw, avg bytes):", static_cast<unsigned long long>(window.untracked));
    for (std::size_t i = 0; i < window.listed && fits(); ++i) {
        const auto& site = top[i];
        const auto address = site.caller - moduleBase + preferredBase;
        n += std::snprintf(line + n, room - static_cast<std::size_t>(n), " 0x%llx %s %llu %.3f %llu;", static_cast<unsigned long long>(address), SourceNames[static_cast<std::size_t>(site.source)], static_cast<unsigned long long>(site.count), perDraw(site.count), static_cast<unsigned long long>(site.count != 0 ? site.bytes / site.count : 0));
    }
    return n;
}

inline std::uint64_t PreferredImageBase(const unsigned char* file, std::size_t bytes) noexcept {
    const auto read = [&](std::size_t at, std::size_t width) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < width; ++i) value |= static_cast<std::uint64_t>(file[at + i]) << (8u * i);
        return value;
    };
    if (bytes < 0x40 || file[0] != 'M' || file[1] != 'Z') return 0;
    const auto nt = static_cast<std::size_t>(read(0x3c, 4));
    if (nt > bytes || bytes - nt < 0x18 + 0x20 || read(nt, 4) != 0x4550u) return 0;
    const auto optional = nt + 0x18;
    const auto magic = read(optional, 2);
    if (magic == 0x20bu) return read(optional + 0x18, 8);
    if (magic == 0x10bu) return read(optional + 0x1c, 4);
    return 0;
}

bool Enabled() noexcept;
void Note(Source source, const void* caller, std::size_t bytes) noexcept;
void NoteDraw() noexcept;
std::uint64_t Total(Source source) noexcept;

}

#endif
