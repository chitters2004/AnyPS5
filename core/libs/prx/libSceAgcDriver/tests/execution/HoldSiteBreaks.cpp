#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace AgcDriver;
using Graphics::Require;
using Range = std::pair<std::uint64_t, std::uint64_t>;

template <typename Body>
void OnThread(bool proof, Body body) {
    std::thread thread([&] {
        if (proof) GuestMemory::MarkProofThread();
        body();
    });
    thread.join();
}

void IdleHold(bool proof) {
    OnThread(proof, [] { std::lock_guard hold(GuestMemory::GpuMutex()); });
}

void IdleHoldCases() {
    const bool scoped = !Graphics::RuntimeSwitches::Set(Graphics::RuntimeSwitch::NoHoldSiteBreaks);
    const auto proof = GuestMemory::ProofSerial();
    const auto certificate = GuestMemory::CertificateBreakSerial();
    const auto skipped = GuestMemory::SkippedHoldBreaks();
    IdleHold(false);
    if (scoped) {
        Require(GuestMemory::ProofSerial() == proof, "an idle GPU-lock hold off the proof thread moved the proof serial");
        Require(GuestMemory::SkippedHoldBreaks() == skipped + 1, "an idle hold was not counted as a skipped hold break");
    } else {
        Require(GuestMemory::ProofSerial() == proof + 1, "with APS5_NO_HOLD_SITE_BREAKS an idle hold did not break proofs at acquire");
        const std::array<Range, 1> any{{{0x1000, 0x2000}}};
        const auto window = GuestMemory::ProofBreaksSince(proof, any);
        Require(window.complete && window.unranged && window.blocker == GuestMemory::ProofBreakSite::GpuMutex, "with APS5_NO_HOLD_SITE_BREAKS the acquire break was not unranged");
    }
    Require(GuestMemory::CertificateBreakSerial() == certificate + 1, "an idle hold off the proof thread no longer voided certificates");
    const auto beforeProofThread = GuestMemory::ProofSerial();
    const auto skippedBeforeProofThread = GuestMemory::SkippedHoldBreaks();
    IdleHold(true);
    Require(GuestMemory::ProofSerial() == beforeProofThread && GuestMemory::SkippedHoldBreaks() == skippedBeforeProofThread, "a hold on the proof thread broke proofs or counted a skipped break");
}

void KeysFillCases() {
    constexpr std::uint64_t keys = 0x7f0000000000ull;
    constexpr std::size_t count = 256;
    const auto proof = GuestMemory::ProofSerial();
    OnThread(false, [] {
        std::lock_guard hold(GuestMemory::GpuMutex());
        Graphics::NoteKeysFillOnGpu(keys, count, Graphics::DccKeys::Uncompressed);
    });
    Require(GuestMemory::ProofSerial() > proof, "a key fill memo under an off-thread hold logged no proof break");
    const std::array<Range, 1> inside{{{keys + 16, keys + 32}}};
    const std::array<Range, 1> outside{{{keys + 0x100000, keys + 0x100100}}};
    const auto hit = GuestMemory::ProofBreaksSince(proof, inside);
    Require(hit.complete && !hit.clear, "a key fill memo under an off-thread hold did not block a set reading its keys");
    if (Graphics::RuntimeSwitches::Set(Graphics::RuntimeSwitch::NoHoldSiteBreaks)) return;
    Require(!hit.unranged && hit.blocker == GuestMemory::ProofBreakSite::HeldState, "a key fill memo did not block a set reading its keys with a ranged break");
    const auto miss = GuestMemory::ProofBreaksSince(proof, outside);
    Require(miss.complete && miss.clear, "a key fill memo blocked a set outside its keys");
}

void ClosedPipelineCases() {
    auto& pipeline = DriverDetail::DrawPipeline::Queue0();
    pipeline.Stop();
    const auto proof = GuestMemory::ProofSerial();
    bool committed = false;
    OnThread(false, [&] {
        pipeline.Enqueue([&] {
            std::lock_guard hold(GuestMemory::GpuMutex());
            committed = true;
        }, {});
    });
    Require(committed, "a commit enqueued after the pipeline closed did not run");
    const std::array<Range, 1> any{{{0x1000, 0x2000}}};
    const auto window = GuestMemory::ProofBreaksSince(proof, any);
    Require(window.complete && window.unranged && window.blocker == GuestMemory::ProofBreakSite::GpuMutex, "a commit run inline after the pipeline closed broke no proof, although it skips the committer's packet breaks");
}

}

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "--acquire") Graphics::RuntimeSwitches::Assign(Graphics::RuntimeSwitch::NoHoldSiteBreaks, true);
        IdleHoldCases();
        KeysFillCases();
        ClosedPipelineCases();
        Graphics::RuntimeSwitches::Assign(Graphics::RuntimeSwitch::NoHoldSiteBreaks, !Graphics::RuntimeSwitches::Set(Graphics::RuntimeSwitch::NoHoldSiteBreaks));
        IdleHoldCases();
        KeysFillCases();
        ClosedPipelineCases();
        std::puts("hold site break tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
