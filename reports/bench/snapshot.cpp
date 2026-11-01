#include "prx/libSceAgcDriver/Graphics/include/SnapshotCopies.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
using AgcDriver::Graphics::Detail::SnapshotCopies;
template <bool Move> double run(std::size_t bytes, std::size_t items) {
    const std::shared_ptr<AgcDriver::Graphics::Buffer> buffer;
    std::vector<std::byte> source(bytes, std::byte{0x5a});
    const auto start = std::chrono::steady_clock::now();
    const int rounds = 20;
    for (int r = 0; r < rounds; ++r) {
        SnapshotCopies copies;
        for (std::size_t i = 0; i < items; ++i) {
            std::vector<std::byte> region(source);
            const SnapshotCopies::Key key{0x1000 + i * 0x10000, bytes, 0};
            if constexpr (Move) copies.Insert(key, buffer, std::move(region));
            else copies.Insert(key, buffer, std::span<const std::byte>(region));
        }
    }
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / (rounds * items);
}
int main() {
    for (const auto [bytes, items] : {std::pair<std::size_t, std::size_t>{0, 1024}, {256, 1024}, {4096, 1024}, {65536, 256}, {1 << 20, 16}}) {
        const double copy = run<false>(bytes, items), moved = run<true>(bytes, items);
        std::printf("bytes=%7zu items=%5zu  copy %9.1f ns/insert  move %9.1f ns/insert  saved %9.1f ns\n", bytes, items, copy, moved, copy - moved);
    }
}
