#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/NanTestEliminator.hpp"
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <initializer_list>
#include <utility>

using namespace ShaderRecompiler;

namespace {

struct Builder {
    IrProgram program;
    IrBlock* block = nullptr;

    Builder() {
        block = &newBlock();
        program.SetEntryBlock(*block);
    }

    IrBlock& newBlock() {
        auto& created = program.CreateBlock();
        program.BlockOrder().push_back(&created);
        return created;
    }

    IrValue& constant(std::uint32_t value) {
        auto& created = program.CreateValue(IrOpcode::Void, IrType::U32);
        created.SetImmediateU32(value);
        return created;
    }

    IrValue& emit(IrOpcode opcode, IrType type, std::initializer_list<IrValue*> arguments) {
        auto& created = program.CreateValue(opcode, type);
        for (auto* argument : arguments) created.AddArgument(argument);
        block->AppendInstruction(&created);
        return created;
    }

    IrValue& unknown() {
        return emit(IrOpcode::BitCastF32U32, IrType::F32, {&emit(IrOpcode::LaneId, IrType::U32, {})});
    }

    IrValue& integer() {
        return emit(IrOpcode::ConvertF32S32, IrType::F32, {&emit(IrOpcode::LaneId, IrType::U32, {})});
    }

    IrValue& saturated() {
        return emit(IrOpcode::FPSaturate32, IrType::F32, {&unknown()});
    }

    IrValue& f32(std::uint32_t bits) {
        return emit(IrOpcode::BitCastF32U32, IrType::F32, {&constant(bits)});
    }

    IrValue& op(IrOpcode opcode, std::initializer_list<IrValue*> arguments) {
        return emit(opcode, IrType::F32, arguments);
    }

    IrValue& isNan(IrValue& value) {
        return emit(IrOpcode::FPIsNan32, IrType::Bool, {&value});
    }

    IrValue& isNanBits(IrValue& value) {
        auto& bits = emit(IrOpcode::BitCastU32F32, IrType::U32, {&value});
        auto& magnitude = emit(IrOpcode::BitwiseAnd32, IrType::U32, {&bits, &constant(0x7fffffffu)});
        return emit(IrOpcode::UGreaterThan32, IrType::Bool, {&magnitude, &constant(0x7f800000u)});
    }

    IrValue& repaired(IrValue& value) {
        auto& bits = emit(IrOpcode::BitCastU32F32, IrType::U32, {&value});
        auto& select = emit(IrOpcode::SelectU32, IrType::U32, {&isNanBits(value), &constant(0xffc00000u), &bits});
        return emit(IrOpcode::BitCastF32U32, IrType::F32, {&select});
    }

    IrValue& phi(IrBlock& target, std::initializer_list<std::pair<IrBlock*, IrValue*>> incoming) {
        auto& created = program.CreateValue(IrOpcode::Phi, IrType::F32);
        target.AppendInstruction(&created);
        for (const auto& [predecessor, value] : incoming) created.AddPhiOperand(predecessor, value);
        return created;
    }

    void keep(IrValue& value) {
        (void)emit(IrOpcode::ReferenceU32, IrType::Void, {&value});
    }

    bool removed(IrValue& test) {
        keep(test);
        const auto stats = NanTestEliminator{}.Eliminate(program);
        return stats.tests >= 1u && !test.HasUses();
    }

    bool kept(IrValue& test) {
        keep(test);
        (void)NanTestEliminator{}.Eliminate(program);
        return test.HasUses();
    }
};

bool run(const char* name, const std::function<bool()>& test) {
    try {
        if (test()) return true;
        std::fprintf(stderr, "%s\n", name);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: %s\n", name, error.what());
    }
    return false;
}

}

