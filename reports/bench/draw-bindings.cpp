#define main graphicsTestsMain
#include "tests/Graphics.cpp"
#undef main

namespace {

VKAPI_ATTR void VKAPI_CALL benchUpdateDescriptorSets(VkDevice, std::uint32_t, const VkWriteDescriptorSet*, std::uint32_t, const VkCopyDescriptorSet*) {}

double runBench(bool samplers, bool both, int draws, std::uint32_t dataDwords) {
    using AgcDriver::Graphics::CompiledShader;
    using AgcDriver::Graphics::DescriptorCache;
    using AgcDriver::Graphics::Recorder;
    using AgcDriver::Graphics::ShaderResources;
    mock = MockVulkan{};
    mock.allowCopies = true;
    auto context = mockContext();
    context.limits.maxDescriptorSetStorageBuffersDynamic = 8;
    context.limits.minStorageBufferOffsetAlignment = 256;
    context.limits.maxSamplerAnisotropy = 1.0f;
    context.limits.maxPerStageDescriptorSamplers = 64;
    context.limits.maxDescriptorSetSamplers = 64;
    AgcDriver::Graphics::DeviceFunctions functions{};
    functions.updateDescriptorSets = benchUpdateDescriptorSets;
    context.functions = &functions;
    double perDraw = 0;
    {
        DescriptorCache cache(context);
        context.descriptorCache = &cache;
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        Recorder recorder(context);
        ShaderRecompiler::RecompileResult vertex, fragment;
        vertex.shaderDataDwords = dataDwords;
        const auto dataWords = [&](std::uint32_t value) {
            if (dataDwords == ShaderRecompiler::RuntimeAbi::ShaderDataDwords) return ShaderDataWords({value, 1, 2});
            std::vector<std::uint32_t> words(dataDwords, 0u);
            words[0] = value;
            return words;
        };
        auto buffers = makeBinding(Role::GuestBuffers, 0, 1, vsharp(guestFirst.data(), 16));
        buffers.bufferWritten = {false};
        vertex.bindings.push_back(buffers);
        vertex.bindings.push_back(makeBinding(Role::ShaderData, 5, 1, dataWords(7)));
        vertex.bindings.push_back(makeBinding(Role::FlattenedSrt, 6, 1, {1, 2, 3, 4}));
        if (samplers) {
            std::vector<std::uint32_t> words;
            for (int i = 0; i < 16; ++i) words.insert(words.end(), {0x92u, (4u * 256u) << 12u, 0u, 0u});
            auto sampler = makeBinding(Role::GuestSamplers, static_cast<std::uint32_t>(ShaderRecompiler::RuntimeAbi::Binding::Samplers), 16, words);
            sampler.kind = Kind::Sampler;
            sampler.samplerDepthCompare.assign(16, false);
            sampler.samplerUnnormalized.assign(16, false);
            sampler.samplerIntegerBorder.assign(16, false);
            sampler.samplerCoordinatesIgnored.assign(16, false);
            vertex.bindings.push_back(sampler);
        }
        const auto state = AgcDriver::Graphics::DecodeState(makeState());
        const std::array<CompiledShader, 2> stages{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &fragment, 0}}};
        ShaderResources resources(context, stages, state.color, 0, 0, 0);
        std::vector<ShaderRecompiler::RecompileResult> drawn(64, vertex);
        for (std::size_t i = 0; i < drawn.size(); ++i) {
            drawn[i].bindings[1].guestDescriptor = dataWords(static_cast<std::uint32_t>(i + 100));
            if (both) drawn[i].bindings[2].guestDescriptor = {static_cast<std::uint32_t>(i), 0, 0, 0};
        }
        std::chrono::nanoseconds total{0};
        for (int draw = 0; draw < draws; ++draw) {
            const std::array<CompiledShader, 2> drawStages{{{ShaderRecompiler::ShaderStage::Vertex, &drawn[static_cast<std::size_t>(draw) % drawn.size()], 0}, {ShaderRecompiler::ShaderStage::Fragment, &fragment, 0}}};
            const auto start = std::chrono::steady_clock::now();
            {
                const auto moved = resources.MovedReadOnlyBuffers(drawStages, recorder, &state.color);
                const auto bindings = resources.PrepareDrawBindings(recorder, *moved);
                if (bindings == nullptr) throw std::runtime_error("no bindings");
            }
            total += std::chrono::steady_clock::now() - start;
            if (draw % 256 == 255) {
                recorder.Sync();
                recorder.FlushDeferredReleases();
            }
        }
        recorder.Sync();
        recorder.FlushDeferredReleases();
        perDraw = static_cast<double>(total.count()) / 1000.0 / draws;
        const auto paths = ShaderResources::DrawBindingPathCounts();
        std::printf("  ring %llu, rewritten %llu, ring sets %llu\n", static_cast<unsigned long long>(paths.ring), static_cast<unsigned long long>(paths.rewritten), static_cast<unsigned long long>(paths.ringSets));
    }
    return perDraw;
}

}

int main(int argc, char** argv) {
    const bool samplers = argc > 1 && std::string_view(argv[1]) == "many";
    const bool both = argc > 2 && std::string_view(argv[2]) == "both";
    const int draws = 20000;
    const std::uint32_t dataDwords = argc > 3 ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10)) : ShaderRecompiler::RuntimeAbi::ShaderDataDwords;
    runBench(samplers, both, 2000, dataDwords);
    const auto us = runBench(samplers, both, draws, dataDwords);
    std::printf("shader data %u dwords, template %s, data changed %s, dynamic %s: %.2f us per draw (MovedReadOnlyBuffers + PrepareDrawBindings, descriptor updates stubbed)\n", dataDwords, samplers ? "many (16 samplers)" : "small", both ? "ShaderData+SRT" : "ShaderData only", std::getenv("APS5_NO_DYNAMIC_DRAW_DATA") ? "off" : "on", us);
}
