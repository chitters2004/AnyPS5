#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_EFFECTREPEATSCREEN_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_EFFECTREPEATSCREEN_HPP

#include <cstdint>
#include <span>
#include <utility>

namespace AgcDriver::Graphics {

inline std::uint64_t EffectReadPrint(std::span<const std::pair<std::uint64_t, std::uint64_t>> reads, const void* list, std::uint32_t kind) {
    std::uint64_t print = 0xcbf29ce484222325ull ^ (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(list)) * 0x9e3779b97f4a7c15ull) ^ (static_cast<std::uint64_t>(kind) << 56u);
    for (const auto& [begin, end] : reads) {
        print = (print ^ begin) * 0x100000001b3ull;
        print = (print ^ end) * 0x100000001b3ull;
    }
    return (print ^ reads.size()) * 0x100000001b3ull;
}

enum class EffectRepeat {
    First,
    Unchanged,
    Changed,
};

class EffectRepeatScreen {
public:
    EffectRepeat Note(const void* recorder, std::uint64_t batch, std::uint64_t print) {
        const bool sameBatch = held && recorder == heldRecorder && batch == heldBatch;
        const bool unchanged = sameBatch && print == heldPrint;
        held = true;
        heldRecorder = recorder;
        heldBatch = batch;
        heldPrint = print;
        return !sameBatch ? EffectRepeat::First : unchanged ? EffectRepeat::Unchanged : EffectRepeat::Changed;
    }

private:
    bool held = false;
    const void* heldRecorder = nullptr;
    std::uint64_t heldBatch = 0;
    std::uint64_t heldPrint = 0;
};

}

#endif
