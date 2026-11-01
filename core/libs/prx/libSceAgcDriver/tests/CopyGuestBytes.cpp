#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <span>

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
}

namespace {

std::size_t flushes = 0;
std::uint64_t flushedAddress = 0;
std::size_t flushedBytes = 0;
std::size_t guards = 0;

void countFlush(std::uint64_t address, std::size_t bytes) {
    ++flushes;
    flushedAddress = address;
    flushedBytes = bytes;
}

void countGuard(std::uint64_t, std::size_t) {
    ++guards;
}

void readFlushes() {
    using namespace AgcDriver::GuestMemory;
    alignas(8) static std::array<unsigned char, 64> source{};
    for (std::size_t index = 0; index < source.size(); ++index) source[index] = static_cast<unsigned char>(index * 13 + 5);
    const auto address = reinterpret_cast<std::uint64_t>(source.data()) + 8;
    SetFlushHook(&countFlush);
    SetReadGuard(&countGuard);
    std::array<std::byte, 24> destination{};
    Read(address, destination, 8);
    Require(flushes == 1 && flushedAddress == address && flushedBytes == destination.size(), "a read flushes the GPU writes over its range once");
    Require(guards == 1, "a read is guarded");
    destination.fill(std::byte{0});
    ReadUnsynced(address, destination, 8);
    Require(flushes == 1, "an unsynced read does not flush the GPU writes again");
    Require(guards == 2, "an unsynced read is still guarded");
    Require(std::memcmp(destination.data(), source.data() + 8, destination.size()) == 0, "an unsynced read copies the bytes");
    ReadUnsynced(address, std::span<std::byte>{}, 8);
    Require(flushes == 1 && guards == 2, "an empty unsynced read does nothing");
    bool misaligned = false;
    try {
        ReadUnsynced(address + 1, destination, 8);
    } catch (const std::exception&) {
        misaligned = true;
    }
    Require(misaligned, "an unsynced read checks the alignment");
    SetReadGuard(nullptr);
    SetFlushHook(nullptr);
}

}

int main() {
    std::array<unsigned char, 96> source{};
    for (std::size_t index = 0; index < source.size(); ++index) source[index] = static_cast<unsigned char>(index * 37 + 11);
    for (std::size_t offset = 0; offset < 8; ++offset) {
        for (std::size_t bytes : {0, 1, 7, 8, 9, 15, 16, 17, 64, 80}) {
            std::array<unsigned char, 96> destination{};
            destination.fill(0xa5);
            AgcDriver::GuestMemory::CopyGuestBytes(destination.data() + 1, source.data() + offset, bytes);
            Require(destination[0] == 0xa5, "the byte before the destination is kept");
            Require(std::memcmp(destination.data() + 1, source.data() + offset, bytes) == 0, "the bytes are copied from every source offset");
            Require(destination[1 + bytes] == 0xa5, "the byte after the destination is kept");
        }
    }
    readFlushes();
    std::puts("copy guest bytes: ok");
}
