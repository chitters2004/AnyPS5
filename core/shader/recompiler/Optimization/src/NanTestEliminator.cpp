#include "Optimization/NanTestEliminator.hpp"
#include "IntermediateRepresentation/IrBuilder.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace ShaderRecompiler {

namespace {

constexpr double Unbounded = std::numeric_limits<double>::infinity();
constexpr double LargestFinite = static_cast<double>(std::numeric_limits<float>::max());

struct Facts {
    bool notNan = false;
    double bound = Unbounded;
};

bool isImmediate(const IrValue* value, std::uint32_t bits) {
    return value->HasImmediate() && value->ImmediateU32() == bits;
}

const IrValue* maskedOperand(const IrValue* value, IrOpcode opcode, std::uint32_t bits) {
    if (value->Opcode() != opcode || value->ArgumentCount() != 2u) return nullptr;
    const IrValue* lhs = value->Argument(0)->Resolve();
    const IrValue* rhs = value->Argument(1)->Resolve();
    if (isImmediate(rhs, bits)) return lhs;
    if (isImmediate(lhs, bits)) return rhs;
    return nullptr;
}

const IrValue* testedValue(const IrValue& condition) {
    if (condition.Opcode() == IrOpcode::FPIsNan32 && condition.ArgumentCount() == 1u) return condition.Argument(0)->Resolve();
    if (condition.Opcode() != IrOpcode::UGreaterThan32 || condition.ArgumentCount() != 2u || !isImmediate(condition.Argument(1)->Resolve(), 0x7f800000u)) return nullptr;
    return maskedOperand(condition.Argument(0)->Resolve(), IrOpcode::BitwiseAnd32, 0x7fffffffu);
}

Facts finite(double bound) {
    return {true, bound <= LargestFinite ? bound : Unbounded};
}

Facts immediateFacts(std::uint32_t bits) {
    if ((bits & 0x7fffffffu) > 0x7f800000u) return {};
    if ((bits & 0x7fffffffu) == 0x7f800000u) return {true, Unbounded};
    return {true, std::fabs(static_cast<double>(std::bit_cast<float>(bits)))};
}

class NanFreeValues {
public:
    Facts Of(const IrValue* value) {
        value = value->Resolve();
        if (const auto found = known.find(value); found != known.end()) return found->second;
        known.emplace(value, Facts{});
        const Facts result = compute(*value);
        known[value] = result;
        return result;
    }

    bool NotNan(const IrValue* value) {
        return Of(value).notNan;
    }

private:
    bool bounded(const IrValue* value) {
        return Of(value).bound != Unbounded;
    }

    Facts sum(const IrValue* lhs, const IrValue* rhs) {
        const Facts a = Of(lhs);
        const Facts b = Of(rhs);
        if (!a.notNan || !b.notNan) return {};
        if (a.bound == Unbounded && b.bound == Unbounded) return {};
        return finite(a.bound + b.bound);
    }

    Facts product(const IrValue* lhs, const IrValue* rhs) {
        const Facts a = Of(lhs);
        const Facts b = Of(rhs);
        if (a.bound == Unbounded || b.bound == Unbounded) return {};
        return finite(a.bound * b.bound);
    }

    Facts productSum(const IrValue* lhs, const IrValue* rhs, const IrValue* addend) {
        const Facts p = product(lhs, rhs);
        const Facts c = Of(addend);
        if (!p.notNan || !c.notNan) return {};
        if (p.bound == Unbounded && c.bound == Unbounded) return {};
        return finite(p.bound + c.bound);
    }

    Facts all(const IrValue& value, std::size_t first) {
        Facts result{true, 0.0};
        for (std::size_t index = first; index < value.ArgumentCount(); ++index) {
            const Facts facts = Of(value.Argument(index));
            if (!facts.notNan) return {};
            result.bound = std::max(result.bound, facts.bound);
        }
        return result;
    }

