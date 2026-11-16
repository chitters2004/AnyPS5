#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "Optimization/SrtWalker.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

struct DriverTestAccess {
    static void Insert(std::uint64_t key, const std::shared_ptr<DispatchVariant>& variant, std::shared_ptr<const DrawDecode> decode, const DrawRelocationSource& source) {
        std::vector<std::shared_ptr<DispatchVariant>> fresh{variant};
        Driver::Get().insertDrawEntry(key, fresh, std::move(decode), 0, false, source);
    }
    static std::shared_ptr<const DrawRelocation> Learned(std::uint64_t identity) {
        auto& driver = Driver::Get();
        std::lock_guard lock(driver.drawCacheMutex);
        const auto found = driver.drawRelocations.find(identity);
        return found == driver.drawRelocations.end() ? nullptr : found->second;
    }
    static std::uint64_t Donor(std::uint64_t alias) {
        auto& driver = Driver::Get();
        std::lock_guard lock(driver.drawCacheMutex);
        const auto found = driver.drawAliases.find(alias);
        return found == driver.drawAliases.end() ? 0 : found->second;
    }
    static AgcDriver::DriverDetail::DrawRelocationCounters RelocationCounters() {
        auto& driver = Driver::Get();
        std::lock_guard lock(driver.drawCacheMutex);
        return driver.drawRelocationCounters;
    }
};

}

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::DispatchVariant;
using AgcDriver::DriverDetail::DrawEntry;
using AgcDriver::DriverDetail::DrawMiss;
using AgcDriver::DriverDetail::DrawRelocation;

alignas(256) constexpr std::array<std::uint32_t, 8> VertexCode{0x7e000208u, 0x7e020280u, 0x7e040280u, 0x7e0602f2u, 0xf80008cfu, 0x03020100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> SecondVertexCode = VertexCode;
alignas(256) constexpr std::array<std::uint32_t, 8> RecipeVertexCode = VertexCode;
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0xf4000100u, 0xfa000000u, 0x7e000204u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 16> TriangleVertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u, 0xf80008cfu, 0x04030201u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 16> ForwardedPixelCode{
    0xf4040080u, 0xfa000000u, 0xbf8cc07fu, 0xf4000101u, 0xfa000000u, 0xbf8cc07fu, 0x7e000204u, 0xd5480001u,
    0x02050800u, 0x7e020d01u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};
alignas(16) std::array<float, 4> Other{0.5f, 0.0f, 0.0f, 0.0f};
alignas(16) std::array<float, 4> ForwardedData{};
alignas(32) std::array<std::array<std::uint64_t, 4>, 6> AlignedTables{};
alignas(32) std::array<std::uint64_t, 4> OffsetTable{};
alignas(256) std::array<std::byte, 1024> ColorMemory{};
constexpr std::uint32_t LinearAttrib3 = 0x9000000u;
constexpr std::uint32_t RenderTargetAttrib3 = LinearAttrib3 | (0x1bu << 14u);
constexpr std::size_t TiledTargetBytes = 65536;

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};
alignas(8) Header SecondVertexHeader{};
alignas(8) Header RecipeVertexHeader{};
alignas(8) Header TriangleVertexHeader{};
alignas(8) Header ForwardedPixelHeader{};

const Shader* Registered(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
    return &header.shader;
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

std::vector<std::uint32_t> DrawFrom(std::initializer_list<const void*> sources, const std::uint32_t* vertexCode = VertexCode.data(), const std::uint32_t* pixelCode = PixelCode.data(), const void* target = ColorMemory.data(), std::uint32_t attrib3 = LinearAttrib3) {
    const auto color = reinterpret_cast<std::uintptr_t>(target);
    const auto vertex = reinterpret_cast<std::uintptr_t>(vertexCode);
    const auto pixel = reinterpret_cast<std::uintptr_t>(pixelCode);
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, attrib3}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
    };
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 1u << 1u}, {0x8c, 0},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 2u << 1u},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    for (const auto* source : sources) {
        const auto address = reinterpret_cast<std::uintptr_t>(source);
        Append(commands, 0x76, {0x00c, static_cast<std::uint32_t>(address)});
        Append(commands, 0x76, {0x00d, static_cast<std::uint32_t>(address >> 32u)});
        Append(commands, 0x2d, {3, 2});
    }
    return commands;
}

