#include "SpirvBackend/SpirvValueNumbering.hpp"
#include "SpirvInstructionOperands.hpp"
#include <spirv/unified1/GLSL.std.450.h>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace ShaderRecompiler {

namespace {

using SpirvInstructions::Opcode;
using SpirvInstructions::VisitIdOperands;

bool Pure(spv::Op op) {
    switch (op) {
    case spv::OpCopyObject:
    case spv::OpIAdd:
    case spv::OpISub:
    case spv::OpIMul:
    case spv::OpUDiv:
    case spv::OpSDiv:
    case spv::OpUMod:
    case spv::OpSRem:
    case spv::OpSMod:
    case spv::OpSNegate:
    case spv::OpFAdd:
    case spv::OpFSub:
    case spv::OpFMul:
    case spv::OpFDiv:
    case spv::OpFRem:
    case spv::OpFMod:
    case spv::OpFNegate:
    case spv::OpShiftLeftLogical:
    case spv::OpShiftRightLogical:
    case spv::OpShiftRightArithmetic:
    case spv::OpBitwiseAnd:
    case spv::OpBitwiseOr:
    case spv::OpBitwiseXor:
    case spv::OpNot:
    case spv::OpBitFieldInsert:
    case spv::OpBitFieldSExtract:
    case spv::OpBitFieldUExtract:
    case spv::OpBitReverse:
    case spv::OpBitCount:
    case spv::OpBitcast:
    case spv::OpConvertFToU:
    case spv::OpConvertFToS:
    case spv::OpConvertSToF:
    case spv::OpConvertUToF:
    case spv::OpUConvert:
    case spv::OpSConvert:
    case spv::OpFConvert:
    case spv::OpIEqual:
    case spv::OpINotEqual:
    case spv::OpUGreaterThan:
    case spv::OpSGreaterThan:
    case spv::OpUGreaterThanEqual:
    case spv::OpSGreaterThanEqual:
    case spv::OpULessThan:
    case spv::OpSLessThan:
    case spv::OpULessThanEqual:
    case spv::OpSLessThanEqual:
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
    case spv::OpIsNan:
    case spv::OpIsInf:
    case spv::OpLogicalEqual:
    case spv::OpLogicalNotEqual:
    case spv::OpLogicalOr:
    case spv::OpLogicalAnd:
    case spv::OpLogicalNot:
    case spv::OpSelect:
    case spv::OpCompositeConstruct:
    case spv::OpCompositeExtract:
    case spv::OpCompositeInsert:
    case spv::OpVectorShuffle:
    case spv::OpVectorExtractDynamic:
    case spv::OpVectorInsertDynamic:
    case spv::OpAccessChain:
    case spv::OpInBoundsAccessChain:
        return true;
    default:
        return false;
    }
}

bool PureExtendedInstruction(std::uint32_t instruction) {
    return instruction != GLSLstd450InterpolateAtCentroid && instruction != GLSLstd450InterpolateAtSample && instruction != GLSLstd450InterpolateAtOffset;
}

struct KeyHash {
    std::size_t operator()(const std::vector<std::uint32_t>& key) const {
        std::uint64_t hash = 14695981039346656037ull;
        for (const auto word : key) {
            hash ^= word;
            hash *= 1099511628211ull;
        }
        return static_cast<std::size_t>(hash);
    }
};

class ValueNumbering {
public:
    explicit ValueNumbering(std::span<const std::uint32_t> words) : words(words) {
        if (words.size() < 5u || words[0] != spv::MagicNumber) throw std::runtime_error("SPIR-V value numbering: invalid module");
        bound = words[3];
        alias.resize(bound);
        for (std::uint32_t id = 0; id < bound; ++id) alias[id] = id;
        resultType.assign(bound, 0u);
        definition.assign(bound, NoDefinition);
        integerWidth.assign(bound, 0u);
        pointerStorage.assign(bound, ~0u);
        decorated.assign(bound, 0u);
    }

    std::vector<std::uint32_t> Run() {
        scan();
        if (removed.empty()) return {words.begin(), words.end()};
        return rewrite();
    }

private:
    static constexpr std::size_t NoDefinition = ~std::size_t{0};

    std::span<const std::uint32_t> instructionAt(std::size_t cursor) const {
        const auto count = words[cursor] >> 16u;
        if (count == 0u || count > words.size() - cursor) throw std::runtime_error("SPIR-V value numbering: truncated instruction");
        return words.subspan(cursor, count);
    }

