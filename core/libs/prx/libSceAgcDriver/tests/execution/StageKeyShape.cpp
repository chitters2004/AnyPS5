#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "Recompiler.hpp"
#include <atomic>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using ShaderRecompiler::DescriptorBinding;
using ShaderRecompiler::DescriptorKind;
using ShaderRecompiler::DescriptorRole;

void Expect(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("stage key shape: " + what);
}

ShaderRecompiler::RecompileResult Program(std::uint32_t patchedWord, std::uint32_t staticWord) {
    ShaderRecompiler::RecompileResult result;
    result.variantId = 0x5eed;
    DescriptorBinding buffer{DescriptorKind::StorageBuffer, DescriptorRole::GuestBuffers, 0, 0, 1, {patchedWord, 0x00100000u, 64u, 0x31016facu}};
    buffer.readOnly = true;
    buffer.bufferWritten = {false};
    DescriptorBinding data{DescriptorKind::StorageBuffer, DescriptorRole::ShaderData, 0, 1, 1, {staticWord, 2u, 3u, 4u}};
    result.bindings = {buffer, data};
    auto shape = std::make_shared<ShaderRecompiler::ResultKeyShape>();
    shape->patchedBindings = {0};
    result.keyShape = std::move(shape);
    return result;
}

AgcDriver::Graphics::CompiledShader Stage(const ShaderRecompiler::RecompileResult& result) {
    return {ShaderRecompiler::ShaderStage::Fragment, &result, 0};
}

bool Shapes() {
    return std::getenv("APS5_NO_KEY_SHAPE") == nullptr;
}

void FirstFillRaces() {
    constexpr int Rounds = 200;
    constexpr int Threads = 8;
    for (int round = 0; round < Rounds; ++round) {
        const auto result = Program(0x1000u + static_cast<std::uint32_t>(round), 7u);
        const auto stage = Stage(result);
        const auto before = AgcDriver::Graphics::StageKeyMemoCounters();
        std::atomic<int> ready{0};
        std::vector<std::shared_ptr<const ShaderRecompiler::ResultKeyShape::Static>> seen(Threads);
        std::vector<std::thread> threads;
        for (int index = 0; index < Threads; ++index) {
            threads.emplace_back([&, index] {
                ready.fetch_add(1);
                while (ready.load() < Threads) {}
                seen[index] = AgcDriver::Graphics::StageStaticKeys(stage);
            });
        }
        for (auto& thread : threads) thread.join();
        const auto after = AgcDriver::Graphics::StageKeyMemoCounters();
        if (!Shapes()) {
            for (const auto& keys : seen) Expect(keys == nullptr, "a shape was used under APS5_NO_KEY_SHAPE");
            continue;
        }
        for (const auto& keys : seen) Expect(keys != nullptr && keys == seen.front(), "threads racing the first fill of one shape got different static keys");
        Expect(after.staticFills - before.staticFills == 1, "the first fill of one shape published " + std::to_string(after.staticFills - before.staticFills) + " times");
        Expect((after.staticLosses - before.staticLosses) + (after.staticHits - before.staticHits) == Threads - 1, "the threads that did not publish were not counted as losers or hits");
    }
    std::printf("stage key shape: %d rounds of %d threads racing the first fill each published one static key\n", Rounds, Threads);
}

void CopiesShareTheStaticPart() {
    if (!Shapes()) return;
    const auto donor = Program(0x2000u, 7u);
    const auto keys = AgcDriver::Graphics::StageStaticKeys(Stage(donor));
    auto patched = donor;
    patched.bindings[0].guestDescriptor[0] = 0x3000u;
    Expect(patched.keyShape == donor.keyShape, "a copy did not keep its donor's shape");
    Expect(AgcDriver::Graphics::StageStaticKeys(Stage(patched)) == keys, "a copy that differs only in a patched slot did not share its donor's static key");
    auto stale = donor;
    stale.bindings[1].guestDescriptor[0] = 8u;
    const bool verify = std::getenv("APS5_VERIFY_KEY_MEMO") != nullptr || std::getenv("APS5_VERIFY_DRAW_PLANS") != nullptr;
    bool refused = false;
    try {
        static_cast<void>(AgcDriver::Graphics::StageStaticKeys(Stage(stale)));
    } catch (const std::exception&) {
        refused = true;
    }
    Expect(refused == verify, verify ? "verify did not refuse a copy whose unpatched binding changed under a shared static key" : "a static key hit was refused without verify");
    std::puts("stage key shape: copies share the static key; verify refuses a copy whose unpatched slots changed");
}