void MovedDataHitsTheCache(bool libraries) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const auto draw = [&](std::initializer_list<const void*> sources) {
        auto commands = DrawFrom(sources);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    static std::array<std::array<float, 4>, 8> same{};
    for (auto& entry : same) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (std::size_t i = 0; i < 4; ++i) counters = draw({same[i].data()});
    Require(counters.lookups == 4 && counters.hits == 0 && counters.inserts == 2, mode + ": the draws that teach the moved pointer and its contents made " + std::to_string(counters.hits) + " hits and " + std::to_string(counters.inserts) + " inserts, expected 0 and 2");
    for (std::size_t i = 4; i < same.size(); ++i) counters = draw({same[i].data()});
    Require(counters.lookups == 8 && counters.hits == 4 && counters.inserts == 2, mode + ": draws whose data moved again did not reuse the cached draw (" + std::to_string(counters.hits) + " hits in " + std::to_string(counters.lookups) + " lookups)");
    const auto other = draw({Other.data()});
    Require(other.lookups == 9 && other.hits == 4, mode + ": a draw whose moved data holds another value reused the cached draw");
    const auto again = draw({same[0].data()});
    Require(again.lookups == 10 && again.hits == 5, mode + ": a draw at an address seen before did not reuse the cached draw (" + std::to_string(again.hits) + " hits)");
}

void EvictedEntriesServeNoRelocation(bool libraries) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    Registered(SecondVertexHeader, SecondVertexCode.data(), sizeof(SecondVertexCode), 2);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const auto draw = [&](const float* source, const std::uint32_t* vertexCode) {
        auto commands = DrawFrom({source}, vertexCode);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    static std::array<std::array<float, 4>, 6> same{};
    for (auto& entry : same) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    alignas(16) static std::array<std::array<float, 4>, 3> others{};
    for (std::size_t i = 0; i < others.size(); ++i) others[i] = {0.25f + 0.03125f * static_cast<float>(i), 0.0f, 0.0f, 0.0f};
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (std::size_t i = 0; i < 4; ++i) counters = draw(same[i].data(), VertexCode.data());
    Require(counters.lookups == 4 && counters.hits == 0 && counters.inserts == 2, mode + ": the draws that teach the moved pointer and its contents made " + std::to_string(counters.hits) + " hits and " + std::to_string(counters.inserts) + " inserts, expected 0 and 2");
    const auto relocated = draw(same[4].data(), VertexCode.data());
    Require(relocated.hits == 1, mode + ": a draw whose data moved after the teaching draws did not reuse the cached draw (" + std::to_string(relocated.hits) + " hits)");
    auto evicted = relocated;
    for (const auto& entry : others) {
        draw(entry.data(), SecondVertexCode.data());
        evicted = draw(entry.data(), SecondVertexCode.data());
    }
    Require(evicted.evictions - relocated.evictions >= 2 && evicted.inserts - relocated.inserts >= 2, mode + ": draws of another vertex shader did not evict the two taught entries from the draw cache (" + std::to_string(evicted.inserts - relocated.inserts) + " inserts, " + std::to_string(evicted.evictions - relocated.evictions) + " evictions; run with APS5_DRAW_CACHE_ENTRIES=2)");
    const auto after = draw(same[5].data(), VertexCode.data());
    Require(after.lookups == evicted.lookups + 1 && after.hits == evicted.hits, mode + ": a draw whose data moved reused a draw the cache had evicted, through its relocation alias (" + std::to_string(after.hits - evicted.hits) + " hits)");
}

void RelocatingDrawsKeepTheirDonor(bool libraries) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    Registered(SecondVertexHeader, SecondVertexCode.data(), sizeof(SecondVertexCode), 2);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const auto draw = [&](const float* source, const std::uint32_t* vertexCode) {
        auto commands = DrawFrom({source}, vertexCode);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    constexpr std::size_t Rounds = 3;
    alignas(16) static std::array<std::array<float, 4>, Rounds> others{};
    for (std::size_t i = 0; i < others.size(); ++i) others[i] = {0.25f + 0.03125f * static_cast<float>(i), 0.0f, 0.0f, 0.0f};
    static std::array<std::array<float, 4>, 5 + Rounds> same{};
    for (auto& entry : same) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (std::size_t i = 0; i < 4; ++i) counters = draw(same[i].data(), VertexCode.data());
    Require(counters.lookups == 4 && counters.hits == 0 && counters.inserts == 2, mode + ": the draws that teach the moved pointer and its contents made " + std::to_string(counters.hits) + " hits and " + std::to_string(counters.inserts) + " inserts, expected 0 and 2");
    const auto start = draw(same[4].data(), VertexCode.data());
    Require(start.hits == 1, mode + ": a draw whose data moved after the teaching draws did not reuse the cached draw (" + std::to_string(start.hits) + " hits)");
    auto relocated = start;
    for (std::size_t i = 0; i < others.size(); ++i) {
        draw(others[i].data(), SecondVertexCode.data());
        const auto inserted = draw(others[i].data(), SecondVertexCode.data());
        relocated = draw(same[5 + i].data(), VertexCode.data());
        Require(relocated.hits == inserted.hits + 1, mode + ": a draw whose data moved again lost the cached draw it reused before each of " + std::to_string(i + 1) + " rounds of another vertex shader's draws (" + std::to_string(inserted.inserts - start.inserts) + " inserts, " + std::to_string(inserted.evictions - start.evictions) + " evictions)");
    }
    Require(relocated.evictions - start.evictions >= 2, mode + ": draws of another vertex shader evicted " + std::to_string(relocated.evictions - start.evictions) + " entries, expected at least 2 (run with APS5_DRAW_CACHE_ENTRIES=2)");
}

