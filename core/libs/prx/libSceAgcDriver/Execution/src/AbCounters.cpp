#include "prx/libSceAgcDriver/Execution/include/AbCounters.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include <mutex>
#include <thread>

namespace AgcDriver {

namespace {

struct Phase {
    std::mutex mutex;
    bool settled = false;
    char arm = 'A';
    std::uint64_t index = 0;
    AbSample base;
    std::vector<double> flips;
};

Phase& phase() {
    static Phase state;
    return state;
}

AbSample Take(std::uint64_t present, double seconds) {
    AbSample sample;
    sample.seconds = seconds;
    sample.present = present;
    const auto counts = VulkanDevice::PresentCounts();
    sample.gpuBusyMs = counts.gpuBusyMs;
    sample.gpuGapMs = counts.gpuGapMs;
    const auto totals = DriverDetail::DrawPipeline::Queue0().LifetimeTotals();
    const auto finisherDrains = DriverDetail::DrawFinisher::DrainWaitTotals();
    sample.committer = {totals.commits, totals.commitNs, totals.drawCommits, totals.drawNs, totals.enqueued, totals.fullWaitNs, totals.wakes, totals.drains, totals.drainWaitNs, finisherDrains.first, finisherDrains.second};
    sample.preparerRoomWaitNs = DriverDetail::DrawFinisher::PreparerRoomWaitNs();
    const auto sites = DriverDetail::DrawFinisher::SiteWaitTotals();
    for (std::size_t index = 0; index < sites.size(); ++index) sample.drainSites.push_back({DriverDetail::DrawFinisher::WaitSiteNames[index], sites[index].waits, sites[index].waitNs, sites[index].queued});
    const auto relocation = DriverDetail::DrawRelocationLearningTotals();
    sample.relocation = {relocation.lookups, relocation.misses, relocation.relocatedHits, relocation.learned, relocation.late, relocation.never, relocation.held};
    const auto ring = DriverDetail::DrawFinisher::RingTotals();
    sample.ring = {ring.issued, ring.pushed, ring.worked, ring.busyWallNs};
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (auto threads = PerformanceTrace::ThreadTotals()) {
            sample.threads = std::move(*threads);
            break;
        }
        std::this_thread::yield();
    }
    return sample;
}

void Observe(Graphics::AbSchedule::Event event, char arm, std::uint64_t index, std::uint64_t present, double seconds) {
    auto& state = phase();
    std::lock_guard lock(state.mutex);
    if (event == Graphics::AbSchedule::Event::Settled) {
        state.base = Take(present, seconds);
        state.arm = arm;
        state.index = index;
        state.flips.assign(1, seconds);
        state.settled = true;
        return;
    }
    if (!state.settled) return;
    state.flips.push_back(seconds);
    if (event != Graphics::AbSchedule::Event::Begin) return;
    state.settled = false;
    ProfilePrint_nid_no_patch("%s", AbCounters::Line(state.arm, state.index, state.base, Take(present, seconds), state.flips).c_str());
}

}

void InstallAbCounters() {
    Graphics::SetAbObserver(&Observe);
}

}
