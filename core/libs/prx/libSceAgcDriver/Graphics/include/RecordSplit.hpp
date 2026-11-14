#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDSPLIT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDSPLIT_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

#include "prx/libSceAgcDriver/Execution/include/ProfileCounter.hpp"

namespace AgcDriver::Graphics::RecordSplit {

enum class Span : std::size_t {
    Assemble,
    Flush,
    Access,
    DrawSets,
    PassAccess,
    StartPass,
    PassSetup,
    PassState,
    Binds,
    Push,
    Commands,
    Tail,
    Leave,
    HazardCheck,
    HazardAdd,
    SetsGuard,
    SetsWalk,
    SetsSnapshots,
    SetsUploads,
    SetsWrite,
    SetsKeeps,
    SetsRecount,
    SetsAllocate,
    SetsBuild,
    SetsUpdate,
    Count
};

inline constexpr std::size_t RecordSteps = static_cast<std::size_t>(Span::HazardCheck);

enum class Event : std::size_t {
    Draws,
    PassesBegun,
    PassesContinued,
    HazardChecks,
    HazardCheckRanges,
    HazardCheckImages,
    HazardAdds,
    HazardRangesOffered,
    HazardRangesChanged,
    HazardImagesOffered,
    HazardImagesNew,
    ReadRanges,
    WriteRanges,
    StorageImages,
    BindingCalls,
    BindingEarlyOuts,
    SetsWritten,
    SnapshotFast,
    SnapshotReused,
    SnapshotNew,
    Uploads,
    UploadBytes,
    SetsSameAsPrevious,
    RangesSameAsPrevious,
    SetBinds,
    SetBindsRepeated,
    PushCalls,
    PushRepeated,
    VertexBinds,
    VertexRepeated,
    IndexBinds,
    IndexRepeated,
    Count
};

inline constexpr std::array<const char*, static_cast<std::size_t>(Span::Count)> SpanNames{"assemble", "flush checks", "access", "draw sets", "pass access", "start pass", "pass setup", "pass state", "binds", "push", "commands", "tail", "leave", "hazard check", "hazard add", "sets guard", "sets walk", "sets snapshots", "sets uploads", "sets write", "sets keeps", "sets recount", "sets allocate", "sets build", "sets update"};
inline constexpr std::array<const char*, static_cast<std::size_t>(Event::Count)> EventNames{"draws", "passes begun", "passes continued", "hazard checks", "hazard ranges at check", "hazard images at check", "hazard adds", "hazard ranges offered", "hazard ranges changed", "hazard images offered", "hazard images new", "read ranges", "write ranges", "storage images", "binding calls", "binding early outs", "sets written", "snapshot fast", "snapshot reused", "snapshot new", "uploads", "upload bytes", "sets same as previous", "ranges same as previous", "set binds", "set binds repeated", "push calls", "push repeated", "vertex binds", "vertex binds repeated", "index binds", "index binds repeated"};

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

inline void Charge(Span span, std::uint64_t nanoseconds) {
    Shared().ns[static_cast<std::size_t>(span)].Add(nanoseconds);
}

inline void Count(Event event, std::uint64_t value = 1) {
    if (!Enabled()) return;
    Shared().events[static_cast<std::size_t>(event)].Add(value);
}

class Lap {
public:
    using Clock = std::chrono::steady_clock;

    Lap() : on(Enabled()), mark(on ? Clock::now() : Clock::time_point{}) {}

    bool On() const { return on; }

    Clock::time_point Now() const { return on ? Clock::now() : Clock::time_point{}; }

    void To(Span span, std::uint64_t excluded = 0) {
        if (!on) return;
        const auto at = Clock::now();
        const auto elapsed = Nanoseconds(mark, at);
        Charge(span, elapsed > excluded ? elapsed - excluded : 0);
        mark = at;
    }

    std::uint64_t Since(Clock::time_point from, Span span) const {
        if (!on) return 0;
        const auto elapsed = Nanoseconds(from, Clock::now());
        Charge(span, elapsed);
        return elapsed;
    }

private:
    static std::uint64_t Nanoseconds(Clock::time_point from, Clock::time_point to) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count());
    }

    bool on;
    Clock::time_point mark;
};

class Fingerprint {
public:
    void Mix(std::uint64_t value) {
        hash ^= value;
        hash *= 0x100000001b3ull;
        hash ^= hash >> 29;
    }

    void Mix(const void* pointer) { Mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer))); }

    void MixBytes(std::span<const std::byte> bytes) {
        std::size_t index = 0;
        for (; index + sizeof(std::uint64_t) <= bytes.size(); index += sizeof(std::uint64_t)) {
            std::uint64_t word = 0;
            std::memcpy(&word, bytes.data() + index, sizeof(word));
            Mix(word);
        }
        std::uint64_t tail = 0;
        std::memcpy(&tail, bytes.data() + index, bytes.size() - index);
        Mix(tail ^ bytes.size());
    }

    std::uint64_t Value() const { return hash; }