int main() {
    bool passed = true;

    passed &= run("a value read from memory keeps its test", [] {
        Builder b;
        return b.kept(b.isNan(b.unknown()));
    });
    passed &= run("a saturated value is never NaN", [] {
        Builder b;
        return b.removed(b.isNan(b.saturated()));
    });
    passed &= run("the bit-pattern test is recognised like FPIsNan32", [] {
        Builder b;
        return b.removed(b.isNanBits(b.saturated()));
    });
    passed &= run("a test whose mask is not the magnitude mask is left alone", [] {
        Builder b;
        auto& bits = b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.saturated()});
        auto& masked = b.emit(IrOpcode::BitwiseAnd32, IrType::U32, {&bits, &b.constant(0x7fff0000u)});
        auto& test = b.emit(IrOpcode::UGreaterThan32, IrType::Bool, {&masked, &b.constant(0x7f800000u)});
        b.keep(test);
        return NanTestEliminator{}.Eliminate(b.program).tests == 0u && test.HasUses();
    });
    passed &= run("integer conversions are finite", [] {
        Builder b;
        auto& unsignedValue = b.op(IrOpcode::ConvertF32U32, {&b.emit(IrOpcode::LaneId, IrType::U32, {})});
        return b.removed(b.isNan(b.op(IrOpcode::FPMul32, {&b.integer(), &unsignedValue})));
    });
    passed &= run("an f16 conversion can carry a NaN", [] {
        Builder b;
        return b.kept(b.isNan(b.op(IrOpcode::ConvertF32F16, {&b.emit(IrOpcode::LaneId, IrType::U32, {})})));
    });
    passed &= run("a NaN constant is NaN", [] {
        Builder b;
        return b.kept(b.isNan(b.op(IrOpcode::FPAdd32, {&b.saturated(), &b.f32(0x7fc00000u)})));
    });
    passed &= run("a finite value plus infinity is not NaN", [] {
        Builder b;
        return b.removed(b.isNan(b.op(IrOpcode::FPAdd32, {&b.saturated(), &b.f32(0x7f800000u)})));
    });
    passed &= run("infinity minus infinity can be NaN", [] {
        Builder b;
        return b.kept(b.isNan(b.op(IrOpcode::FPSub32, {&b.f32(0x7f800000u), &b.f32(0x7f800000u)})));
    });
    passed &= run("zero times infinity can be NaN", [] {
        Builder b;
        return b.kept(b.isNan(b.op(IrOpcode::FPMul32, {&b.saturated(), &b.f32(0x7f800000u)})));
    });
    passed &= run("a product that can overflow is not NaN but is no longer finite", [] {
        Builder b;
        auto& large = b.op(IrOpcode::FPMul32, {&b.integer(), &b.f32(0x7e800000u)});
        auto& test = b.isNan(large);
        auto& overflowed = b.op(IrOpcode::FPMul32, {&large, &b.saturated()});
        auto& second = b.isNan(overflowed);
        b.keep(test);
        b.keep(second);
        const auto stats = NanTestEliminator{}.Eliminate(b.program);
        return stats.tests == 2u && stats.removedTests == 1u && !test.HasUses() && second.HasUses();
    });
    passed &= run("bounds add up through sums", [] {
        Builder b;
        auto& sum = b.op(IrOpcode::FPAdd32, {&b.f32(0x7f000000u), &b.f32(0x7f000000u)});
        auto& test = b.isNan(b.op(IrOpcode::FPMul32, {&sum, &b.saturated()}));
        return b.kept(test);
    });
    passed &= run("a bounded fused product plus infinity is not NaN", [] {
        Builder b;
        return b.removed(b.isNan(b.op(IrOpcode::FPFma32, {&b.saturated(), &b.integer(), &b.f32(0x7f800000u)})));
    });
    passed &= run("a fused product with an unknown addend keeps its test", [] {
        Builder b;
        return b.kept(b.isNan(b.op(IrOpcode::FPFma32, {&b.saturated(), &b.integer(), &b.unknown()})));
    });
    passed &= run("a fused product of an unbounded factor keeps its test", [] {
        Builder b;
        return b.kept(b.isNan(b.op(IrOpcode::FPMad32, {&b.f32(0x7f800000u), &b.saturated(), &b.integer()})));
    });
    passed &= run("min and max need every operand", [] {
        Builder b;
        auto& both = b.op(IrOpcode::FPMax32, {&b.saturated(), &b.integer()});
        auto& one = b.op(IrOpcode::FPMin32, {&b.saturated(), &b.unknown()});
        auto& first = b.isNan(both);
        auto& second = b.isNan(one);
        b.keep(first);
        b.keep(second);
        const auto stats = NanTestEliminator{}.Eliminate(b.program);
        return stats.removedTests == 1u && !first.HasUses() && second.HasUses();
    });
    passed &= run("sine of a bounded value is bounded, of an unknown value is not", [] {
        Builder b;
        auto& bounded = b.isNan(b.op(IrOpcode::FPSin, {&b.integer()}));
        auto& unbounded = b.isNan(b.op(IrOpcode::FPCos, {&b.unknown()}));
        b.keep(bounded);
        b.keep(unbounded);
        const auto stats = NanTestEliminator{}.Eliminate(b.program);
        return stats.removedTests == 1u && !bounded.HasUses() && unbounded.HasUses();
    });
    passed &= run("a repair select of a proven value passes the value's facts on", [] {
        Builder b;
        auto& value = b.repaired(b.op(IrOpcode::FPMul32, {&b.saturated(), &b.integer()}));
        return b.removed(b.isNan(b.op(IrOpcode::FPMul32, {&value, &b.saturated()})));
    });
    passed &= run("a repair select of an unknown value is not proven", [] {
        Builder b;
        auto& value = b.repaired(b.unknown());
        return b.kept(b.isNan(b.op(IrOpcode::FPMul32, {&value, &b.saturated()})));
    });
    passed &= run("a select is proven when both arms are", [] {
        Builder b;
        auto& condition = b.emit(IrOpcode::IEqual32, IrType::Bool, {&b.emit(IrOpcode::LaneId, IrType::U32, {}), &b.constant(0u)});
        auto& both = b.emit(IrOpcode::SelectF32, IrType::F32, {&condition, &b.saturated(), &b.integer()});
        auto& one = b.emit(IrOpcode::SelectF32, IrType::F32, {&condition, &b.unknown(), &b.saturated()});
        auto& first = b.isNan(both);
        auto& second = b.isNan(one);
        b.keep(first);
        b.keep(second);
        const auto stats = NanTestEliminator{}.Eliminate(b.program);
        return stats.removedTests == 1u && !first.HasUses() && second.HasUses();
    });
    passed &= run("sign and magnitude operations keep facts", [] {
        Builder b;
        auto& negated = b.op(IrOpcode::FPNeg32, {&b.op(IrOpcode::FPAbs32, {&b.saturated()})});
        auto& bits = b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&negated});
        auto& flipped = b.emit(IrOpcode::BitwiseXor32, IrType::U32, {&bits, &b.constant(0x80000000u)});
        auto& cleared = b.emit(IrOpcode::BitwiseAnd32, IrType::U32, {&flipped, &b.constant(0x7fffffffu)});
        auto& other = b.emit(IrOpcode::BitwiseXor32, IrType::U32, {&bits, &b.constant(0x00400000u)});
        auto& first = b.isNan(b.emit(IrOpcode::BitCastF32U32, IrType::F32, {&cleared}));
        auto& second = b.isNan(b.emit(IrOpcode::BitCastF32U32, IrType::F32, {&other}));
        b.keep(first);
        b.keep(second);
        const auto stats = NanTestEliminator{}.Eliminate(b.program);
        return stats.removedTests == 1u && !first.HasUses() && second.HasUses();
    });
    passed &= run("rounding keeps a bounded value bounded", [] {
        Builder b;
        auto& floor = b.op(IrOpcode::FPFloor32, {&b.op(IrOpcode::FPMul32, {&b.saturated(), &b.integer()})});
        return b.removed(b.isNan(b.op(IrOpcode::FPMul32, {&floor, &b.integer()})));
    });
    passed &= run("fract of an unknown value can be NaN", [] {
        Builder b;
        return b.kept(b.isNan(b.op(IrOpcode::FPFract32, {&b.unknown()})));
    });
    passed &= run("a phi of proven values is proven", [] {
        Builder b;
        auto& entry = *b.block;
        auto& next = b.newBlock();
        auto& first = b.saturated();
        auto& second = b.integer();
        b.block = &next;
        auto& merged = b.phi(next, {{&entry, &first}, {&entry, &second}});
        return b.removed(b.isNan(merged));
    });
    passed &= run("a phi on a cycle is not proven", [] {
        Builder b;
        auto& entry = *b.block;
        auto& loop = b.newBlock();
        auto& start = b.saturated();
        b.block = &loop;
        auto& merged = b.phi(loop, {{&entry, &start}});
        auto& next = b.op(IrOpcode::FPAdd32, {&merged, &b.saturated()});
        merged.AddPhiOperand(&loop, &next);
        return b.kept(b.isNan(merged));
    });
    passed &= run("a removed test is replaced by false", [] {
        Builder b;
        auto& test = b.isNan(b.saturated());
        auto& user = b.emit(IrOpcode::LogicalNot, IrType::Bool, {&test});
        b.keep(user);
        (void)NanTestEliminator{}.Eliminate(b.program);
        const IrValue* replaced = user.Argument(0)->Resolve();
        return replaced->HasImmediate() && replaced->ImmediateU32() == 0u;
    });

    if (!passed) return 1;
    std::puts("nan test elimination tests passed");
    return 0;
}