std::pair<unsigned long long, unsigned long long> AfterMissCounts(const std::string& text) {
    const std::string label = "draws after a relocation miss (learned by the finisher, not waited for): same identity ";
    const auto at = text.find(label);
    Require(at != std::string::npos, "the finisher drain report has no relocation miss entry:" + text);
    unsigned long long same = 0, other = 0;
    Require(std::sscanf(text.c_str() + at + label.size(), "%llu, other identity %llu", &same, &other) == 2, "the finisher drain report's relocation miss entry has no counts:" + text);
    return {same, other};
}

void RelocationDrainsNameTheirIdentity(bool libraries) {
    using AgcDriver::DriverDetail::DrawFinisher;
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    Registered(SecondVertexHeader, SecondVertexCode.data(), sizeof(SecondVertexCode), 2);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    alignas(16) static std::array<std::array<float, 4>, 3> same{};
    for (auto& entry : same) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    alignas(16) static std::array<float, 4> second{0.5f, 0.0f, 0.0f, 0.0f};
    static_cast<void>(DrawFinisher::DrainText());
    auto commands = DrawFrom({same[0].data(), same[1].data(), same[2].data()});
    const auto other = DrawFrom({second.data()}, SecondVertexCode.data());
    commands.insert(commands.end(), other.begin(), other.end());
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
    const auto text = DrawFinisher::DrainText();
    const auto [sameCount, otherCount] = AfterMissCounts(text);
    Require(sameCount == 2, mode + ": " + std::to_string(sameCount) + " draws after a miss of their own shaders were counted, expected 2:" + text);
    Require(otherCount == 1, mode + ": " + std::to_string(otherCount) + " draws after a miss of other shaders were counted, expected 1:" + text);
}

std::uint64_t DrawHits(std::span<const float* const> sources) {
    const auto start = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    AgcDriver::DriverDetail::DrawEntryCounters counters = start;
    for (const auto* source : sources) {
        auto commands = DrawFrom({source});
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        counters = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    }
    Require(counters.lookups - start.lookups == sources.size(), "the draws were not all looked up in the draw cache");
    return counters.hits - start.hits;
}

std::uint64_t MovedDataHits(std::span<const std::array<float, 4>> sources) {
    std::vector<const float*> addresses;
    for (const auto& source : sources) addresses.push_back(source.data());
    return DrawHits(addresses);
}

void RelocationOffServesNoMovedData(bool libraries) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    alignas(16) static std::array<std::array<float, 4>, 8> moved{};
    for (auto& entry : moved) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    const auto hits = MovedDataHits(moved);
    Require(hits == 0, mode + ": with APS5_NO_DRAW_RELOCATION " + std::to_string(hits) + " draws whose data moved reused a cached draw");
}

void PipelineVerifyChecksRelocatedHits(bool libraries) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    alignas(16) static std::array<std::array<float, 4>, 8> moved{};
    for (auto& entry : moved) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    const auto start = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    const auto hits = MovedDataHits(moved);
    const auto counters = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    Require(hits >= 4, mode + ": only " + std::to_string(hits) + " draws whose data moved reused a cached draw");
    Require(counters.verifyHits - start.verifyHits == hits, mode + ": APS5_VERIFY_DRAW_PIPELINE rebuilt " + std::to_string(counters.verifyHits - start.verifyHits) + " of " + std::to_string(hits) + " relocated hits");
    Require(counters.verifyDecodes > start.verifyDecodes, mode + ": APS5_VERIFY_DRAW_PIPELINE decoded no relocated hit again");
    Require(counters.verifyMismatches == start.verifyMismatches && counters.verifyDecodeMismatches == start.verifyDecodeMismatches, mode + ": a relocated hit differed from its full rebuild (" + std::to_string(counters.verifyMismatches - start.verifyMismatches) + " stages, " + std::to_string(counters.verifyDecodeMismatches - start.verifyDecodeMismatches) + " decodes)");
}