private:
    std::uint64_t hash = 0xcbf29ce484222325ull;
};

template <typename Handle>
std::uint64_t HandleBits(Handle handle) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &handle, sizeof(handle) < sizeof(bits) ? sizeof(handle) : sizeof(bits));
    return bits;
}

struct Previous {
    std::uint64_t setStream;
    std::uint64_t setLayout;
    std::array<std::uint64_t, 2> sets;
    std::uint64_t pushStream;
    std::uint64_t push;
    std::uint64_t vertexStream;
    std::uint64_t vertex;
    std::uint64_t indexStream;
    std::uint64_t index;
    std::uint64_t bindings;
    std::uint64_t ranges;
};

inline Previous& Last() {
    thread_local Previous previous{};
    return previous;
}

inline std::uint64_t Stream(VkCommandBuffer commands, std::uint64_t serial) {
    Fingerprint stream;
    stream.Mix(HandleBits(commands));
    stream.Mix(serial);
    return stream.Value();
}

inline void NoteSetBind(std::uint64_t stream, VkPipelineLayout layout, std::size_t index, VkDescriptorSet set) {
    if (!Enabled() || index >= 2) return;
    auto& last = Last();
    if (last.setStream != stream || last.setLayout != HandleBits(layout)) {
        last.setStream = stream;
        last.setLayout = HandleBits(layout);
        last.sets = {};
    }
    Count(Event::SetBinds);
    if (last.sets[index] == HandleBits(set)) Count(Event::SetBindsRepeated);
    last.sets[index] = HandleBits(set);
}

inline void NotePush(std::uint64_t stream, VkPipelineLayout layout, VkShaderStageFlags stages, std::span<const std::byte> bytes) {
    if (!Enabled()) return;
    Fingerprint push;
    push.Mix(HandleBits(layout));
    push.Mix(stages);
    push.MixBytes(bytes);
    auto& last = Last();
    Count(Event::PushCalls);
    if (last.pushStream == stream && last.push == push.Value()) Count(Event::PushRepeated);
    last.pushStream = stream;
    last.push = push.Value();
}

inline void NoteVertexBind(std::uint64_t stream, std::span<const VkBuffer> handles, std::span<const VkDeviceSize> offsets) {
    if (!Enabled()) return;
    Fingerprint vertex;
    vertex.Mix(handles.size());
    for (const auto handle : handles) vertex.Mix(HandleBits(handle));
    for (const auto offset : offsets) vertex.Mix(offset);
    auto& last = Last();
    Count(Event::VertexBinds);
    if (last.vertexStream == stream && last.vertex == vertex.Value()) Count(Event::VertexRepeated);
    last.vertexStream = stream;
    last.vertex = vertex.Value();
}

inline void NoteIndexBind(std::uint64_t stream, VkBuffer handle, VkDeviceSize offset, VkIndexType type) {
    if (!Enabled()) return;
    Fingerprint index;
    index.Mix(HandleBits(handle));
    index.Mix(offset);
    index.Mix(static_cast<std::uint64_t>(type));
    auto& last = Last();
    Count(Event::IndexBinds);
    if (last.indexStream == stream && last.index == index.Value()) Count(Event::IndexRepeated);
    last.indexStream = stream;
    last.index = index.Value();
}

inline void NoteBindings(std::uint64_t bindings, std::uint64_t ranges) {
    if (!Enabled()) return;
    auto& last = Last();
    if (last.bindings == bindings) Count(Event::SetsSameAsPrevious);
    if (last.ranges == ranges) Count(Event::RangesSameAsPrevious);
    last.bindings = bindings;
    last.ranges = ranges;
}

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
    std::string line = "[record-split] (10 s, all threads) us per draw over " + std::to_string(draws) + " draws:";
    char part[160];
    double steps = 0.0;
    for (std::size_t index = 0; index < window.ns.size(); ++index) {
        const double perDraw = draws != 0 ? static_cast<double>(window.ns[index]) / 1000.0 / static_cast<double>(draws) : 0.0;
        if (index < RecordSteps) steps += perDraw;
        std::snprintf(part, sizeof(part), "%s %s %.3f", index == 0 || index == RecordSteps ? "" : ",", SpanNames[index], perDraw);
        line += part;
        if (index + 1 == RecordSteps) {
            std::snprintf(part, sizeof(part), " (steps %.3f); nested:", steps);
            line += part;
        }
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