double ThreadMicroseconds() {
    timespec now{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return static_cast<double>(now.tv_sec) * 1e6 + static_cast<double>(now.tv_nsec) / 1e3;
}

ShaderRecompiler::RecompileResult Shaped(std::uint32_t staticBufferElements, std::uint32_t patchedBufferElements) {
    ShaderRecompiler::RecompileResult result;
    result.variantId = 0xbe7c;
    const auto buffers = [](std::uint32_t binding, std::uint32_t elements, std::uint32_t seed) {
        DescriptorBinding buffer{DescriptorKind::StorageBuffer, DescriptorRole::GuestBuffers, 0, binding, elements, {}};
        buffer.readOnly = true;
        for (std::uint32_t element = 0; element < elements; ++element) {
            for (const auto word : {seed + element * 64u, 0x00100000u, 64u, 0x31016facu}) buffer.guestDescriptor.push_back(word);
            buffer.bufferWritten.push_back(false);
        }
        return buffer;
    };
    result.bindings.push_back(buffers(0, patchedBufferElements, 0x10000u));
    DescriptorBinding data{DescriptorKind::StorageBuffer, DescriptorRole::ShaderData, 0, 1, 1, std::vector<std::uint32_t>(32, 5u)};
    result.bindings.push_back(data);
    for (std::uint32_t image = 0; image < 3; ++image) result.bindings.push_back({DescriptorKind::SampledImage, DescriptorRole::GuestImages, 0, 2 + image, 1, std::vector<std::uint32_t>(8, 0x100u + image)});
    result.bindings.push_back({DescriptorKind::Sampler, DescriptorRole::GuestSamplers, 0, 5, 1, std::vector<std::uint32_t>(4, 9u)});
    if (staticBufferElements != 0) result.bindings.push_back(buffers(6, staticBufferElements, 0x900000u));
    auto shape = std::make_shared<ShaderRecompiler::ResultKeyShape>();
    shape->patchedBindings = {0};
    result.keyShape = std::move(shape);
    return result;
}

void BenchIdentity() {
    auto result = Program(0x1000u, 0x2000u);
    result.spirv = std::vector<std::uint32_t>{0x07230203u, 0x00010500u, 0u, 16u, 0u};
    for (std::uint32_t binding = 2; binding < 24u; ++binding) result.bindings.push_back({DescriptorKind::SampledImage, DescriptorRole::GuestImages, 0, binding, 1, {}});
    constexpr int Calls = 200000;
    std::uint64_t sink = 0;
    const auto computedStart = ThreadMicroseconds();
    for (int call = 0; call < Calls; ++call) sink += result.Identity().low;
    const auto computedNs = (ThreadMicroseconds() - computedStart) * 1000.0 / Calls;
    result.SealIdentity();
    const auto memoStart = ThreadMicroseconds();
    for (int call = 0; call < Calls; ++call) sink += result.Identity().low;
    const auto memoNs = (ThreadMicroseconds() - memoStart) * 1000.0 / Calls;
    std::printf("bench identity, 24 bindings: unsealed %.1f ns, sealed %.1f ns per call, thread CPU (%llu)\n", computedNs, memoNs, static_cast<unsigned long long>(sink & 1u));
}

void BenchPushShare(const char* name, std::uint32_t staticBufferElements) {
    auto donor = Shaped(staticBufferElements, 1);
    donor.pushConstants.assign(64, std::byte{0x5a});
    constexpr int Draws = 20000;
    std::uint64_t sink = 0;
    const auto copyStart = ThreadMicroseconds();
    for (int draw = 0; draw < Draws; ++draw) {
        auto copy = std::make_shared<ShaderRecompiler::RecompileResult>(donor);
        copy->pushConstants[0] = static_cast<std::byte>(draw);
        sink += static_cast<std::uint64_t>(copy->pushConstants[0]);
    }
    const auto copyUs = (ThreadMicroseconds() - copyStart) / Draws;
    const auto shareStart = ThreadMicroseconds();
    for (int draw = 0; draw < Draws; ++draw) {
        auto push = std::make_shared<const std::vector<std::byte>>(donor.pushConstants);
        sink += static_cast<std::uint64_t>((*push)[1]);
    }
    const auto shareUs = (ThreadMicroseconds() - shareStart) / Draws;
    std::printf("bench push share, %s: patched copy of the donor %.3f us, shared donor with its own push bytes %.3f us per stage, thread CPU (%llu)\n", name, copyUs, shareUs, static_cast<unsigned long long>(sink & 1u));
}

void Bench(const char* name, std::uint32_t staticBufferElements, std::uint32_t patchedBufferElements) {
    using AgcDriver::Graphics::ShaderResources;
    constexpr int Draws = 4000;
    const auto donor = Shaped(staticBufferElements, patchedBufferElements);
    std::vector<ShaderRecompiler::RecompileResult> copies(Draws, donor);
    for (int draw = 0; draw < Draws; ++draw) copies[draw].bindings[0].guestDescriptor[0] += static_cast<std::uint32_t>(draw) * 256u;
    std::vector<std::uint32_t> key;
    key.reserve(1u << 16u);
    std::size_t words = 0;
    const auto oldStart = ThreadMicroseconds();
    for (const auto& copy : copies) {
        key.clear();
        ShaderResources::AppendContentKey(Stage(copy), true, true, key);
        words += key.size();
    }
    const auto oldUs = (ThreadMicroseconds() - oldStart) / Draws;
    const auto fullWords = words / Draws;
    words = 0;
    const auto newStart = ThreadMicroseconds();
    for (const auto& copy : copies) {
        key.clear();
        const auto stage = Stage(copy);
        const auto statics = AgcDriver::Graphics::StageStaticKeys(stage);
        key.insert(key.end(), statics->drawWords.begin(), statics->drawWords.end());
        for (const auto binding : copy.keyShape->patchedBindings) ShaderResources::AppendBindingContentKey(stage, binding, true, true, key);
        words += key.size();
    }
    const auto newUs = (ThreadMicroseconds() - newStart) / Draws;
    std::printf("bench %s: %zu-word key; full walk per fresh copy %.3f us, shared static key plus patched segments %.3f us (%zu words), thread CPU\n", name, fullWords, oldUs, newUs, words / Draws);
}

}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "--bench") {
            Bench("GTA-like (one 1-element patched buffer)", 0, 1);
            Bench("Village-like (2000-element static buffer table, 1-element patched buffer)", 2000, 1);
            Bench("Village-like worst case (2000-element patched buffer table)", 0, 2000);
            BenchIdentity();
            BenchPushShare("GTA-like (7 bindings)", 0);
            BenchPushShare("Village-like (2000-element buffer table)", 2000);
            return 0;
        }
        FirstFillRaces();
        CopiesShareTheStaticPart();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