    std::uint32_t canonical(std::uint32_t id) const {
        return id < bound ? alias[id] : id;
    }

    std::uint32_t switchLiteralWords(std::span<const std::uint32_t> instruction) const {
        if (Opcode(instruction) != spv::OpSwitch || instruction.size() < 2u || instruction[1] >= bound) return 1u;
        const auto type = resultType[instruction[1]];
        return type < bound && integerWidth[type] == 64u ? 2u : 1u;
    }

    struct Block {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::vector<std::uint32_t> successors;
    };

    using Table = std::unordered_map<std::vector<std::uint32_t>, std::uint32_t, KeyHash>;

    static bool Terminator(spv::Op op) {
        switch (op) {
        case spv::OpBranch:
        case spv::OpBranchConditional:
        case spv::OpSwitch:
        case spv::OpReturn:
        case spv::OpReturnValue:
        case spv::OpKill:
        case spv::OpUnreachable:
        case spv::OpTerminateInvocation:
        case spv::OpEmitMeshTasksEXT:
            return true;
        default:
            return false;
        }
    }

    std::vector<std::uint32_t> successorsOf(std::span<const std::uint32_t> instruction) const {
        switch (Opcode(instruction)) {
        case spv::OpBranch:
            return {instruction[1]};
        case spv::OpBranchConditional:
            return {instruction[2], instruction[3]};
        case spv::OpSwitch: {
            std::vector<std::uint32_t> targets{instruction[2]};
            const auto literalWords = switchLiteralWords(instruction);
            for (std::size_t index = 3u + literalWords; index < instruction.size(); index += literalWords + 1u) targets.push_back(instruction[index]);
            return targets;
        }
        default:
            return {};
        }
    }

    static bool GlslImport(std::span<const std::uint32_t> instruction) {
        std::string name;
        for (std::size_t index = 2; index < instruction.size(); ++index) {
            for (std::uint32_t byte = 0; byte < 4u; ++byte) {
                const auto character = static_cast<char>((instruction[index] >> (byte * 8u)) & 0xffu);
                if (character == '\0') return name == "GLSL.std.450";
                name.push_back(character);
            }
        }
        return false;
    }

    void scan() {
        std::vector<std::vector<Block>> functions;
        for (std::size_t cursor = 5; cursor < words.size();) {
            const auto instruction = instructionAt(cursor);
            const auto op = Opcode(instruction);
            bool hasResult = false;
            bool hasType = false;
            spv::HasResultAndType(op, &hasResult, &hasType);
            const auto result = hasResult ? instruction[hasType ? 2u : 1u] : 0u;
            if (hasResult && result >= bound) throw std::runtime_error("SPIR-V value numbering: id exceeds the module bound");
            if (hasResult) {
                definition[result] = cursor;
                if (hasType) resultType[result] = instruction[1];
            }
            if (op == spv::OpTypeInt && instruction.size() >= 3u) integerWidth[result] = instruction[2];
            if (op == spv::OpTypePointer && instruction.size() >= 4u) pointerStorage[result] = instruction[2];
            if (op == spv::OpDecorate && instruction.size() >= 3u && instruction[2] == spv::DecorationBufferBlock) bufferBlocks = true;
            if (op == spv::OpExtInstImport && GlslImport(instruction)) glslImports.push_back(result);
            if ((op == spv::OpDecorate || op == spv::OpDecorateId || op == spv::OpDecorateString) && instruction.size() >= 2u && instruction[1] < bound) {
                decorated[instruction[1]] = 1u;
                decorationsOf[instruction[1]].insert(decorationsOf[instruction[1]].end(), instruction.begin() + 2, instruction.end());
                decorationsOf[instruction[1]].push_back(~0u);
            }
            if (op == spv::OpFunction) functions.emplace_back();
            if (op == spv::OpLabel) {
                if (functions.empty()) throw std::runtime_error("SPIR-V value numbering: a label outside a function");
                functions.back().push_back({cursor, 0u, {}});
            }
            if (Terminator(op) && !functions.empty() && !functions.back().empty()) {
                functions.back().back().end = cursor + instruction.size();
                functions.back().back().successors = successorsOf(instruction);
            }
            cursor += instruction.size();
        }
        for (const auto& blocks : functions) numberFunction(blocks);
    }