std::shared_ptr<const DispatchVariant> TaggedRuns() {
    alignas(16) static constexpr std::array<std::uint32_t, 4> data{0x1000u, 0u, 0u, 0u};
    constexpr std::uint64_t address = 0x2000;
    const std::array<ShaderRecompiler::MemoryRegion, 1> regions{{{address, std::as_bytes(std::span(data))}}};
    ShaderRecompiler::SrtUserDataUse use;
    use.asBase = 0b1;
    use.reads = {{address, 0}};
    auto variant = std::make_shared<DispatchVariant>();
    AgcDriver::DriverDetail::Driver::tagVariantRuns(*variant, regions, use, true, 0);
    return variant;
}

void TaggingIgnoresTheSwitch() {
    using AgcDriver::Graphics::RuntimeSwitch;
    namespace Switches = AgcDriver::Graphics::RuntimeSwitches;
    const auto clear = TaggedRuns();
    Switches::Assign(RuntimeSwitch::NoDrawRelocation, true);
    const auto set = TaggedRuns();
    Switches::Assign(RuntimeSwitch::NoDrawRelocation, false);
    Require(clear->relocatableWords == 0b1, "a word read only as a base was not tagged relocatable");
    Require(set->relocatableWords == clear->relocatableWords && set->runs == clear->runs, "a variant tagged while the relocation switch was set differs from one tagged with it clear, so it would ban its words once relocation is turned back on");
}

void RelocationSwitchFlipsMidRun(bool libraries) {
    using AgcDriver::Graphics::RuntimeSwitch;
    namespace Switches = AgcDriver::Graphics::RuntimeSwitches;
    TaggingIgnoresTheSwitch();
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    Require(!Switches::Set(RuntimeSwitch::NoDrawRelocation), "the relocation switch started set");
    alignas(16) static std::array<std::array<float, 4>, 8> off{};
    alignas(16) static std::array<std::array<float, 4>, 8> on{};
    for (auto& entry : off) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    for (auto& entry : on) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    constexpr std::size_t Repeats = 3;
    std::vector<const float*> offDraws;
    for (const auto& source : off) offDraws.insert(offDraws.end(), Repeats, source.data());
    Switches::Assign(RuntimeSwitch::NoDrawRelocation, true);
    const auto offHits = DrawHits(offDraws);
    Switches::Assign(RuntimeSwitch::NoDrawRelocation, false);
    Require(offHits == off.size() * (Repeats - 2), mode + ": with the relocation switch set " + std::to_string(offHits) + " draws reused a cached draw, expected " + std::to_string(off.size() * (Repeats - 2)) + " repeats of unmoved data and no moved data");
    const auto onHits = MovedDataHits(on);
    Require(onHits >= 4, mode + ": after the relocation switch was cleared mid-run only " + std::to_string(onHits) + " of " + std::to_string(on.size()) + " draws whose data moved reused a cached draw, expected at least 4 as in a fresh run");
}

struct LearningInput {
    std::shared_ptr<DispatchVariant> variant;
    std::shared_ptr<const AgcDriver::DriverDetail::DrawDecode> decode;
};

constexpr std::uint32_t LearnedBase = 0x0c;
constexpr std::uint32_t LearnedPointer = 0x1000;

LearningInput Learning(std::uint64_t relocatable, std::uint64_t values, std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> runs) {
    auto variant = std::make_shared<DispatchVariant>();
    variant->relocatableWords = relocatable;
    variant->valueWords = values;
    for (const auto& run : runs) {
        variant->runs.push_back(run);
        variant->runBases.push_back(0);
    }
    auto decode = std::make_shared<AgcDriver::DriverDetail::DrawDecode>();
    AgcDriver::DriverDetail::DrawProgram program{};
    program.userDataBase = LearnedBase;
    program.userData = {LearnedPointer, 0, 0, 0, 0, 0};
    decode->programs.push_back(std::move(program));
    return {std::move(variant), std::move(decode)};
}

std::vector<LearningInput> LearningInputs() {
    return {
        Learning(0b01101, 0b10000, {{LearnedPointer + 0x10, LearnedPointer + 0x20}}),
        Learning(0b01001, 0b10000, {{LearnedPointer + 0x40, LearnedPointer + 0x48}}),
        Learning(0b11001, 0, {{LearnedPointer + 0x10, LearnedPointer + 0x20}}),
    };
}

std::string ShapeText(const DrawRelocation& shape) {
    std::string text = "words";
    for (const auto& word : shape.words) {
        text += " " + std::to_string(word.reg) + (word.value ? "v" : "r") + "@" + std::to_string(word.high) + "[";
        for (const auto& [offset, words] : word.reads) text += std::to_string(offset) + "+" + std::to_string(words) + ";";
        text += "]";
    }
    text += " banned";
    for (const auto reg : shape.banned) text += " " + std::to_string(reg);
    return text;
}

