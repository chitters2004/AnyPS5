#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_KEYHASH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_KEYHASH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace AgcDriver {

inline std::uint64_t HashKeyWords(std::span<const std::uint64_t> words, std::uint64_t seed) {
    const auto mix = [](std::uint64_t hash, std::uint64_t word) {
        hash = (hash ^ word) * 0xff51afd7ed558ccdull;
        return hash ^ (hash >> 32u);
    };
    std::array<std::uint64_t, 4> lanes{seed ^ words.size(), 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull};
    std::size_t i = 0;
    for (; i + lanes.size() <= words.size(); i += lanes.size()) {
        for (std::size_t lane = 0; lane < lanes.size(); ++lane) lanes[lane] = mix(lanes[lane], words[i + lane]);
    }
    for (; i < words.size(); ++i) lanes[0] = mix(lanes[0], words[i]);
    auto hash = lanes[0];
    for (std::size_t lane = 1; lane < lanes.size(); ++lane) hash = mix(hash, lanes[lane]);
    return hash;
}

}

#endif