    void numberFunction(const std::vector<Block>& blocks) {
        if (blocks.empty()) return;
        std::unordered_map<std::uint32_t, std::size_t> indexOf;
        for (std::size_t index = 0; index < blocks.size(); ++index) indexOf.emplace(words[blocks[index].begin + 1u], index);
        const auto count = blocks.size();
        std::vector<std::vector<std::size_t>> successors(count);
        std::vector<std::vector<std::size_t>> predecessors(count);
        for (std::size_t index = 0; index < count; ++index) {
            if (blocks[index].end == 0u) throw std::runtime_error("SPIR-V value numbering: a block without a terminator");
            for (const auto label : blocks[index].successors) {
                const auto found = indexOf.find(label);
                if (found == indexOf.end()) throw std::runtime_error("SPIR-V value numbering: a branch to an unknown block");
                successors[index].push_back(found->second);
                predecessors[found->second].push_back(index);
            }
        }
        constexpr auto Unvisited = ~std::size_t{0};
        std::vector<std::size_t> postorder;
        std::vector<std::size_t> order(count, Unvisited);
        {
            std::vector<std::uint8_t> seen(count, 0u);
            std::vector<std::pair<std::size_t, std::size_t>> stack{{0u, 0u}};
            seen[0] = 1u;
            while (!stack.empty()) {
                auto& [node, next] = stack.back();
                if (next < successors[node].size()) {
                    const auto successor = successors[node][next++];
                    if (seen[successor] == 0u) {
                        seen[successor] = 1u;
                        stack.emplace_back(successor, 0u);
                    }
                } else {
                    order[node] = postorder.size();
                    postorder.push_back(node);
                    stack.pop_back();
                }
            }
        }
        std::vector<std::size_t> dominator(count, Unvisited);
        dominator[0] = 0u;
        for (bool changed = true; changed;) {
            changed = false;
            for (auto position = postorder.size(); position-- > 0u;) {
                const auto node = postorder[position];
                if (node == 0u) continue;
                auto chosen = Unvisited;
                for (const auto predecessor : predecessors[node]) {
                    if (dominator[predecessor] == Unvisited) continue;
                    if (chosen == Unvisited) {
                        chosen = predecessor;
                        continue;
                    }
                    auto left = predecessor;
                    auto right = chosen;
                    while (left != right) {
                        while (order[left] < order[right]) left = dominator[left];
                        while (order[right] < order[left]) right = dominator[right];
                    }
                    chosen = left;
                }
                if (chosen != Unvisited && dominator[node] != chosen) {
                    dominator[node] = chosen;
                    changed = true;
                }
            }
        }
        std::vector<std::vector<std::size_t>> children(count);
        for (std::size_t node = 1; node < count; ++node) {
            if (dominator[node] != Unvisited) children[dominator[node]].push_back(node);
        }
        Table table;
        std::vector<std::vector<std::uint32_t>> inserted;
        std::vector<std::uint32_t> key;
        std::vector<std::pair<std::size_t, std::size_t>> stack{{0u, 0u}};
        std::vector<std::size_t> marks{0u};
        numberBlock(blocks[0], table, inserted, key);
        while (!stack.empty()) {
            auto& [node, next] = stack.back();
            if (next < children[node].size()) {
                const auto child = children[node][next++];
                marks.push_back(inserted.size());
                numberBlock(blocks[child], table, inserted, key);
                stack.emplace_back(child, 0u);
            } else {
                while (inserted.size() > marks.back()) {
                    table.erase(inserted.back());
                    inserted.pop_back();
                }
                marks.pop_back();
                stack.pop_back();
            }
        }
    }

    void numberBlock(const Block& block, Table& table, std::vector<std::vector<std::uint32_t>>& inserted, std::vector<std::uint32_t>& key) {
        for (std::size_t cursor = block.begin; cursor < block.end;) {
            const auto instruction = instructionAt(cursor);
            cursor += instruction.size();
            bool hasResult = false;
            bool hasType = false;
            spv::HasResultAndType(Opcode(instruction), &hasResult, &hasType);
            if (!hasResult || !hasType || !numberable(instruction)) continue;
            if (number(instruction, instruction[2], table, key)) inserted.push_back(key);
        }
    }

    bool ReadOnlyPointer(std::uint32_t pointer) const {
        if (pointer >= bound || resultType[pointer] >= bound) return false;
        switch (pointerStorage[resultType[pointer]]) {
        case spv::StorageClassPushConstant:
        case spv::StorageClassUniformConstant:
        case spv::StorageClassInput:
            return true;
        case spv::StorageClassUniform:
            return !bufferBlocks;
        default:
            return false;
        }
    }

