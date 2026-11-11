// agc_shader_replay <shader_*.req>...: replays compute recompile requests the driver saved with
// APS5_DUMP_SHADERS=1, running the same resource analysis and recompile steps without the game.
#include "ControlFlow/GraphBuilder.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "ControlFlow/Structurizer.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/RequestMemoryView.hpp"
#include "Recompiler.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"

#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp>

#if ANYPS5_ENABLE_SPIRV_TOOLS
#include "spirv-tools/libspirv.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <fstream>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

std::string ReadText(const char* path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream stream;
    stream << file.rdbuf();
    auto text = stream.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

// The error text without the serialized request that Recompile appends.
std::string FirstLine(const std::string& text) {
    const auto request = text.find("\nRecompileRequest:");
    auto shown = request == std::string::npos ? text : text.substr(0, request);
    if (shown.size() > 2000) shown.resize(2000);
    return shown;
}

bool g_disassemble = false;
bool g_assembly = false;
bool g_memory = false;
bool g_spirv = false;
bool g_prepare = false;
bool g_materialize = false;
bool g_graph = false;
bool g_maintenance8 = false;
bool g_robust2 = false;
bool g_waveLdsMemoryBarriers = false;
bool g_deviceScopeLdsMemoryBarriers = false;
bool g_ldsLockFences = false;
bool g_shaderFma = false;
bool g_shaderInt16 = false;
bool g_code = false;
bool g_float16WidenNans = false;
bool g_float16NarrowNans = false;
bool g_operations = false;
bool g_spannedWaves = false;
bool g_specializedFragments = false;

enum class OperationFamily { SpecialValueTests, FloatArithmetic, FloatCompares, Selects, BitManipulation, IntegerArithmetic, IntegerCompares, Logical, Conversions, Composites, Memory, Subgroup, Control, Other, Count };

constexpr std::array<const char*, static_cast<std::size_t>(OperationFamily::Count)> OperationFamilyNames{
    "NaN and infinity tests", "float arithmetic", "float compares", "selects", "bit manipulation", "integer arithmetic", "integer compares",
    "logical", "conversions", "composites", "memory and images", "subgroup", "control", "other",
};

OperationFamily ExtendedFamily(std::uint32_t instruction) {
    if (instruction >= GLSLstd450PackSnorm4x8 && instruction <= GLSLstd450UnpackDouble2x32) return OperationFamily::Conversions;
    switch (instruction) {
        case GLSLstd450SAbs:
        case GLSLstd450SSign:
        case GLSLstd450UMin:
        case GLSLstd450SMin:
        case GLSLstd450UMax:
        case GLSLstd450SMax:
        case GLSLstd450UClamp:
        case GLSLstd450SClamp:
        case GLSLstd450FindILsb:
        case GLSLstd450FindSMsb:
        case GLSLstd450FindUMsb:
            return OperationFamily::IntegerArithmetic;
        default:
            return OperationFamily::FloatArithmetic;
    }
}

OperationFamily IntegerCompareFamily(std::span<const std::uint32_t> operands, const std::unordered_set<std::uint32_t>& specialBounds) {
    for (const auto operand : operands) {
        if (specialBounds.contains(operand)) return OperationFamily::SpecialValueTests;
    }
    return OperationFamily::IntegerCompares;
}

OperationFamily Family(spv::Op opcode, std::span<const std::uint32_t> words, const std::unordered_set<std::uint32_t>& specialBounds) {
    switch (opcode) {
        case spv::OpIsNan:
        case spv::OpIsInf:
            return OperationFamily::SpecialValueTests;
        case spv::OpFAdd:
        case spv::OpFSub:
        case spv::OpFMul:
        case spv::OpFDiv:
        case spv::OpFRem:
        case spv::OpFMod:
        case spv::OpFNegate:
        case spv::OpDot:
        case spv::OpVectorTimesScalar:
        case spv::OpFmaKHR:
            return OperationFamily::FloatArithmetic;
        case spv::OpExtInst:
            return words.size() > 4u ? ExtendedFamily(words[4]) : OperationFamily::Other;
        case spv::OpFOrdEqual:
        case spv::OpFUnordEqual:
        case spv::OpFOrdNotEqual:
        case spv::OpFUnordNotEqual:
        case spv::OpFOrdLessThan:
        case spv::OpFUnordLessThan:
        case spv::OpFOrdGreaterThan:
        case spv::OpFUnordGreaterThan:
        case spv::OpFOrdLessThanEqual:
        case spv::OpFUnordLessThanEqual:
        case spv::OpFOrdGreaterThanEqual:
        case spv::OpFUnordGreaterThanEqual:
            return OperationFamily::FloatCompares;
        case spv::OpSelect:
            return OperationFamily::Selects;
        case spv::OpBitwiseAnd:
        case spv::OpBitwiseOr:
        case spv::OpBitwiseXor:
        case spv::OpNot:
        case spv::OpShiftLeftLogical:
        case spv::OpShiftRightLogical:
        case spv::OpShiftRightArithmetic:
        case spv::OpBitFieldInsert:
        case spv::OpBitFieldSExtract:
        case spv::OpBitFieldUExtract:
        case spv::OpBitReverse:
        case spv::OpBitCount:
            return OperationFamily::BitManipulation;
        case spv::OpIAdd:
        case spv::OpISub:
        case spv::OpIMul:
        case spv::OpUDiv:
        case spv::OpSDiv:
        case spv::OpUMod:
        case spv::OpSRem:
        case spv::OpSMod:
        case spv::OpSNegate:
        case spv::OpIAddCarry:
        case spv::OpISubBorrow:
        case spv::OpUMulExtended:
        case spv::OpSMulExtended:
            return OperationFamily::IntegerArithmetic;
        case spv::OpIEqual:
        case spv::OpINotEqual:
        case spv::OpUGreaterThan:
        case spv::OpUGreaterThanEqual:
        case spv::OpULessThan:
        case spv::OpULessThanEqual:
            return words.size() > 3u ? IntegerCompareFamily(words.subspan(3u), specialBounds) : OperationFamily::IntegerCompares;
        case spv::OpSGreaterThan:
        case spv::OpSGreaterThanEqual:
        case spv::OpSLessThan:
        case spv::OpSLessThanEqual:
            return OperationFamily::IntegerCompares;
        case spv::OpLogicalAnd:
        case spv::OpLogicalOr:
        case spv::OpLogicalNot:
        case spv::OpLogicalEqual:
        case spv::OpLogicalNotEqual:
        case spv::OpAny:
        case spv::OpAll:
            return OperationFamily::Logical;
        case spv::OpBitcast:
        case spv::OpFConvert:
        case spv::OpUConvert:
        case spv::OpSConvert:
        case spv::OpConvertFToU:
        case spv::OpConvertFToS:
        case spv::OpConvertUToF:
        case spv::OpConvertSToF:
        case spv::OpConvertPtrToU:
        case spv::OpConvertUToPtr:
        case spv::OpQuantizeToF16:
            return OperationFamily::Conversions;
        case spv::OpCompositeConstruct:
        case spv::OpCompositeExtract:
        case spv::OpCompositeInsert:
        case spv::OpVectorShuffle:
        case spv::OpVectorExtractDynamic:
        case spv::OpVectorInsertDynamic:
        case spv::OpCopyObject:
            return OperationFamily::Composites;
        case spv::OpLoad:
        case spv::OpStore:
        case spv::OpAccessChain:
        case spv::OpInBoundsAccessChain:
        case spv::OpPtrAccessChain:
        case spv::OpArrayLength:
        case spv::OpSampledImage:
        case spv::OpImage:
        case spv::OpImageQuerySize:
        case spv::OpImageQuerySizeLod:
        case spv::OpImageQueryLevels:
        case spv::OpImageQueryLod:
        case spv::OpImageRead:
        case spv::OpImageWrite:
        case spv::OpImageFetch:
        case spv::OpImageGather:
        case spv::OpImageDrefGather:
        case spv::OpImageSampleImplicitLod:
        case spv::OpImageSampleExplicitLod:
        case spv::OpImageSampleDrefImplicitLod:
        case spv::OpImageSampleDrefExplicitLod:
        case spv::OpImageTexelPointer:
            return OperationFamily::Memory;
        case spv::OpPhi:
        case spv::OpFunctionCall:
        case spv::OpControlBarrier:
        case spv::OpMemoryBarrier:
        case spv::OpKill:
        case spv::OpTerminateInvocation:
        case spv::OpDemoteToHelperInvocation:
        case spv::OpUnreachable:
            return OperationFamily::Control;
        default:
            if (opcode >= spv::OpAtomicLoad && opcode <= spv::OpAtomicXor) return OperationFamily::Memory;
            if (opcode >= spv::OpGroupNonUniformElect && opcode <= spv::OpGroupNonUniformQuadSwap) return OperationFamily::Subgroup;
            return OperationFamily::Other;
    }
}

bool Structural(spv::Op opcode) {
    switch (opcode) {
        case spv::OpLabel:
        case spv::OpBranch:
        case spv::OpBranchConditional:
        case spv::OpSwitch:
        case spv::OpSelectionMerge:
        case spv::OpLoopMerge:
        case spv::OpFunction:
        case spv::OpFunctionParameter:
        case spv::OpFunctionEnd:
        case spv::OpReturn:
        case spv::OpReturnValue:
        case spv::OpVariable:
        case spv::OpLine:
        case spv::OpNoLine:
            return true;
        default:
            return false;
    }
}

bool PrintOperations(std::span<const std::uint32_t> spirv) {
    if (spirv.size() < 5u || spirv[0] != spv::MagicNumber) {
        std::printf("  ops: not a SPIR-V module\n");
        return false;
    }
    std::unordered_set<std::uint32_t> specialBounds;
    std::array<std::size_t, static_cast<std::size_t>(OperationFamily::Count)> counts{};
    std::size_t total = 0u;
    bool inFunction = false;
    for (std::size_t offset = 5u; offset < spirv.size();) {
        const auto count = spirv[offset] >> spv::WordCountShift;
        if (count == 0u || count > spirv.size() - offset) {
            std::printf("  ops: malformed instruction at word %zu\n", offset);
            return false;
        }
        const auto opcode = static_cast<spv::Op>(spirv[offset] & spv::OpCodeMask);
        const auto words = spirv.subspan(offset, count);
        offset += count;
        if (opcode == spv::OpConstant && count == 4u && (words[3] == 0x7c00u || words[3] == 0x7f800000u)) specialBounds.insert(words[2]);
        if (opcode == spv::OpFunction) inFunction = true;
        if (!inFunction || Structural(opcode)) continue;
        ++counts[static_cast<std::size_t>(Family(opcode, words, specialBounds))];
        ++total;
    }
    std::vector<std::pair<std::size_t, std::size_t>> ranked;
    for (std::size_t family = 0; family < counts.size(); ++family) {
        if (counts[family] != 0u) ranked.emplace_back(counts[family], family);
    }
    std::ranges::sort(ranked, [](const auto& lhs, const auto& rhs) { return lhs.first != rhs.first ? lhs.first > rhs.first : lhs.second < rhs.second; });
    std::printf("  ops: %zu instructions\n", total);
    for (const auto& [count, family] : ranked) std::printf("  ops %s: %zu (%.1f%%)\n", OperationFamilyNames[family], count, 100.0 * static_cast<double>(count) / static_cast<double>(total));
    return true;
}

bool OutputSpirv(const char* path, std::span<const std::uint32_t> spirv) {
    if (g_operations && !PrintOperations(spirv)) return false;
    if (g_spirv) {
        std::string name(path);
        name = name.substr(name.find_last_of("/\\") + 1) + ".spv";
        std::ofstream file(name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size_bytes()));
        file.close();
        if (!file) {
            std::printf("  could not write %s\n", name.c_str());
            return false;
        }
    }
#if ANYPS5_ENABLE_SPIRV_TOOLS
    if (g_disassemble) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
        std::string text;
        if (!tools.Disassemble(spirv.data(), spirv.size(), &text, SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES)) {
            std::printf("  could not disassemble SPIR-V\n");
            return false;
        }
        std::printf("%s\n", text.c_str());
    }
