#include "prx/libSceAgcDriver/Graphics/include/LabelCompare.hpp"
#include <cstdio>
#include <functional>
#include <vector>

using namespace AgcDriver::Graphics::LabelCompare;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

enum class Lease { None, Same, Different };

struct Outcome {
    bool changedAfter;
    bool stampedAfter;
};

std::vector<Outcome> Outcomes(const BlockState& state) {
    std::vector<bool> afterWriter;
    if (state.writerOver) afterWriter = {false, true};
    else afterWriter = {state.changedNow};
    std::vector<Outcome> outcomes;
    for (const bool beforeLease : afterWriter) {
        std::vector<Lease> leases{Lease::None};
        if (state.leaseOver) leases = {Lease::None, Lease::Same, Lease::Different};
        for (const auto lease : leases) {
            std::vector<bool> changed;
            if (lease != Lease::Different) changed = {beforeLease};
            else if (beforeLease) changed = {false, true};
            else changed = {true};
            for (const bool after : changed) outcomes.push_back({after, state.stampedNow || lease != Lease::None});
        }
    }
    return outcomes;
}

bool Today(const BlockState& state, const Outcome& outcome) {
    if (state.unknown) return false;
    return state.kept || (outcome.stampedAfter && outcome.changedAfter);
}

struct Tally {
    int checked = 0;
    int mismatches = 0;
    int onGpu = 0;
    int fallback = 0;
};

Tally Check(const std::function<BlockMode(const BlockState&)>& classify) {
    Tally tally;
    for (unsigned bits = 0; bits < 64; ++bits) {
        const BlockState state{(bits & 1u) != 0, (bits & 2u) != 0, (bits & 4u) != 0, (bits & 8u) != 0, (bits & 16u) != 0, (bits & 32u) != 0};
        const auto mode = classify(state);
        if (mode == BlockMode::Fallback) {
            ++tally.fallback;
            continue;
        }
        if (mode == BlockMode::OnGpu) ++tally.onGpu;
        for (const auto& outcome : Outcomes(state)) {
            ++tally.checked;
            if (Written(mode, outcome.changedAfter) != Today(state, outcome)) ++tally.mismatches;
        }
    }
    return tally;
}

BlockMode WithoutLeaseGuard(const BlockState& block) {
    if (block.unknown) return BlockMode::Store;
    if (block.kept) return BlockMode::Keep;
    if (!block.writerOver && !block.leaseOver) return block.stampedNow && block.changedNow ? BlockMode::Keep : BlockMode::Store;
    if (block.stampedNow || block.leaseOver) return BlockMode::OnGpu;
    return BlockMode::Store;
}

BlockMode WithoutWriterInGuard(const BlockState& block) {
    if (block.unknown) return BlockMode::Store;
    if (block.kept) return BlockMode::Keep;
    if (!block.writerOver && !block.leaseOver) return block.stampedNow && block.changedNow ? BlockMode::Keep : BlockMode::Store;
    if (block.leaseOver && !block.stampedNow && block.changedNow) return BlockMode::Fallback;
    if (block.stampedNow || block.leaseOver) return BlockMode::OnGpu;
    return BlockMode::Store;
}

BlockMode KeepForgetsCpuWrites(const BlockState& block) {
    if (block.writerOver || block.leaseOver) return Classify(block);
    return BlockMode::Store;
}

BlockMode WriterAlwaysOnGpu(const BlockState& block) {
    if (block.unknown) return BlockMode::Store;
    if (block.kept) return BlockMode::Keep;
    if (!block.writerOver && !block.leaseOver) return block.stampedNow && block.changedNow ? BlockMode::Keep : BlockMode::Store;
    if (block.leaseOver && !block.stampedNow && (block.changedNow || block.writerOver)) return BlockMode::Fallback;
    return BlockMode::OnGpu;
}

BlockMode KeptOnGpu(const BlockState& block) {
    if (!block.unknown && block.kept && (block.writerOver || block.leaseOver)) return BlockMode::OnGpu;
    return Classify(block);
}

BlockMode UntrackedKept(const BlockState& block) {
    if (block.unknown) return BlockMode::Keep;
    return Classify(block);
}

}

int main() {
    const auto rule = Check(Classify);
    Expect(rule.mismatches == 0, "the rule decides every block as the flush does after its wait");
    Expect(rule.onGpu > 0, "the rule leaves some blocks to the GPU");
    Expect(rule.fallback > 0 && rule.fallback < 16, "the rule falls back for few states");
    std::printf("label compare rule: %d outcomes checked, %d mismatches, %d states on the GPU, %d fall back\n", rule.checked, rule.mismatches, rule.onGpu, rule.fallback);

    struct Control {
        const char* name;
        BlockMode (*classify)(const BlockState&);
    };
    const Control controls[] = {
        {"without the lease guard", WithoutLeaseGuard},
        {"without the writer in the lease guard", WithoutWriterInGuard},
        {"storing CPU-written blocks without writers", KeepForgetsCpuWrites},
        {"comparing unstamped explicit writes on the GPU", WriterAlwaysOnGpu},
        {"comparing kept blocks on the GPU", KeptOnGpu},
        {"keeping untracked units", UntrackedKept},
    };
    for (const auto& control : controls) {
        const auto tally = Check(control.classify);
        std::printf("negative control %s: %d mismatches\n", control.name, tally.mismatches);
        Expect(tally.mismatches > 0, control.name);
    }

    Expect(Classify({true, false, false, false, true, true}) == BlockMode::Keep, "a block kept at unwatch stays kept under pending writers");
    Expect(Classify({false, true, false, false, true, false}) == BlockMode::Store, "an untracked unit is stored whole under pending writers");
    Expect(Classify({true, true, true, true, true, true}) == BlockMode::Store, "an untracked unit is stored whole even where a block was kept at unwatch");
    Expect(Classify({false, false, true, true, false, false}) == BlockMode::Keep, "a CPU-written block without pending writers is kept on the CPU");
    Expect(Classify({false, false, true, false, false, false}) == BlockMode::Store, "a stamped block holding its baseline is stored");
    Expect(Classify({false, false, false, false, true, false}) == BlockMode::Store, "an unstamped block under an explicit writer is stored");
    Expect(Classify({false, false, true, false, true, false}) == BlockMode::OnGpu, "a stamped block under an explicit writer is compared on the GPU");
    Expect(Classify({false, false, false, false, false, true}) == BlockMode::OnGpu, "an unstamped block holding its baseline under a lease is compared on the GPU");
    Expect(Classify({false, false, false, true, false, true}) == BlockMode::Fallback, "an unstamped changed block under a lease falls back");
    Expect(Classify({false, false, false, false, true, true}) == BlockMode::Fallback, "an unstamped block under a lease and an explicit writer falls back");
    Expect(Written(BlockMode::OnGpu, true) && !Written(BlockMode::OnGpu, false), "a GPU-compared block is kept only when its bytes changed");
    Expect(!Written(BlockMode::Fallback, true), "a fallback block decides nothing");

    if (failures != 0) {
        std::fprintf(stderr, "label compare tests: %d failures\n", failures);
        return 1;
    }
    std::printf("label compare tests passed\n");
    return 0;
}