void LearningIgnoresInsertOrder() {
    const auto inputs = LearningInputs();
    std::array<std::size_t, 3> order{0, 1, 2};
    std::string first;
    do {
        std::shared_ptr<const DrawRelocation> shape;
        for (const auto index : order) {
            DrawEntry entry;
            entry.decode = inputs[index].decode;
            entry.stages = {{inputs[index].variant}};
            shape = AgcDriver::DriverDetail::Driver::MergeDrawRelocation(shape, entry);
        }
        const auto text = ShapeText(*shape);
        if (first.empty()) first = text;
        Require(text == first, "learning in insert order " + std::to_string(order[0]) + std::to_string(order[1]) + std::to_string(order[2]) + " gave '" + text + "', the first order gave '" + first + "'");
    } while (std::next_permutation(order.begin(), order.end()));
    const auto expected = "words " + std::to_string(LearnedBase) + "r@0[16+4;64+2;] " + std::to_string(LearnedBase + 3) + "r@0[] banned " + std::to_string(LearnedBase + 1) + " " + std::to_string(LearnedBase + 2) + " " + std::to_string(LearnedBase + 4) + " " + std::to_string(LearnedBase + 5);
    Require(first == expected, "learning the three variants gave '" + first + "', expected '" + expected + "'");
}

void LearningTakesEachJobsOwnKey() {
    using AgcDriver::DriverDetail::DriverTestAccess;
    const auto inputs = LearningInputs();
    constexpr std::uint64_t Forward = 0x51000, Backward = 0x52000, Single = 0x53000;
    const std::array<std::size_t, 3> forward{0, 1, 2}, backward{2, 0, 1};
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        DriverTestAccess::Insert(0xa100 + i, inputs[forward[i]].variant, inputs[forward[i]].decode, {0xb100 + i, Forward});
        DriverTestAccess::Insert(0xa200 + i, inputs[backward[i]].variant, inputs[backward[i]].decode, {0xb200 + i, Backward});
    }
    DriverTestAccess::Insert(0xa300, inputs[1].variant, inputs[1].decode, {0xb300, Single});
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        Require(DriverTestAccess::Donor(0xb100 + i) == 0xa100 + i && DriverTestAccess::Donor(0xb200 + i) == 0xa200 + i, "an alias names another job's draw key as its donor (alias " + std::to_string(i) + ")");
    }
    Require(DriverTestAccess::Donor(0xb300) == 0xa300, "the last job's alias names another job's draw key as its donor");
    const auto forwardShape = DriverTestAccess::Learned(Forward), backwardShape = DriverTestAccess::Learned(Backward), single = DriverTestAccess::Learned(Single);
    Require(forwardShape != nullptr && backwardShape != nullptr && single != nullptr, "an inserted job's identity learned no relocation");
    Require(ShapeText(*forwardShape) == ShapeText(*backwardShape), "inserts of the same three jobs in two orders learned '" + ShapeText(*forwardShape) + "' and '" + ShapeText(*backwardShape) + "'");
    DrawEntry entry;
    entry.decode = inputs[1].decode;
    entry.stages = {{inputs[1].variant}};
    const auto alone = AgcDriver::DriverDetail::Driver::MergeDrawRelocation(nullptr, entry);
    Require(ShapeText(*single) == ShapeText(*alone), "a job learned '" + ShapeText(*single) + "', not its own entry's '" + ShapeText(*alone) + "'");
}

void FillLearningEntry(DrawEntry& entry, const LearningInput& input, bool compiled) {
    entry.decode = input.decode;
    entry.stages.assign(input.decode->programs.size(), {});
    if (compiled) entry.stages.front().push_back(input.variant);
}