#else
    if (g_disassemble) std::printf("  (--dis needs ANYPS5_ENABLE_SPIRV_TOOLS=ON)\n");
#endif
    return true;
}

bool Replay(const char* path) {
    auto request = ShaderRecompiler::RequestSerializer{}.Deserialize(ReadText(path));
    if (g_maintenance8) request.request.target.nonConstantImageOffsets = true;
    if (g_robust2) request.request.target.robustBufferAccess2 = true;
    if (g_waveLdsMemoryBarriers) request.request.target.waveLdsMemoryBarriers = true;
    if (g_deviceScopeLdsMemoryBarriers) request.request.target.deviceScopeLdsMemoryBarriers = true;
    if (g_ldsLockFences) request.request.target.ldsLockFences = true;
    if (g_shaderFma) {
        request.supportedCapabilities.push_back(spv::CapabilityFMAKHR);
        request.supportedExtensions.push_back("SPV_KHR_fma");
        request.request.target.supportedCapabilities = request.supportedCapabilities;
        request.request.target.supportedExtensions = request.supportedExtensions;
    }
    if (g_shaderInt16) {
        request.supportedCapabilities.push_back(spv::CapabilityInt16);
        request.request.target.supportedCapabilities = request.supportedCapabilities;
    }
    if (g_float16WidenNans) request.request.target.float16WidenNans = true;
    if (g_float16NarrowNans) request.request.target.float16NarrowNans = true;
    if (g_spannedWaves) request.request.target.spannedWaves = true;
    if (g_specializedFragments) request.request.target.registrationWordLimit = 0u;
    std::printf("%s: %zu code words, %zu user data, %zu memory regions, wave%u\n", path, request.request.shader.code.size(), request.request.context.userData.size(), request.request.context.memory.size(), request.request.context.waveSize);
    if (request.request.context.compute.has_value()) {
        const auto& compute = *request.request.context.compute;
        std::printf("  compute: threads %ux%ux%u, lds %u dwords, group ids %d%d%d, tg size %d, thread id components %u\n", compute.numThreads[0], compute.numThreads[1], compute.numThreads[2], compute.ldsSizeDwords, compute.groupIdEnable[0], compute.groupIdEnable[1], compute.groupIdEnable[2], compute.tgSizeEnable, compute.threadIdComponentCount);
        if (compute.PartialGroups()) std::printf("  partial groups: dispatch of %ux%ux%u threads\n", compute.partialThreads[0], compute.partialThreads[1], compute.partialThreads[2]);
    }
    if (request.request.graphics.has_value()) {
        const auto& graphics = *request.request.graphics;
        std::printf("  graphics: %zu linked programs, draw %u indices of %u bytes at 0x%llx, %u instances\n", graphics.linkedPrograms.size(), graphics.draw.indexCount, graphics.draw.indexElementBytes, static_cast<unsigned long long>(graphics.draw.indexAddress), graphics.draw.instanceCount);
        if (graphics.mesh.has_value()) {
            const auto& mesh = *graphics.mesh;
            std::printf("  mesh: input primitive %u, %u primitives / %u vertices per group, max %u vertices / %u primitives, %u threads, lds %u dwords, provoking %u, ESGS item %u\n", mesh.inputPrimitive, mesh.primitivesPerGroup, mesh.verticesPerGroup, mesh.maxVertices, mesh.maxPrimitives, mesh.threadsPerGroup, mesh.ldsSizeDwords, mesh.provokingVertex, mesh.esgsItemSize);
        }
    }
    if (g_memory) {
        // --mem: the captured inputs. User data words are printed; each memory region is written to
        // mem_<code address>_<guest address>.bin next to the request for inspection with other tools.
        const auto& context = request.request.context;
        for (std::size_t i = 0; i < context.userData.size(); ++i) std::printf("  user[%zu] = 0x%08x\n", i, context.userData[i]);
        if (request.request.graphics.has_value()) {
            const auto& linked = request.request.graphics->linkedPrograms;
            for (std::size_t index = 0; index < linked.size(); ++index) {
                std::printf("  linked program %zu (stage %u, 0x%llx): user data base register %u, first user SGPR %u\n", index, static_cast<unsigned>(linked[index].binary.stage), static_cast<unsigned long long>(linked[index].binary.codeAddress), linked[index].userDataBaseRegister, linked[index].firstUserSgpr);
                for (std::size_t i = 0; i < linked[index].userData.size(); ++i) std::printf("    user[%zu] = 0x%08x\n", i, linked[index].userData[i]);
            }
        }
        for (const auto& region : context.memory) {
            char name[96];
            std::snprintf(name, sizeof(name), "mem_%llx_%llx.bin", static_cast<unsigned long long>(request.request.shader.codeAddress), static_cast<unsigned long long>(region.guestAddress));
            if (std::FILE* file = std::fopen(name, "wb")) {
                std::fwrite(region.bytes.data(), 1, region.bytes.size(), file);
                std::fclose(file);
            }
            std::printf("  region 0x%llx + 0x%zx -> %s\n", static_cast<unsigned long long>(region.guestAddress), region.bytes.size(), name);
        }
    }
    if (g_code) {
        std::string name(path);
        name = name.substr(name.find_last_of("/\\") + 1) + ".code";
        const auto& code = request.request.shader.code;
        std::ofstream file(name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(code.data()), static_cast<std::streamsize>(code.size() * sizeof(code[0])));
        if (!file) {
            std::printf("  could not write %s\n", name.c_str());
            return false;
        }
        std::printf("  code -> %s\n", name.c_str());
        if (request.request.graphics.has_value()) {
            const auto& linked = request.request.graphics->linkedPrograms;
            for (std::size_t index = 0; index < linked.size(); ++index) {
                const auto linkedName = name + ".linked" + std::to_string(index);
                const auto& linkedCode = linked[index].binary.code;
                std::ofstream linkedFile(linkedName, std::ios::binary);
                linkedFile.write(reinterpret_cast<const char*>(linkedCode.data()), static_cast<std::streamsize>(linkedCode.size() * sizeof(linkedCode[0])));
                if (!linkedFile) {
                    std::printf("  could not write %s\n", linkedName.c_str());
                    return false;
                }
                std::printf("  linked program %zu (stage %u, 0x%llx) -> %s\n", index, static_cast<unsigned>(linked[index].binary.stage), static_cast<unsigned long long>(linked[index].binary.codeAddress), linkedName.c_str());
            }
        }
    }
    if (g_assembly) {
        const auto program = ShaderRecompiler::RdnaInstructionDecoder{}.Decode(request.request.shader.code);
        // Raw first words carry what the text omits (branch offsets, waitcnt fields).
        for (const auto& instruction : program.instructions) std::printf("raw=%08x %s\n", instruction.rawWords[0], ShaderRecompiler::RdnaInstructionToString(instruction).c_str());
    }
    if (g_graph) {
        const auto program = ShaderRecompiler::RdnaInstructionDecoder{}.Decode(request.request.shader.code);
        auto graph = ShaderRecompiler::GraphBuilder{}.Build(program);
        std::printf("control flow graph:\n%s", ShaderRecompiler::GraphToString(graph).c_str());
        try {
            ShaderRecompiler::Structurizer{}.Structurize(graph);
            std::printf("structured control flow graph:\n%s", ShaderRecompiler::GraphToString(graph).c_str());
        } catch (const std::exception& error) {
            std::printf("  structurization failed: %s\n", FirstLine(error.what()).c_str());
            return false;
        }
    }
    if (!g_materialize) {
        try {
            auto program = ShaderRecompiler::PrepareResourceProgram(request.request);
            constexpr ShaderRecompiler::ResourceMaterializer materializer;
            static_cast<void>(materializer.ExtractPlan(program));
        } catch (const std::exception& error) {
            std::printf("  resource analysis failed: %s\n", FirstLine(error.what()).c_str());
            return false;
        }
    }
    if (g_prepare || g_materialize) {
        const char* phase = "prepare";
        try {
            const auto handle = ShaderRecompiler::PrepareShader(request.request);
            std::printf("  prepared: %zu SPIR-V words\n", ShaderRecompiler::GetPreparedArtifact(*handle).spirv.size());
            if (g_materialize) {
                phase = "prepared invocation";
                const auto invocation = ShaderRecompiler::PreparedShaderInvocation::TryCreate(request.request, handle);
                if (!invocation.has_value()) throw std::runtime_error("request does not match its prepared shader");
                ShaderRecompiler::RequestMemoryView memory(request.request.context.memory);
                const auto runtime = memory.MakeRuntime(request.request.context.userData, request.request.shader.codeAddress);
                phase = "capture";
                const auto capture = invocation->Capture(runtime);
                phase = "materialize";
                const auto result = invocation->Materialize(*capture);
                std::printf("  materialized: %zu SPIR-V words\n", result->spirv.size());
                return OutputSpirv(path, result->spirv);
            }
            return OutputSpirv(path, ShaderRecompiler::GetPreparedArtifact(*handle).spirv);
        } catch (const std::exception& error) {
            std::printf("  %s failed: %s\n", phase, FirstLine(error.what()).c_str());
            return false;
        }
    }
    try {
        const auto result = ShaderRecompiler::Recompile(request.request);
        std::printf("  recompiled: %zu SPIR-V words\n", result.spirv.size());
        if (g_memory) {
            for (const auto& binding : result.bindings) {
                if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
                for (std::size_t i = 0; i + 4 <= binding.guestDescriptor.size(); i += 4) {
                    const auto* v = binding.guestDescriptor.data() + i;
                    const bool written = i / 4 >= binding.bufferWritten.size() || binding.bufferWritten[i / 4];
                    std::printf("  buffer %zu: V# %08x %08x %08x %08x%s\n", i / 4, v[0], v[1], v[2], v[3], written ? " written" : "");
                }
            }
        }
        return OutputSpirv(path, result.spirv);
    } catch (const std::exception& error) {
        std::printf("  recompile failed: %s\n", FirstLine(error.what()).c_str());
        return false;
    }
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: agc_shader_replay [--dis] [--asm] [--cfg] [--mem] [--spv] [--prepare] [--materialize] [--code] [--maintenance8] [--robust2] [--wave-lds-memory-barriers] [--device-scope-lds-memory-barriers] [--lds-lock-fences] [--shader-fma] [--shader-int16] [--f16-widen-nans] [--f16-narrow-nans] [--spanned-waves] [--ops] [--specialized-fragments] <shader.req>...\n  the driver writes shader_<address>.req files when APS5_DUMP_SHADERS is set; shader_<address>.prepare.req holds a request whose preparation failed before any resource capture, so replay it with --prepare\n");
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--gcn") {
            ShaderRecompiler::SetGcnGuestShaders(true);
            continue;
        }
        if (std::string(argv[i]) == "--dis") {
            g_disassemble = true;
            continue;
        }
        if (std::string(argv[i]) == "--asm") {
            g_assembly = true;
            continue;
        }
        if (std::string(argv[i]) == "--cfg") {
            g_graph = true;
            continue;
        }
        if (std::string(argv[i]) == "--maintenance8") {
            g_maintenance8 = true;
            continue;
        }
        if (std::string(argv[i]) == "--robust2") {
            g_robust2 = true;
            continue;
        }
        if (std::string(argv[i]) == "--wave-lds-memory-barriers") {
            g_waveLdsMemoryBarriers = true;
            continue;
        }
        if (std::string(argv[i]) == "--device-scope-lds-memory-barriers") {
            g_deviceScopeLdsMemoryBarriers = true;
            continue;
        }
        if (std::string(argv[i]) == "--lds-lock-fences") {
            g_ldsLockFences = true;
            continue;
        }
        if (std::string(argv[i]) == "--shader-fma") {
            g_shaderFma = true;
            continue;
        }
        if (std::string(argv[i]) == "--shader-int16") {
            g_shaderInt16 = true;
            continue;
        }
        if (std::string(argv[i]) == "--f16-widen-nans") {
            g_float16WidenNans = true;
            continue;
        }
        if (std::string(argv[i]) == "--f16-narrow-nans") {
            g_float16NarrowNans = true;
            continue;
        }
        if (std::string(argv[i]) == "--mem") {
            g_memory = true;
            continue;
        }
        if (std::string(argv[i]) == "--code") {
            g_code = true;
            continue;
        }
        if (std::string(argv[i]) == "--prepare") {
            g_prepare = true;
            continue;
        }
        if (std::string(argv[i]) == "--materialize") {
            g_materialize = true;
            continue;
        }
        if (std::string(argv[i]) == "--spanned-waves") {
            g_spannedWaves = true;
            continue;
        }
        if (std::string(argv[i]) == "--specialized-fragments") {
            g_specializedFragments = true;
            continue;
        }
        if (std::string(argv[i]) == "--ops") {
            g_operations = true;
            continue;
        }
        if (std::string(argv[i]) == "--spv") {
            g_spirv = true;
            continue;
        }
        try {
            if (!Replay(argv[i])) ++failures;
        } catch (const std::exception& error) {
            std::printf("%s: could not load request: %s\n", argv[i], FirstLine(error.what()).c_str());
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
