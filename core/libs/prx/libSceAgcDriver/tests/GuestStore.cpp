#include "prx/libSceAgcDriver/Execution/include/GuestStore.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

namespace Store = AgcDriver::GuestStore;

void check(bool condition, const std::string& message) {
    if (condition) return;
    std::fprintf(stderr, "guest store test failed: %s\n", message.c_str());
    std::exit(1);
}

void testLabels() {
    alignas(16) std::array<std::byte, 32> guest{};
    const std::array<std::byte, 8> value{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    for (const std::size_t bytes : {std::size_t{4}, std::size_t{8}}) {
        for (std::size_t offset = 0; offset < 16; offset += bytes) {
            guest.fill(std::byte{0xee});
            Store::StoreLabel(guest.data() + offset, std::span(value).first(bytes));
            for (std::size_t index = 0; index < guest.size(); ++index) {
                const bool inside = index >= offset && index < offset + bytes;
                check(guest[index] == (inside ? value[index - offset] : std::byte{0xee}), "StoreLabel wrote the wrong byte at offset " + std::to_string(offset) + " for " + std::to_string(bytes) + " bytes");
            }
        }
    }
    for (const auto& [offset, bytes] : {std::pair{std::size_t{2}, std::size_t{4}}, std::pair{std::size_t{4}, std::size_t{8}}, std::pair{std::size_t{0}, std::size_t{2}}, std::pair{std::size_t{0}, std::size_t{16}}}) {
        check(!Store::IsLabel(guest.data() + offset, bytes), "a misaligned or odd-sized store counted as a label");
        bool refused = false;
        try {
            Store::StoreLabel(guest.data() + offset, std::span(guest).subspan(16, bytes));
        } catch (const std::logic_error&) {
            refused = true;
        }
        check(refused, "StoreLabel accepted " + std::to_string(bytes) + " bytes at offset " + std::to_string(offset));
    }
}

void testConcurrent() {
    alignas(8) std::uint64_t label = 0;
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            const auto word = Store::Load<std::uint64_t>(&label);
            check((word >> 32u) == (word & 0xffffffffu), "a label was read torn");
        }
    });
    for (std::uint64_t round = 0; round < 200000; ++round) {
        const auto value = round << 32u | round;
        Store::StoreLabel(&label, std::as_bytes(std::span(&value, 1)));
    }
    done.store(true, std::memory_order_release);
    reader.join();
}

}

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    testLabels();
    if (mode == "concurrent") testConcurrent();
    std::puts("guest store tests passed");
    return 0;
}
