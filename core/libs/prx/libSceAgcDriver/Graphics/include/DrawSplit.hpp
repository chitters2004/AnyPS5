#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWSPLIT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWSPLIT_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "prx/libSceAgcDriver/Execution/include/ProfileCounter.hpp"

namespace AgcDriver::Graphics::DrawSplit {

enum class Span : std::size_t { DepthView, GpuMutexWait, SurfacesLockWait, MaskedCopy, PipelineLockWait, PipelineLockHold, RememberShape, Framebuffer, Count };
enum class Event : std::size_t { DepthViews, DepthCollects, MaskedCopies, PipelineTouches, ShapeRemembers, Framebuffers, Count };

inline constexpr std::array<const char*, static_cast<std::size_t>(Span::Count)> SpanNames{"depth view", "gpu mutex wait", "surfaces lock wait", "masked state copy", "pipeline store lock wait", "pipeline store lock hold", "remember shape", "acquire framebuffer"};
inline constexpr std::array<const char*, static_cast<std::size_t>(Event::Count)> EventNames{"depth views", "depth collects", "masked copies", "pipeline touches", "shape remembers", "framebuffer acquires"};

inline bool Enabled() {
    static const bool enabled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return enabled;
}

struct Counters {
    std::array<AgcDriver::ProfileCounter, static_cast<std::size_t>(Span::Count)> ns{};
    std::array<AgcDriver::ProfileCounter, static_cast<std::size_t>(Event::Count)> events{};
};

inline Counters& Shared() {
    static Counters counters;
    return counters;
}

inline std::chrono::steady_clock::time_point Now() {
    return Enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
}

inline void Add(Span span, std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
    if (!Enabled()) return;
    Shared().ns[static_cast<std::size_t>(span)].Add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count()));
}

inline void Count(Event event, std::uint64_t value = 1) {
    if (!Enabled()) return;
    Shared().events[static_cast<std::size_t>(event)].Add(value);
}

class Scope {
public:
    explicit Scope(Span span) : span(span), start(Now()) {}
    Scope(Span span, std::chrono::steady_clock::time_point start) : span(span), start(start) {}
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() {
        if (Enabled()) Add(span, start, std::chrono::steady_clock::now());
    }

private:
    Span span;
    std::chrono::steady_clock::time_point start;
};

struct Window {
    std::array<std::uint64_t, static_cast<std::size_t>(Span::Count)> ns{};
    std::array<std::uint64_t, static_cast<std::size_t>(Event::Count)> events{};
};

inline Window Take() {
    Window window;
    auto& counters = Shared();
    for (std::size_t index = 0; index < window.ns.size(); ++index) window.ns[index] = counters.ns[index].Take();
    for (std::size_t index = 0; index < window.events.size(); ++index) window.events[index] = counters.events[index].Take();
    return window;
}

inline std::string Describe(const Window& window, std::uint64_t draws) {
    std::string line = "[draw-split] (10 s, all threads) per draw over " + std::to_string(draws) + " draws:";
    char part[160];
    for (std::size_t index = 0; index < window.ns.size(); ++index) {
        const double perDraw = draws != 0 ? static_cast<double>(window.ns[index]) / 1000.0 / static_cast<double>(draws) : 0.0;
        std::snprintf(part, sizeof(part), "%s %s %.3f us", index == 0 ? "" : ",", SpanNames[index], perDraw);
        line += part;
    }
    line += "; events:";
    for (std::size_t index = 0; index < window.events.size(); ++index) {
        std::snprintf(part, sizeof(part), "%s %s %llu", index == 0 ? "" : ",", EventNames[index], static_cast<unsigned long long>(window.events[index]));
        line += part;
    }
    return line;
}

}

#endif
