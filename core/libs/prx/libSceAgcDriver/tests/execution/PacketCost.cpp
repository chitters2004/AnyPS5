#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr auto CompletionDeadline = std::chrono::seconds(120);

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t Low(const void* p) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p)); }
std::uint32_t High(const void* p) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p) >> 32u); }

struct Mix {
    const char* name;
    std::uint32_t opcode;
    std::uint32_t firstRegister;
    std::uint32_t span;
    std::uint32_t values;
};

constexpr std::array<Mix, 4> Mixes{{
    {"SET_CONTEXT_REG x1", 0x69, 0x100, 0x40, 1},
    {"SET_CONTEXT_REG x4", 0x69, 0x100, 0x40, 4},
    {"SET_SH_REG x1", 0x76, 0x0c, 0x20, 1},
    {"SET_UCONFIG_REG x1", 0x79, 0x380, 0x10, 1},
}};

std::vector<std::uint32_t> RegisterStream(const Mix& mix, std::size_t packets) {
    std::vector<std::uint32_t> words;
    words.reserve(packets * (2 + mix.values));
    for (std::size_t index = 0; index < packets; ++index) {
        words.push_back(0xc0000000u | (mix.values << 16u) | (mix.opcode << 8u));
        words.push_back(mix.firstRegister + static_cast<std::uint32_t>(index * mix.values) % mix.span);
        for (std::uint32_t value = 0; value < mix.values; ++value) words.push_back(static_cast<std::uint32_t>(index * 2654435761u) + value);
    }
    return words;
}

void Release(std::vector<std::uint32_t>& words, const void* label, std::uint32_t value) {
    words.insert(words.end(), {0xc0064900u, 0x514u, 2u << 29u, Low(label), High(label), value, 0u, 0u});
}

void Await(const std::uint32_t* label, std::uint32_t value) {
    const auto deadline = std::chrono::steady_clock::now() + CompletionDeadline;
    while (*static_cast<const volatile std::uint32_t*>(label) != value) {
        Require(std::chrono::steady_clock::now() < deadline, "the submission's release never landed");
        std::this_thread::yield();
    }
}

double TableNanoseconds(const std::vector<std::uint32_t>& words, std::size_t packets) {
    AgcDriver::QueueState queue;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t cursor = 0; cursor < words.size();) {
        const auto count = AgcDriver::Pm4::PacketWords(words[cursor]);
        AgcDriver::Pm4::Execute(std::span(words).subspan(cursor, count), queue);
        cursor += count;
    }
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / static_cast<double>(packets);
}

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2u;
    return values.size() % 2u == 1u ? values[middle] : (values[middle - 1u] + values[middle]) / 2.0;
}

}

int main(int argc, char** argv) {
    void* block = nullptr;
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        std::size_t packets = 20000;
        std::size_t rounds = 5;
        for (int index = 1; index + 1 < argc; index += 2) {
            const std::string option = argv[index];
            if (option == "--packets") packets = std::strtoull(argv[index + 1], nullptr, 10);
            else if (option == "--rounds") rounds = std::strtoull(argv[index + 1], nullptr, 10);
        }
        Require(packets != 0 && rounds != 0, "--packets and --rounds must be positive");
#ifdef _WIN32
        block = VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        block = std::aligned_alloc(65536, 65536);
#endif
        Require(block != nullptr, "cannot allocate the label page");
        std::memset(block, 0, 65536);
        GuestAllocations::Mutation().Add(block, 65536, true, true, true);
        auto* label = static_cast<std::uint32_t*>(block);
        std::uint32_t serial = 0;
        const auto submitTimed = [&](std::vector<std::uint32_t> words) {
            ++serial;
            Release(words, label, serial);
            Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
            const auto start = std::chrono::steady_clock::now();
            AgcDriver::Submit(&packet, 0);
            Await(label, serial);
            const auto spent = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
            AgcDriverWaitIdle_nid_postfix();
            return spent;
        };
        submitTimed({});
        std::vector<double> empty;
        for (std::size_t round = 0; round < rounds; ++round) empty.push_back(submitTimed({}));
        const auto emptyNs = Median(empty);
        std::printf("[packet-cost] empty submission: %.1f us (median of %zu)\n", emptyNs / 1e3, rounds);
        for (const auto& mix : Mixes) {
            const auto words = RegisterStream(mix, packets);
            std::vector<double> table;
            std::vector<double> worker;
            for (std::size_t round = 0; round < rounds; ++round) {
                table.push_back(TableNanoseconds(words, packets));
                worker.push_back((submitTimed(words) - emptyNs) / static_cast<double>(packets));
            }
            const auto tableNs = Median(table);
            const auto workerNs = Median(worker);
            std::printf("[packet-cost] %-20s %zu packets: Pm4::Execute %.1f ns/packet, worker loop %.1f ns/packet (%.1fx), loop overhead %.1f ns/packet\n", mix.name, packets, tableNs, workerNs, tableNs > 0 ? workerNs / tableNs : 0.0, workerNs - tableNs);
            Require(workerNs > 0, std::string(mix.name) + ": the worker loop measured no time");
        }
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
}