    bool numberable(std::span<const std::uint32_t> instruction) const {
        const auto op = Opcode(instruction);
        if (op == spv::OpExtInst) return instruction.size() >= 5u && std::ranges::find(glslImports, instruction[3]) != glslImports.end() && PureExtendedInstruction(instruction[4]);
        if (op == spv::OpLoad) return instruction.size() >= 4u && ReadOnlyPointer(instruction[3]) && (instruction.size() == 4u || (instruction[4] & static_cast<std::uint32_t>(spv::MemoryAccessVolatileMask)) == 0u);
        return Pure(op);
    }

    bool number(std::span<const std::uint32_t> instruction, std::uint32_t result, Table& table, std::vector<std::uint32_t>& key) {
        const auto op = Opcode(instruction);
        if (decorated[result] == 0u) {
            if (op == spv::OpCopyObject && instruction.size() == 4u) {
                alias[result] = canonical(instruction[3]);
                removed.push_back(result);
                return false;
            }
            if (op == spv::OpCompositeExtract && instruction.size() == 5u) {
                const auto composite = canonical(instruction[3]);
                const auto where = composite < bound ? definition[composite] : NoDefinition;
                if (where != NoDefinition) {
                    const auto constructed = instructionAt(where);
                    const auto part = instruction[4];
                    if (Opcode(constructed) == spv::OpCompositeConstruct && 3u + part < constructed.size()) {
                        const auto constituent = canonical(constructed[3u + part]);
                        const auto parts = constructed.size() - 3u;
                        bool scalarParts = true;
                        for (std::size_t index = 3; index < constructed.size(); ++index) scalarParts = scalarParts && constructed[index] < bound && resultType[constructed[index]] == instruction[1];
                        if (scalarParts && parts > part) {
                            alias[result] = constituent;
                            removed.push_back(result);
                            return false;
                        }
                    }
                }
            }
        }
        key.clear();
        key.push_back(static_cast<std::uint32_t>(op));
        key.insert(key.end(), instruction.begin() + 1, instruction.end());
        key[2] = 0u;
        VisitIdOperands(instruction, switchLiteralWords(instruction), [&](std::size_t index) {
            if (index != 2u) key[index] = canonical(instruction[index]);
        });
        if (decorated[result] != 0u) {
            key.push_back(~0u);
            const auto& decorations = decorationsOf.at(result);
            key.insert(key.end(), decorations.begin(), decorations.end());
        }
        const auto [found, inserted] = table.try_emplace(key, result);
        if (!inserted) {
            alias[result] = found->second;
            removed.push_back(result);
        }
        return inserted;
    }

    std::vector<std::uint32_t> rewrite() const {
        std::vector<std::uint8_t> drop(bound, 0u);
        for (const auto id : removed) drop[id] = 1u;
        std::vector<std::uint32_t> output(words.begin(), words.begin() + 5);
        output.reserve(words.size());
        for (std::size_t cursor = 5; cursor < words.size();) {
            const auto instruction = instructionAt(cursor);
            cursor += instruction.size();
            const auto op = Opcode(instruction);
            bool hasResult = false;
            bool hasType = false;
            spv::HasResultAndType(op, &hasResult, &hasType);
            if (hasResult && drop[instruction[hasType ? 2u : 1u]] != 0u) continue;
            if ((op == spv::OpName || op == spv::OpDecorate || op == spv::OpDecorateId || op == spv::OpDecorateString) && instruction.size() >= 2u && instruction[1] < bound && drop[instruction[1]] != 0u) continue;
            const auto start = output.size();
            output.insert(output.end(), instruction.begin(), instruction.end());
            VisitIdOperands(instruction, switchLiteralWords(instruction), [&](std::size_t index) {
                output[start + index] = canonical(instruction[index]);
            });
        }
        return output;
    }

    std::span<const std::uint32_t> words;
    std::uint32_t bound = 0;
    std::vector<std::uint32_t> alias;
    std::vector<std::uint32_t> resultType;
    std::vector<std::size_t> definition;
    std::vector<std::uint32_t> integerWidth;
    std::vector<std::uint32_t> pointerStorage;
    bool bufferBlocks = false;
    std::vector<std::uint8_t> decorated;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> decorationsOf;
    std::vector<std::uint32_t> glslImports;
    std::vector<std::uint32_t> removed;
};

}

std::vector<std::uint32_t> NumberSpirvValues(std::span<const std::uint32_t> words) {
    return ValueNumbering(words).Run();
}

}
