#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestBufferProfile.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
namespace Profile = AgcDriver::GuestBufferProfile;

constexpr std::uint32_t Threads = 8;
constexpr std::uint32_t OutputHandle = 9;
constexpr std::size_t PageBytes = std::size_t{1} << Profile::PageShift;
constexpr std::size_t Pages = 12;
constexpr std::uint32_t Unwritten = 0xdeadbeefu;
constexpr std::uint32_t LabelValue = 1;
constexpr std::size_t LabelOffset = 0x400;

enum Page : std::size_t { First, Second, Third, Fourth, Fifth, StoreA, StoreB, Rewound, Labels, Count };

alignas(256) constexpr std::array<std::uint32_t, 4> StoreCode{
    0x34020082, 0xe0701000, 0x80000001, 0xbf810000,
};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

std::uint32_t Low(const void* data) {
    return static_cast<std::uint32_t>(AddressOf(data));
}

std::uint32_t High(const void* data) {
    return static_cast<std::uint32_t>(AddressOf(data) >> 32u);
}

class Arena {
public:
    Arena() : storage((Pages + 1) * PageBytes / sizeof(std::uint32_t)) {
        base = reinterpret_cast<std::byte*>((reinterpret_cast<std::uintptr_t>(storage.data()) + PageBytes - 1) & ~std::uintptr_t{PageBytes - 1});
        GuestAllocations::Mutation mutation;
        mutation.Add(base, Pages * PageBytes, true, true, true);
    }
    ~Arena() {
        GuestAllocations::Mutation mutation;
        mutation.Remove(base);
    }
    std::uint32_t* At(Page page) const { return reinterpret_cast<std::uint32_t*>(base + page * PageBytes); }
private:
    std::vector<std::uint32_t> storage;
    std::byte* base = nullptr;
};

void Dispatch(std::vector<std::uint32_t>& words, std::uint32_t* target) {
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{Threads, 1, 1};
    registers(0x207, shape);
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(AddressOf(StoreCode.data()) >> 8u), static_cast<std::uint32_t>(AddressOf(StoreCode.data()) >> 40u)};
    registers(0x20c, program);
    const std::array<std::uint32_t, 1> resource{4u << 1u};
    registers(0x213, resource);
    const std::array<std::uint32_t, 4> buffer{Low(target), (High(target) & 0xffffu), Threads * 4u, 0x3104dfacu};
    registers(0x240, buffer);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x41u});
}

void WriteLabel(std::vector<std::uint32_t>& words, std::uint32_t* label) {
    words.insert(words.end(), {0xc0064900u, 0x514u, (1u << 29u) | (2u << 24u), Low(label), High(label), LabelValue, 0, 0});
}

void WaitLabel(std::vector<std::uint32_t>& words, std::uint32_t* label) {
    words.insert(words.end(), {0xc0053c00u, 0x13u, Low(label), High(label), LabelValue, 0xffffffffu, 0x19u});
}

void FlipOutput(std::vector<std::uint32_t>& words) {
    words.insert(words.end(), {AgcDriver::FlipPacketHeader, OutputHandle, 0xfffffffeu, 1, 0, 0});
}

std::uint32_t Place(Arena& arena, Page page, const std::vector<std::uint32_t>& words) {
    std::copy(words.begin(), words.end(), arena.At(page));
    return static_cast<std::uint32_t>(words.size());
}

void Build(Arena& arena, Page page, std::uint32_t dwords) {
    AgcDriverPrepareCommandWrite_nid_no_patch(AddressOf(arena.At(page)), dwords * sizeof(std::uint32_t));
}

void BuildOnThread(Arena& arena, Page page, std::uint32_t dwords) {
    std::thread builder([&] { Build(arena, page, dwords); });
    builder.join();
}

class Request final : public AgcDriver::IFlipRequest {
public:
    explicit Request(std::atomic<int>& ready) : ready(ready) {}
    void GpuReady(const std::shared_ptr<AgcDriver::FrameTiming>&) override { ++ready; }
    void Fail(std::exception_ptr) noexcept override {}
private:
    std::atomic<int>& ready;
};

class Output final : public AgcDriver::IVideoOutput {
public:
    std::shared_ptr<AgcDriver::IFlipRequest> Reserve(const AgcDriver::FlipInfo&) override { return std::make_shared<Request>(ready); }
    void Fail(std::exception_ptr) noexcept override {}
    std::atomic<int> ready = 0;
};

void Expect(std::uint64_t actual, std::uint64_t expected, const std::string& what) {
    Require(actual == expected, what + ": expected " + std::to_string(expected) + ", counted " + std::to_string(actual));
}

