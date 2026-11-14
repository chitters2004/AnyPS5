#include <array>
#include <cstddef>
#include <cstdint>

namespace {
thread_local std::array<std::uint64_t, 32768> block{};
}

extern thread_local std::uint64_t TlsModelLast;
thread_local std::uint64_t TlsModelLast = 0;

extern "C" std::size_t TlsModelBytes() {
    return sizeof(block);
}

extern "C" void TlsModelStore(std::uint64_t seed) {
    for (std::size_t i = 0; i < block.size(); ++i) block[i] = seed * 0x9E3779B97F4A7C15ull + i;
    TlsModelLast = seed;
}

extern "C" int TlsModelCheck(std::uint64_t seed) {
    if (TlsModelLast != seed) return 0;
    for (std::size_t i = 0; i < block.size(); ++i) {
        if (block[i] != seed * 0x9E3779B97F4A7C15ull + i) return 0;
    }
    return 1;
}
