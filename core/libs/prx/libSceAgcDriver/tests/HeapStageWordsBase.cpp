#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/HeapStages.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "Recompiler.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using ShaderRecompiler::DescriptorRole;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

ShaderRecompiler::DescriptorBinding Binding(DescriptorRole role, std::vector<std::uint32_t> words) {
    ShaderRecompiler::DescriptorBinding binding{};
    binding.role = role;
    binding.count = 1;
    binding.guestDescriptor = std::move(words);
    return binding;
}

void TestTemplateWords() {
    ShaderRecompiler::RecompileResult program;
    program.bindings = {Binding(DescriptorRole::ShaderData, {1, 2, 3, 4}), Binding(DescriptorRole::GuestImages, {10, 11, 12, 13, 14, 15, 16, 17}),
                        Binding(DescriptorRole::FlattenedSrt, {20, 21}), Binding(DescriptorRole::GuestSamplers, {30, 31, 32, 33})};
    auto shared = std::make_shared<ShaderRecompiler::HeapStageWords>();
    shared->bindings = {{9, 9, 9, 9}, {40, 41, 42, 43, 44, 45, 46, 47}, {50, 51}, {}};
    ShaderRecompiler::HeapStageWords words;
    words.base = shared;
    words.userData = {7};
    words.bindings.resize(program.bindings.size());
    const auto& b = program.bindings;
    Check(AgcDriver::Graphics::DrawGuestWords(program, &words, b[1]) == shared->bindings[1], "a resource binding without per-draw words does not read its template's");
    Check(AgcDriver::Graphics::DrawGuestWords(program, &words, b[3]) == b[3].guestDescriptor, "a binding the template does not hold does not read the program's words");
    Check(AgcDriver::Graphics::DrawGuestWords(program, &words, b[0]) == b[0].guestDescriptor, "shader data fell back to the template");
    Check(AgcDriver::Graphics::DrawGuestWords(program, &words, b[2]) == b[2].guestDescriptor, "the flattened SRT fell back to the template");
    words.bindings[1] = {60, 61, 62, 63, 64, 65, 66, 67};
    Check(AgcDriver::Graphics::DrawGuestWords(program, &words, b[1]) == words.bindings[1], "per-draw words do not override the template's");
    words.bindings[1].clear();
    const auto patched = AgcDriver::DriverDetail::PatchedHeapResult(program, words);
    Check(patched->bindings[0].guestDescriptor == std::vector<std::uint32_t>({7, 2, 3, 4}), "the patched shader data does not take the draw's user data over the program's");
    Check(patched->bindings[1].guestDescriptor == shared->bindings[1], "the patched result does not take the template's resource words");
    Check(patched->bindings[2].guestDescriptor == b[2].guestDescriptor, "the patched flattened SRT took the template's words");
    Check(patched->bindings[3].guestDescriptor == b[3].guestDescriptor, "the patched result lost the program's words");
    shared->bindings[3] = {1};
    bool refused = false;
    try {
        static_cast<void>(AgcDriver::DriverDetail::PatchedHeapResult(program, words));
    } catch (const std::runtime_error&) {
        refused = true;
    }
    Check(refused, "template words of another size than the program's were taken");
}


using AgcDriver::Graphics::RuntimeSwitch;

ShaderRecompiler::RecompileResult KeyedProgram() {
    ShaderRecompiler::RecompileResult program;
    program.heapTemplateId = 0x1234'5678'9abcull;
    program.variantId = 0x77ull;
    program.bindings = {Binding(DescriptorRole::ShaderData, {1, 2, 3, 4}), Binding(DescriptorRole::GuestImages, {10, 11, 12, 13, 14, 15, 16, 17}),
                        Binding(DescriptorRole::GuestSamplers, {30, 31, 32, 33})};
    return program;
}

struct StageKey {
    std::vector<std::uint32_t> words;
    std::uint64_t built = 0;
};

StageKey KeyOf(const ShaderRecompiler::RecompileResult& program, const ShaderRecompiler::HeapStageWords& words,
               ShaderRecompiler::ShaderStage stage = ShaderRecompiler::ShaderStage::Fragment) {
    const AgcDriver::Graphics::CompiledShader shader{stage, &program, 0, nullptr, &words};
    StageKey key;
    const auto before = AgcDriver::Graphics::DrawContentKeysBuilt();
    AgcDriver::Graphics::AppendDrawStageKey(shader, key.words);
    key.built = AgcDriver::Graphics::DrawContentKeysBuilt() - before;
    return key;
}