void UnchangedLearningKeepsTheShape() {
    using AgcDriver::DriverDetail::Driver;
    using AgcDriver::DriverDetail::DriverTestAccess;
    const auto inputs = LearningInputs();
    DrawEntry entry0, entry1, bare;
    FillLearningEntry(entry0, inputs[0], true);
    FillLearningEntry(entry1, inputs[1], true);
    FillLearningEntry(bare, inputs[0], false);
    const auto first = Driver::MergeDrawRelocation(nullptr, entry0);
    Require(first != nullptr, "learning a first entry gave no shape");
    const auto firstText = ShapeText(*first);
    Require(Driver::MergeDrawRelocation(first, entry0) == first, "learning the same entry again copied a shape it left unchanged");
    const auto second = Driver::MergeDrawRelocation(first, entry1);
    Require(second != first, "learning an entry with other reads did not make a new shape");
    Require(ShapeText(*first) == firstText, "learning into a published shape changed it to '" + ShapeText(*first) + "', it was '" + firstText + "'");
    Require(Driver::MergeDrawRelocation(second, entry1) == second && Driver::MergeDrawRelocation(second, entry0) == second, "learning entries already in a shape copied it");
    const auto empty = Driver::MergeDrawRelocation(nullptr, bare);
    Require(empty != nullptr && empty->words.empty() && empty->banned.empty(), "an entry with no compiled stage did not learn an empty shape for a new identity");

    constexpr std::uint64_t Identity = 0x54000;
    const auto before = DriverTestAccess::RelocationCounters();
    DriverTestAccess::Insert(0xa400, inputs[0].variant, inputs[0].decode, {0xb400, Identity});
    const auto learned = DriverTestAccess::Learned(Identity);
    DriverTestAccess::Insert(0xa401, inputs[0].variant, inputs[0].decode, {0xb401, Identity});
    Require(learned != nullptr && DriverTestAccess::Learned(Identity) == learned, "a second insert of the same job replaced its identity's learned shape");
    DriverTestAccess::Insert(0xa402, inputs[1].variant, inputs[1].decode, {0xb402, Identity});
    const auto widened = DriverTestAccess::Learned(Identity);
    Require(widened != learned && ShapeText(*learned) == firstText, "an insert with other reads did not publish a new shape, or changed the published one");
    const auto after = DriverTestAccess::RelocationCounters();
    Require(after.learnedChanged - before.learnedChanged == 2 && after.learnedUnchanged - before.learnedUnchanged == 1, "three inserts counted " + std::to_string(after.learnedChanged - before.learnedChanged) + " changed and " + std::to_string(after.learnedUnchanged - before.learnedUnchanged) + " unchanged learnings, expected 2 and 1");
}

void SplitRunsNameTheirSharedWords() {
    alignas(16) static std::array<std::uint32_t, 4> data{1u, 2u, 3u, 4u};
    auto table = std::make_shared<ShaderRecompiler::SrtGuestWords>();
    table->words = {5u, 6u, 7u, 8u};
    const auto dataAddress = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data.data()));
    const auto tableAddress = dataAddress + 0x1000u;
    const std::array<ShaderRecompiler::MemoryRegion, 2> regions{{{dataAddress, std::as_bytes(std::span(data))}, {tableAddress, std::as_bytes(std::span(table->words))}}};
    ShaderRecompiler::SrtUserDataUse use;
    use.asBase = 0x3u;
    use.reads = {{dataAddress, 0}, {dataAddress + 8u, 1}};
    const std::array<std::shared_ptr<const ShaderRecompiler::SrtGuestWords>, 1> supplied{table};
    AgcDriver::DriverDetail::DispatchVariant variant;
    AgcDriver::DriverDetail::Driver::tagVariantRuns(variant, regions, use, true, 0, supplied);
    Require(variant.runs.size() > regions.size(), "two user-data bases into one region did not split it into several runs");
    Require(variant.shared.size() == 1u && variant.shared.front().regions == 1u, "the table region's words were not recorded as one shared run");
    const auto& shared = variant.shared.front();
    std::size_t first = 0;
    for (std::size_t run = 0; run < shared.region && run < variant.runs.size(); ++run) first += static_cast<std::size_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t));
    Require(shared.region < variant.runs.size() && variant.runs[shared.region].first == tableAddress && first == shared.first, "the shared words of a variant whose runs were split for relocation name run " + std::to_string(shared.region) + " of " + std::to_string(variant.runs.size()) + ", not the table's run " + std::to_string(variant.runs.size() - 1u));
}

class TiledTarget {
public:
    TiledTarget() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(TiledTargetBytes, TiledTargetBytes));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, TiledTargetBytes, PAGE_READWRITE, TiledTargetBytes);
#else
        void* mapped = mmap(nullptr, TiledTargetBytes * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + TiledTargetBytes - 1) & ~(static_cast<std::uintptr_t>(TiledTargetBytes) - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned != begin + TiledTargetBytes) munmap(reinterpret_cast<void*>(aligned + TiledTargetBytes), begin + TiledTargetBytes - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, TiledTargetBytes);
        }
#endif
        Require(block != nullptr, "cannot allocate the tiled color target");
        std::memset(block, 0, TiledTargetBytes);
        GuestAllocations::Mutation().Add(block, TiledTargetBytes, true, true, true);
    }

    TiledTarget(const TiledTarget&) = delete;
    TiledTarget& operator=(const TiledTarget&) = delete;

    const void* Data() const { return block; }

private:
    std::uint8_t* block = nullptr;
};

