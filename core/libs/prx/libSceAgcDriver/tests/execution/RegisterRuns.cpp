#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
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

using AgcDriver::Graphics::RuntimeSwitch;
namespace RuntimeSwitches = AgcDriver::Graphics::RuntimeSwitches;

constexpr auto CompletionDeadline = std::chrono::seconds(60);
constexpr std::uint32_t ContextBase = 0xa000;
constexpr std::uint32_t ShaderBase = 0x2c00;
constexpr std::uint32_t UserConfigBase = 0xc000;

bool traceSlicing = false;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t Low(const void* p) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p)); }
std::uint32_t High(const void* p) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p) >> 32u); }

void SetRegisters(std::vector<std::uint32_t>& words, std::uint32_t opcode, std::uint32_t offset, std::initializer_list<std::uint32_t> values) {
    words.push_back(0xc0000000u | (static_cast<std::uint32_t>(values.size()) << 16u) | (opcode << 8u));
    words.push_back(offset);
    words.insert(words.end(), values);
}

void CopyRegister(std::vector<std::uint32_t>& words, std::uint32_t registerAddress, const void* destination) {
    words.insert(words.end(), {0xc0044000u, 0x00000500u, registerAddress, 0u, Low(destination), High(destination)});
}

void Release(std::vector<std::uint32_t>& words, const void* label, std::uint32_t value) {
    words.insert(words.end(), {0xc0064900u, 0x514u, 2u << 29u, Low(label), High(label), value, 0u, 0u});
}

void Submit(std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
}

void Await(const std::uint32_t* label, std::uint32_t value) {
    const auto deadline = std::chrono::steady_clock::now() + CompletionDeadline;
    while (*static_cast<const volatile std::uint32_t*>(label) != value) {
        Require(std::chrono::steady_clock::now() < deadline, "the submission's release never landed");
        std::this_thread::yield();
    }
}

void RunArm(bool runs, std::uint32_t* page, std::uint32_t serial) {
    RuntimeSwitches::Assign(RuntimeSwitch::RegisterRuns, runs);
    const auto arm = std::string(runs ? "with" : "without") + " APS5_REGISTER_RUNS";
    auto* label = page;
    auto* copied = page + 16;
    std::memset(copied, 0xee, 8 * sizeof(std::uint32_t));
    const auto base = serial * 0x100u;
    const auto coveredBefore = AgcDriverRegisterRunPackets_nid_postfix();

    std::vector<std::uint32_t> ending;
    SetRegisters(ending, 0x69, 0x110, {base + 1});
    SetRegisters(ending, 0x76, 0x0d, {base + 2});
    SetRegisters(ending, 0x79, 0x381, {base + 3});
    Submit(ending);
    AgcDriverWaitIdle_nid_postfix();

    std::vector<std::uint32_t> words;
    SetRegisters(words, 0x69, 0x100, {base + 4});
    SetRegisters(words, 0x76, 0x0c, {base + 5});
    SetRegisters(words, 0x79, 0x380, {base + 6});
    words.push_back(0x80000000u);
    SetRegisters(words, 0x69, 0x100, {base + 7, base + 8});
    words.insert(words.end(), {0xc0001000u, 0u});
    SetRegisters(words, 0x79, 0x380, {base + 9});
    SetRegisters(words, 0x76, 0x0c, {base + 10});
    SetRegisters(words, 0x69, 0x101, {base + 11});
    const std::array<std::uint32_t, 7> registers{ContextBase + 0x100, ContextBase + 0x101, ShaderBase + 0x0c, UserConfigBase + 0x380, ContextBase + 0x110, ShaderBase + 0x0d, UserConfigBase + 0x381};
    for (std::size_t index = 0; index < registers.size(); ++index) CopyRegister(words, registers[index], copied + index);
    Release(words, label, serial);
    Submit(words);
    Await(label, serial);
    AgcDriverWaitIdle_nid_postfix();

    const std::array<std::uint32_t, 7> expected{base + 7, base + 11, base + 10, base + 9, base + 1, base + 2, base + 3};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        Require(copied[index] == expected[index], arm + ": register 0x" + std::to_string(registers[index]) + " read " + std::to_string(copied[index]) + ", expected " + std::to_string(expected[index]));
    }
    const auto covered = AgcDriverRegisterRunPackets_nid_postfix() - coveredBefore;
    const std::uint64_t expectedCovered = runs && !traceSlicing ? 10 : 0;
    Require(covered == expectedCovered, arm + ": runs covered " + std::to_string(covered) + " register packets, expected " + std::to_string(expectedCovered));
    std::printf("[register-runs] %s: %zu registers match, %llu packets ran in runs\n", arm.c_str(), expected.size(), static_cast<unsigned long long>(covered));
}

}

int main() {
    void* block = nullptr;
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
#ifdef _WIN32
        block = VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        block = std::aligned_alloc(65536, 65536);
#endif
        Require(block != nullptr, "cannot allocate the label page");
        std::memset(block, 0, 65536);
        GuestAllocations::Mutation().Add(block, 65536, true, true, true);
        auto* page = static_cast<std::uint32_t*>(block);
        const bool traced = std::getenv("APS5_TRACE_SUMMARY_EVERY") != nullptr || std::getenv("APS5_TRACE_FRAMES") != nullptr;
        if (traced) {
            AgcDriver::PerformanceTrace::Present(1, 1, nullptr);
            Require(AgcDriver::PerformanceTrace::Recording(), "the trace is not recording after the first present");
            traceSlicing = std::getenv("APS5_TRACE_FRAMES") != nullptr;
        }
        RunArm(false, page, 1);
        RunArm(true, page, 2);
        RunArm(false, page, 3);
        RunArm(true, page, 4);
        RuntimeSwitches::Assign(RuntimeSwitch::RegisterRuns, false);
        if (traced) AgcDriver::PerformanceTrace::StopOutput();
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