void TestProgramKeyMemo() {
    AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoProgramHeapKeys, false);
    const auto program = KeyedProgram();
    ShaderRecompiler::HeapStageWords record;
    record.userData = {5};
    const auto first = KeyOf(program, record);
    Check(first.built == 1, "the first heap stage key was not built");
    AgcDriver::DriverDetail::ResetHeapStageWords(record);
    record.userData = {6};
    const auto recycled = KeyOf(program, record);
    Check(recycled.built == 0, "a recycled heap stage record rebuilt its key, so the key memo died with the record");
    Check(recycled.words == first.words, "a recycled heap stage record's key differs although its resource words did not change");
    ShaderRecompiler::HeapStageWords other;
    const auto fresh = KeyOf(program, other);
    Check(fresh.built == 0 && fresh.words == first.words, "another heap stage record of the same program did not reuse its key");
    const auto vertex = KeyOf(program, other, ShaderRecompiler::ShaderStage::Vertex);
    Check(vertex.built == 1 && vertex.words != first.words, "a key memoized for one stage served another");

    ShaderRecompiler::HeapStageWords imaged;
    imaged.bindings.resize(program.bindings.size());
    imaged.bindings[1] = {90, 91, 92, 93, 94, 95, 96, 97};
    const auto perDraw = KeyOf(program, imaged);
    Check(perDraw.built == 1 && perDraw.words != first.words, "a stage with per-draw image words took the program's key");
    imaged.bindings[1][0] = 98;
    const auto changed = KeyOf(program, imaged);
    Check(changed.built == 1 && changed.words != perDraw.words, "a stage's per-draw image words changed but its key did not");
    Check(imaged.drawKeyMemo.words.load() == nullptr, "a heap stage record stored a key memo that no later draw can read");

    AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoProgramHeapKeys, true);
    const auto program2 = KeyedProgram();
    ShaderRecompiler::HeapStageWords old;
    const auto offFirst = KeyOf(program2, old);
    AgcDriver::DriverDetail::ResetHeapStageWords(old);
    const auto offRecycled = KeyOf(program2, old);
    Check(offFirst.words == first.words && offRecycled.words == first.words, "the key differs with program heap keys switched off");
    Check(offRecycled.built == 1, "with program heap keys off a recycled record did not rebuild its key");
    const auto offImaged = KeyOf(program2, imaged);
    Check(offImaged.words == changed.words, "a per-draw image key differs with program heap keys switched off");
    AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoProgramHeapKeys, false);
}

int BenchmarkDrawKeys(std::size_t draws) {
    const auto program = KeyedProgram();
    std::vector<ShaderRecompiler::HeapStageWords> records(64);
    std::vector<std::uint32_t> key;
    key.reserve(64);
    for (const bool off : {false, true, true, false}) {
        AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoProgramHeapKeys, off);
        const auto before = AgcDriver::Graphics::DrawContentKeysBuilt();
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t draw = 0; draw < draws; ++draw) {
            auto& record = records[draw % records.size()];
            AgcDriver::DriverDetail::ResetHeapStageWords(record);
            key.clear();
            const AgcDriver::Graphics::CompiledShader shader{ShaderRecompiler::ShaderStage::Fragment, &program, 0, nullptr, &record};
            AgcDriver::Graphics::AppendDrawStageKey(shader, key);
        }
        const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        std::printf("draw stage keys, %s: %.1f ns/draw, %llu keys built over %zu draws\n", off ? "APS5_NO_PROGRAM_HEAP_KEYS" : "program memo", ns / static_cast<double>(draws),
                    static_cast<unsigned long long>(AgcDriver::Graphics::DrawContentKeysBuilt() - before), draws);
    }
    AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoProgramHeapKeys, false);
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::strcmp(argv[1], "--benchmark-draw-keys") == 0) return BenchmarkDrawKeys(std::stoull(argv[2]));
        TestTemplateWords();
        TestProgramKeyMemo();
        std::cout << "heap stage template word tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "heap stage template word tests failed: " << error.what() << '\n';
        return 1;
    }
}