void RelocatedDrawsReuseTheirRecipe(bool libraries) {
    Registered(RecipeVertexHeader, RecipeVertexCode.data(), sizeof(RecipeVertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    static const TiledTarget target;
    const auto draw = [&](const float* source) {
        auto commands = DrawFrom({source}, RecipeVertexCode.data(), PixelCode.data(), target.Data(), RenderTargetAttrib3);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    alignas(16) static std::array<std::array<float, 4>, 10> moved{};
    for (auto& entry : moved) entry = {0.75f, 0.0f, 0.0f, 0.0f};
    for (std::size_t i = 0; i < 4; ++i) draw(moved[i].data());
    const auto start = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    const auto recipesBefore = AgcDriver::VulkanDevice::DrawRecipeHits();
    AgcDriver::DriverDetail::DrawEntryCounters counters = start;
    for (std::size_t i = 4; i < moved.size(); ++i) counters = draw(moved[i].data());
    const auto recipeHits = AgcDriver::VulkanDevice::DrawRecipeHits() - recipesBefore;
    const auto hits = counters.hits - start.hits;
    Require(hits == moved.size() - 4, mode + ": draws whose data moved again made " + std::to_string(hits) + " draw-cache hits, expected " + std::to_string(moved.size() - 4));
    Require(recipeHits == hits, mode + ": " + std::to_string(recipeHits) + " of " + std::to_string(hits) + " draws served by a relocated entry were recorded from the cached draw's recipe, expected all");
}

void UnrelocatedAliasesStayOut(bool libraries) {
    Registered(SecondVertexHeader, SecondVertexCode.data(), sizeof(SecondVertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const auto draw = [&](const float* source) {
        auto commands = DrawFrom({source}, SecondVertexCode.data());
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    alignas(16) static std::array<float, 4> repeated{1.0f, 0.0f, 0.0f, 0.0f};
    alignas(16) static std::array<std::array<std::array<float, 4>, 2>, 12> twice{};
    for (std::size_t i = 0; i < twice.size(); ++i) {
        for (auto& entry : twice[i]) entry = {0.25f + 0.03125f * static_cast<float>(i), 0.0f, 0.0f, 0.0f};
    }
    draw(repeated.data());
    const auto start = draw(repeated.data());
    AgcDriver::DriverDetail::DrawEntryCounters counters = start;
    for (const auto& pair : twice) {
        for (const auto& entry : pair) draw(entry.data());
        counters = draw(repeated.data());
    }
    const auto inserts = counters.inserts - start.inserts;
    Require(inserts <= AgcDriver::DriverDetail::DrawRelocationTrial::ProbationAdmissions, mode + ": draws whose alias repeats but never relocates were cached (" + std::to_string(inserts) + " inserts for " + std::to_string(twice.size()) + " aliases)");
    Require(counters.hits - start.hits == twice.size(), mode + ": the exact key of a repeated draw lost its hits next to draws whose alias never relocates (" + std::to_string(counters.hits - start.hits) + " hits in " + std::to_string(twice.size()) + " draws)");
}

void MovedForwardedWordReachesStageDataHit(bool libraries) {
    Registered(TriangleVertexHeader, TriangleVertexCode.data(), sizeof(TriangleVertexCode), 2);
    Registered(ForwardedPixelHeader, ForwardedPixelCode.data(), sizeof(ForwardedPixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    constexpr std::byte Kept{0x40};
    const auto draw = [&](const void* table) {
        ColorMemory.fill(Kept);
        auto commands = DrawFrom({table}, TriangleVertexCode.data(), ForwardedPixelCode.data());
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(ColorMemory.data()), ColorMemory.size());
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    const auto pixels = [&](std::array<std::uint8_t, 4> expected) {
        std::size_t drawn = 0;
        for (std::size_t at = 0; at < ColorMemory.size(); at += 4) {
            const std::array<std::uint8_t, 4> pixel{static_cast<std::uint8_t>(ColorMemory[at]), static_cast<std::uint8_t>(ColorMemory[at + 1]), static_cast<std::uint8_t>(ColorMemory[at + 2]), static_cast<std::uint8_t>(ColorMemory[at + 3])};
            if (pixel == std::array<std::uint8_t, 4>{0x40, 0x40, 0x40, 0x40}) continue;
            if (pixel != expected) return "pixel " + std::to_string(at / 4) + " is " + std::to_string(pixel[0]) + "," + std::to_string(pixel[1]) + "," + std::to_string(pixel[2]) + "," + std::to_string(pixel[3]) + ", expected " + std::to_string(expected[0]) + "," + std::to_string(expected[1]) + "," + std::to_string(expected[2]) + "," + std::to_string(expected[3]);
            ++drawn;
        }
        return drawn == 0 ? std::string("the draw wrote no pixel") : std::string();
    };
    const auto data = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ForwardedData.data()));
    for (auto& table : AlignedTables) table[0] = data;
    OffsetTable[2] = data;
    ForwardedData = {1.0f, 0.0f, 0.0f, 0.0f};
    const auto start = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (const auto& table : AlignedTables) counters = draw(table.data());
    Require(counters.hits != start.hits, mode + ": draws whose pointer table moved to identical contents never reused the cached draw (" + std::to_string(counters.lookups - start.lookups) + " lookups, " + std::to_string(counters.inserts - start.inserts) + " inserts)");
    const auto taught = pixels({255, 0, 0, 255});
    Require(taught.empty(), mode + ": a draw through a pointer table with bit 4 of its address clear: " + taught);
    ForwardedData[0] = 0.25f;
    const auto before = counters;
    counters = draw(&OffsetTable[2]);
    Require(counters.lookups == before.lookups + 1 && counters.hits == before.hits + 1 && counters.stageDataHits == before.stageDataHits + 1, mode + ": a draw whose pointer table moved and whose data changed in place was not a relocated stage data hit (" + std::to_string(counters.hits - before.hits) + " hits, " + std::to_string(counters.stageDataHits - before.stageDataHits) + " stage data hits)");
    const auto moved = pixels({64, 255, 255, 64});
    Require(moved.empty(), mode + ": a relocated stage data hit did not draw with the moved user word: " + moved);
}

}

int main(int argc, char** argv) {
    try {
        const bool evicted = argc == 2 && std::string(argv[1]) == "--evicted";
        const bool kept = argc == 2 && std::string(argv[1]) == "--kept";
        const bool drains = argc == 2 && std::string(argv[1]) == "--drains";
        const bool off = argc == 2 && std::string(argv[1]) == "--relocation-off";
        const bool flip = argc == 2 && std::string(argv[1]) == "--relocation-switch";
        const bool verify = argc == 2 && std::string(argv[1]) == "--pipeline-verify";
        if (argc == 2 && std::string(argv[1]) == "--learning") {
            LearningIgnoresInsertOrder();
            LearningTakesEachJobsOwnKey();
            UnchangedLearningKeepsTheShape();
            std::cout << "Relocation learning took each job's own key in any insert order\n";
            return 0;
        }
        if (!evicted && !kept && !drains && !off && !flip && !verify) SplitRunsNameTheirSharedWords();
        bool libraries = false;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            libraries = device->GraphicsPipelineLibraries();
        }
        if (!libraries && std::getenv("APS5_NO_GPL") == nullptr) return VulkanTestSkipped;
        if (evicted) {
            EvictedEntriesServeNoRelocation(libraries);
            AgcDriverShutdown_nid_postfix();
            std::cout << "An evicted draw served no relocation " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
            return 0;
        }
        if (verify) {
            PipelineVerifyChecksRelocatedHits(libraries);
            AgcDriverShutdown_nid_postfix();
            std::cout << "APS5_VERIFY_DRAW_PIPELINE rebuilt every relocated hit " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
            return 0;
        }
        if (off || flip) {
            if (off) RelocationOffServesNoMovedData(libraries);
            else RelocationSwitchFlipsMidRun(libraries);
            AgcDriverShutdown_nid_postfix();
            std::cout << (off ? "Relocation off served no moved data " : "The relocation switch flipped mid-run ") << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
            return 0;
        }
        if (drains) {
            if (!AgcDriver::DriverDetail::DrawFinisher::Enabled() || !AgcDriver::DriverDetail::DrawFinisher::Overlap()) return VulkanTestSkipped;
            RelocationDrainsNameTheirIdentity(libraries);
            AgcDriverShutdown_nid_postfix();
            std::cout << "Relocation drains named their identity " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
            return 0;
        }
        if (kept) {
            RelocatingDrawsKeepTheirDonor(libraries);
            AgcDriverShutdown_nid_postfix();
            std::cout << "A draw relocated from a cached draw kept it cached " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
            return 0;
        }
        MovedDataHitsTheCache(libraries);
        RelocatedDrawsReuseTheirRecipe(libraries);
        UnrelocatedAliasesStayOut(libraries);
        MovedForwardedWordReachesStageDataHit(libraries);
        if (std::getenv("APS5_VERIFY_DRAW_RECIPE") != nullptr) {
            const auto counters = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
            Require(counters.verifyDecodeMismatches == 0, "the effective relocated draw decode differed from its full decode");
        }
        AgcDriverShutdown_nid_postfix();
        std::cout << "A draw whose data moved reused the cached draw " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
