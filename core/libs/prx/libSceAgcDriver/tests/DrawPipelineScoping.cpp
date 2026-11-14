#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {

using AgcDriver::DriverDetail::DrawPipeline;
using namespace AgcDriver::GuestMemory;

constexpr std::uint64_t Written = std::uint64_t{0x100} * RangeGranuleBytes;
constexpr std::uint64_t Elsewhere = std::uint64_t{0x900} * RangeGranuleBytes;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

RangeStamp StampOf(std::uint64_t address) {
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> ranges{std::pair{address, address + 64}};
    return RangeVersionStamp(ranges);
}

void Commit(DrawPipeline::CommitKind kind, std::vector<DrawPipeline::Range> writes, DrawPipeline::Commit commit = [] {}, bool draw = false) {
    auto& pipeline = DrawPipeline::Queue0();
    pipeline.Enqueue(kind, std::move(commit), std::move(writes), 0, {}, {}, draw);
    pipeline.Drain(DrawPipeline::DrainReason::Flush);
}

void CheckScoping() {
    using Kind = DrawPipeline::CommitKind;
    const auto written = StampOf(Written);
    const auto elsewhere = StampOf(Elsewhere);
    RangeStamp during;
    Commit(Kind::DmaFill, {{Written, Written + 64}}, [&] { during = StampOf(Written); });
    Check(during != written, "a scoped commit did not bump its writes before it ran");
    Check(StampOf(Written) != during, "a scoped commit did not bump its writes after it ran");
    Check(StampOf(Elsewhere) == elsewhere, "a scoped commit moved a range it does not write");
    const auto proofs = ProofSerial();
    const std::array scoped{Kind::SampleDump, Kind::DmaWrite, Kind::DmaCopy, Kind::Store, Kind::EndOfPipe, Kind::Seal, Kind::SubmitEnd, Kind::Completion, Kind::BufferFill};
    for (const auto kind : scoped) Commit(kind, {});
    Check(StampOf(Elsewhere) == elsewhere, "an audited non-draw kind bumped the unscoped version");
    Check(ProofSerial() != proofs, "non-draw commits no longer break the global proof serial");
    const std::array unscoped{Kind::Other, Kind::IndirectDraw, Kind::ColorMetadata, Kind::Dispatch};
    for (const auto kind : unscoped) {
        const auto before = StampOf(Elsewhere);
        Commit(kind, {});
        Check(StampOf(Elsewhere).unscoped != before.unscoped, "an unaudited non-draw kind left the unscoped version");
    }
    const auto drawn = StampOf(Written);
    const auto drawnElsewhere = StampOf(Elsewhere);
    Commit(Kind::Draw, {{Written, Written + 64}}, [] {}, true);
    Check(StampOf(Written) == drawn && StampOf(Elsewhere) == drawnElsewhere, "a draw commit bumped versions in the pipeline");
}

}

int main() {
    try {
        CheckScoping();
        DrawPipeline::Queue0().Stop();
        std::cout << "Draw pipeline scoping passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