void ExpectStored(const std::uint32_t* target, const std::string& what) {
    for (std::uint32_t thread = 0; thread < Threads; ++thread) Require(target[thread] == thread, what + " was not stored by its dispatch");
}

}

int RewindTails() {
    Arena arena;
    auto output = std::make_shared<Output>();
    AgcDriverRegisterVideoOutput_nid_postfix(OutputHandle, output);
    constexpr std::uint32_t Frames = 6;
    for (std::uint32_t frame = 0; frame < Frames; ++frame) {
        const auto token = AgcDriverReserveEopFlip_nid_postfix(AgcDriver::FlipInfo{OutputHandle, 0, 1, static_cast<std::int64_t>(frame)});
        Require(token != 0, "the EOP flip of frame " + std::to_string(frame) + " was not reserved");
        auto* const words = arena.At(Rewound) + frame * 64;
        auto* const label = arena.At(Labels) + frame * 2;
        const std::array<std::uint32_t, 10> commands{0xc0005900u, 0u, 0xc0064900u, 0x06200504u, 0x42010000u, Low(label), High(label), 1u, 0u, token};
        std::copy(commands.begin(), commands.end(), words);
        const Packet packet{words, static_cast<std::uint32_t>(commands.size()), 0, {}};
        AgcDriver::Submit(&packet, 0);
        std::atomic_ref<std::uint32_t>(words[1]).store(AgcDriver::Pm4::RewindValid, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (output->ready.load() < static_cast<int>(frame + 1)) {
            Require(std::chrono::steady_clock::now() < deadline, "the rewind tail of frame " + std::to_string(frame) + " did not flip");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    AgcDriverWaitIdle_nid_postfix();
    const auto held = Profile::Held();
    const auto steady = Profile::Closed();
    Profile::CloseFinished();
    const auto totals = Profile::Closed();
    AgcDriverUnregisterVideoOutput_nid_postfix(OutputHandle, output);
    AgcDriverShutdown_nid_postfix();
    Expect(steady.frames, Frames - 1, "frames closed by flips in rewind tails before the last flip ended");
    Expect(held.frames, 0, "frames held after every tail ran");
    Expect(totals.frames, Frames, "frames ended by flips in rewind tails");
    Expect(totals.buffers[0], 2 * Frames, "graphics buffers, each submission and its rewind tail");
    Expect(totals.rewindTails, Frames, "rewind tails");
    Expect(totals.submissions[0], 2 * Frames, "graphics submissions, a tail released apart from its submission");
    Require(totals.workerNs > 0, "no worker time was counted");
    std::puts("guest buffer counters rewind tail tests passed");
    return 0;
}

int HeldFrame() {
    Profile::NoteSubmitted(1, 0, nullptr, 0, false, false);
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
        const Profile::Execution execution(1);
        started.store(true);
        while (!release.load()) std::this_thread::yield();
    });
    while (!started.load()) std::this_thread::yield();
    constexpr std::uint64_t Frames = 40;
    for (std::uint64_t frame = 0; frame < Frames; ++frame) {
        const auto serial = frame + 2;
        Profile::NoteSubmitted(serial, 0, nullptr, 0, true, false);
        const Profile::Execution execution(serial);
    }
    const auto held = Profile::Held();
    const auto steady = Profile::Closed();
    release.store(true);
    holder.join();
    Profile::CloseFinished();
    const auto all = Profile::Closed();
    Expect(steady.frames, Frames - 2, "frames closed while an earlier frame's buffer still runs");
    Expect(held.frames, 1, "frames held by the running buffer");
    Expect(held.running, 1, "frames held by a running buffer");
    Expect(held.unexecuted, 0, "frames held by unexecuted buffers");
    Expect(held.holderSerial, 1, "the running buffer's serial");
    Expect(held.oldestAge, Frames, "frames back to the held frame");
    Expect(all.frames, Frames, "frames closed once the running buffer finished");
    std::puts("guest buffer counters held frame tests passed");
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    _putenv_s("APS5_PROFILE_GUEST_CBS", "1");
#else
    setenv("APS5_PROFILE_GUEST_CBS", "1", 1);
#endif
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--held-frame") return HeldFrame();
        if (argc == 2 && std::string_view(argv[1]) == "--rewind-tails") return RewindTails();
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        Arena arena;
        for (const auto page : {StoreA, StoreB}) std::fill(arena.At(page), arena.At(page) + Threads, Unwritten);
        auto* const label = arena.At(StoreA) + LabelOffset;
        *label = 0;
        auto output = std::make_shared<Output>();
        AgcDriverRegisterVideoOutput_nid_postfix(OutputHandle, output);

        std::vector<std::uint32_t> words;
        Dispatch(words, arena.At(StoreA));
        WriteLabel(words, label);
        const auto first = Place(arena, First, words);
        words.clear();
        WaitLabel(words, label);
        Dispatch(words, arena.At(StoreB));
        const auto second = Place(arena, Second, words);
        words.clear();
        Dispatch(words, arena.At(StoreA));
        FlipOutput(words);
        const auto third = Place(arena, Third, words);
        words.clear();
        Dispatch(words, arena.At(StoreA));
        const auto fourth = Place(arena, Fourth, words);
        words.clear();
        Dispatch(words, arena.At(StoreB));
        FlipOutput(words);
        const auto fifth = Place(arena, Fifth, words);

        BuildOnThread(arena, First, first);
        BuildOnThread(arena, Second, second);
        Build(arena, Third, third);
        BuildOnThread(arena, Fifth, fifth);

        std::thread compute([&] {
            const Packet packet{arena.At(Fourth), fourth, 0, {}};
            AgcDriver::Submit(&packet, 0x20);
        });
        compute.join();
        std::array<std::uint32_t*, 3> addresses{arena.At(First), arena.At(Second), arena.At(Third)};
        const std::array<std::uint32_t, 3> sizes{first, second, third};
        AgcDriver::SubmitMany(addresses.data(), sizes.data(), static_cast<std::uint32_t>(addresses.size()), 0);
        const Packet last{arena.At(Fifth), fifth, 0, {}};
        AgcDriver::Submit(&last, 0);
        AgcDriverWaitIdle_nid_postfix();
        Profile::CloseFinished();
        const auto totals = Profile::Closed();
        AgcDriverUnregisterVideoOutput_nid_postfix(OutputHandle, output);
        AgcDriverShutdown_nid_postfix();

        ExpectStored(arena.At(StoreA), "the first, third and compute buffers' store");
        ExpectStored(arena.At(StoreB), "the second and fifth buffers' store");
        Expect(static_cast<std::uint64_t>(output->ready.load()), 2, "flips");
        Expect(totals.frames, 2, "frames");
        Expect(totals.submissions[0], 2, "graphics submissions");
        Expect(totals.buffers[0], 4, "graphics buffers");
        Expect(totals.mostBuffers[0], 3, "most graphics buffers in a frame");
        Expect(totals.submissions[1], 1, "compute submissions");
        Expect(totals.buffers[1], 1, "compute buffers");
        Expect(totals.buffersPerSubmission[0], 2, "submissions of one buffer");
        Expect(totals.buffersPerSubmission[1], 1, "submissions of two or three buffers");
        Expect(totals.buffersPerSubmission[2] + totals.buffersPerSubmission[3], 0, "submissions of four or more buffers");
        Expect(totals.draws[0], 5, "buffers without draws");
        Expect(totals.dispatches[1], 5, "buffers with one to nine dispatches");
        Expect(totals.submitThreads, 3, "submitting threads summed over frames");
        Expect(totals.mostSubmitThreads, 2, "most submitting threads in a frame");
        Expect(totals.buildThreads, 4, "building threads summed over frames");
        Expect(totals.mostBuildThreads, 3, "most building threads in a frame");
        Expect(totals.builtElsewhere, 3, "buffers built off their submitting thread");
        Expect(totals.unbuilt, 1, "buffers with no builder seen");
        Expect(totals.pairs, 6, "buffer pairs");
        Expect(totals.conflictingPairs, 3, "pairs that share a written page");
        Expect(totals.conflictingBuffers, 3, "buffers in a sharing pair");
        Expect(totals.labelEdges, 1, "label edges between buffers");
        Expect(totals.ownWaits, 0, "waits on a buffer's own label");
        Expect(totals.outsideWaits, 0, "waits on labels from outside the frame");
        Expect(totals.largeFrames + totals.cyclicFrames + totals.truncatedRanges, 0, "frames skipped or ranges truncated");
        Require(totals.workerNs > 0, "no worker time was counted");
        Require(totals.criticalNs < totals.workerNs + totals.committerNs, "the critical path holds the second and third buffers, which share nothing");
        Require(totals.largestNs < totals.criticalNs, "the critical path is no longer than the largest buffer although the compute, first and third buffers share a page in turn");
        Require(totals.coveringBuffers >= totals.frames, "fewer than one buffer per frame makes 80% of the work");
        std::puts("guest buffer counters tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