    Facts compute(const IrValue& value) {
        if (value.HasImmediate()) return immediateFacts(value.ImmediateU32());
        switch (value.Opcode()) {
        case IrOpcode::ConvertF32S32:
            return finite(2147483648.0);
        case IrOpcode::ConvertF32U32:
            return finite(4294967296.0);
        case IrOpcode::FPSaturate32:
            return finite(1.0);
        case IrOpcode::FPSin:
        case IrOpcode::FPCos:
            return bounded(value.Argument(0)) ? finite(1.0) : Facts{};
        case IrOpcode::FPFract32:
            return bounded(value.Argument(0)) ? finite(1.0) : Facts{};
        case IrOpcode::FPFloor32:
        case IrOpcode::FPCeil32:
        case IrOpcode::FPTrunc32:
        case IrOpcode::FPRoundEven32: {
            const Facts facts = Of(value.Argument(0));
            return facts.bound == Unbounded ? facts : finite(std::floor(facts.bound) + 1.0);
        }
        case IrOpcode::BitCastU32F32:
        case IrOpcode::BitCastF32U32:
        case IrOpcode::FPAbs32:
        case IrOpcode::FPNeg32:
            return Of(value.Argument(0));
        case IrOpcode::BitwiseXor32:
            if (const IrValue* operand = maskedOperand(&value, IrOpcode::BitwiseXor32, 0x80000000u)) return Of(operand);
            return {};
        case IrOpcode::BitwiseAnd32:
            if (const IrValue* operand = maskedOperand(&value, IrOpcode::BitwiseAnd32, 0x7fffffffu)) return Of(operand);
            return {};
        case IrOpcode::FPAdd32:
        case IrOpcode::FPSub32:
            return sum(value.Argument(0), value.Argument(1));
        case IrOpcode::FPMul32:
            return product(value.Argument(0), value.Argument(1));
        case IrOpcode::FPFma32:
        case IrOpcode::FPMad32:
            return productSum(value.Argument(0), value.Argument(1), value.Argument(2));
        case IrOpcode::FPMin32:
        case IrOpcode::FPMax32:
        case IrOpcode::FPMinTri32:
        case IrOpcode::FPMaxTri32:
        case IrOpcode::FPMedTri32:
        case IrOpcode::Phi:
            return all(value, 0u);
        case IrOpcode::SelectU32:
        case IrOpcode::SelectF32: {
            const IrValue* condition = value.Argument(0)->Resolve();
            if (condition->HasImmediate()) return Of(value.Argument(condition->ImmediateU32() != 0u ? 1u : 2u));
            const IrValue* tested = testedValue(*condition);
            if (tested != nullptr && NotNan(tested)) return Of(value.Argument(2));
            return all(value, 1u);
        }
        default:
            return {};
        }
    }

    std::unordered_map<const IrValue*, Facts> known;
};

}

bool NanProofDisabled() {
    static const bool disabled = [] {
        const char* text = std::getenv("APS5_NO_NAN_PROOF");
        return text != nullptr && *text != '\0' && std::strcmp(text, "0") != 0;
    }();
    return disabled;
}

namespace {

std::atomic<std::uint64_t> totalTests{0};
std::atomic<std::uint64_t> totalRemoved{0};

bool StatsLogged() {
    static const bool logged = std::getenv("APS5_NAN_PROOF_STATS") != nullptr;
    return logged;
}

}

void RecordNanTests(std::uint64_t codeAddress, const NanTestEliminationStats& stats) {
    totalTests.fetch_add(stats.tests, std::memory_order_relaxed);
    totalRemoved.fetch_add(stats.removedTests, std::memory_order_relaxed);
    if (!StatsLogged()) return;
    std::fprintf(stderr, "[nan-proof] program 0x%llx f32 NaN tests %u removed %u kept %u\n", static_cast<unsigned long long>(codeAddress), stats.tests, stats.removedTests, stats.tests - stats.removedTests);
}

NanTestEliminationStats NanTestTotals() {
    return {static_cast<std::uint32_t>(totalTests.load(std::memory_order_relaxed)), static_cast<std::uint32_t>(totalRemoved.load(std::memory_order_relaxed))};
}

NanTestEliminationStats NanTestEliminator::Eliminate(IrProgram& program) const {
    NanTestEliminationStats stats;
    NanFreeValues values;
    IrBuilder builder(program);
    IrValue* never = nullptr;
    for (auto* block : program.BlockOrder()) {
        for (auto* inst : block->Instructions()) {
            if (!inst->HasUses()) continue;
            const IrValue* tested = testedValue(*inst);
            if (tested == nullptr) continue;
            ++stats.tests;
            if (!values.NotNan(tested)) continue;
            if (never == nullptr) never = &builder.ConstantBool(false);
            inst->ReplaceAllUsesWith(never);
            ++stats.removedTests;
        }
    }
    return stats;
}

}
