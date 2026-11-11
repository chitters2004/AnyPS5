#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Buffer;
using AgcDriver::Graphics::Context;
using AgcDriver::Graphics::GuestBufferMemory;
using AgcDriver::Graphics::Recorder;
using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::ResidentStatistics;

constexpr std::size_t BlockBytes = std::size_t{1} << 20u;
constexpr std::size_t ElementOffset = 65536;
constexpr std::size_t ElementBytes = 4 * 65536;
constexpr std::size_t FilledBytes = 256;
constexpr std::uint32_t PromoteAfter = 8;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        void* mapped = mmap(nullptr, 2 * BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + 2 * BlockBytes) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + BlockBytes - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            if (AgcDriver::GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "resident range ownership: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        munmap(block, BlockBytes);
        if (AgcDriver::GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
};

struct Use {
    std::size_t offset = ElementOffset;
    std::size_t bytes = ElementBytes;
    std::optional<std::uint32_t> fill;
    std::function<void()> between;
    bool failSubmit = false;
    bool checkView = true;
};

ResidentStatistics Delta(const ResidentStatistics& before, const ResidentStatistics& after) {
    return {after.resident, after.promoted - before.promoted, after.bound - before.bound, after.skipped - before.skipped, after.copied - before.copied, after.invalidated - before.invalidated, after.skippedBytes - before.skippedBytes, after.kept - before.kept, after.refused - before.refused, after.discarded - before.discarded};
}

std::string Counts(const ResidentStatistics& delta) {
    return "skipped " + std::to_string(delta.skipped) + ", copied " + std::to_string(delta.copied) + ", promoted " + std::to_string(delta.promoted) + ", invalidated " + std::to_string(delta.invalidated) + ", kept " + std::to_string(delta.kept) + ", refused " + std::to_string(delta.refused) + ", discarded " + std::to_string(delta.discarded);
}

bool IgnoringPageWrites = false;

void CpuStore(std::uint8_t* at, std::uint8_t value, std::size_t length) {
    std::memset(at, value, length);
    if (IgnoringPageWrites) AgcDriver::GuestMemory::MarkWritten(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(at)), length);
}

class Harness {
public:
    Harness(const Context& context, std::uint8_t* guest) : context(context), guest(guest) {}

    ResidentStatistics Build(const Use& use, const std::string& what) {
        std::unique_lock gpu(AgcDriver::GuestMemory::GpuMutex());
        auto* recorder = Recorder::Active();
        Require(recorder != nullptr, "resident range ownership: no active recorder");
        recorder->Sync();
        const auto before = AgcDriver::Graphics::ResidentCounts();
        const auto address = Address(use.offset);
        GuestBufferMemory memory(context);
        memory.AllowDeviceStaging();
        memory.AddWritable(address, use.bytes, false, true);
        memory.Upload(false);
        Require(memory.InPlaceReads().empty(), "resident range ownership: " + what + ": the element was bound in place, not staged");
        std::uint32_t adjustment = 0;
        const auto view = memory.Descriptor(address, use.bytes, adjustment);
        Buffer readback(context, use.bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto commands = recorder->Commands();
        AgcDriver::Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        AgcDriver::Graphics::CopyBuffer(context, commands, view.buffer, view.offset + adjustment, readback.Handle(), 0, use.bytes);
        if (use.fill.has_value()) {
            AgcDriver::Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, view.buffer, view.offset + adjustment, FilledBytes, *use.fill);
        }
        AgcDriver::Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT);
        memory.RecordCopyBacks(*recorder);
        recorder->NotePendingWrites(memory.Writes());
        memory.MarkDirectWrites();
        std::vector<std::uint8_t> atRecord(guest + use.offset, guest + use.offset + use.bytes);
        if (use.between) use.between();
        if (use.failSubmit) {
            Recorder::FailNextSubmitsForTest(1u);
            bool discarded = false;
            try {
                recorder->Submit();
            } catch (const std::exception& error) {
                discarded = std::string(error.what()).find("injected failure") != std::string::npos;
            }
            Recorder::FailNextSubmitsForTest(0u);
            Require(discarded, "resident range ownership: " + what + ": the build's batch was not discarded");
            return Delta(before, AgcDriver::Graphics::ResidentCounts());
        }
        recorder->Sync();
        memory.WriteBack();
        AgcDriver::GuestMemory::BumpCollectEpoch();
        if (use.checkView) {
            readback.Invalidate();
            const auto seen = readback.Bytes();
            for (std::size_t index = 0; index < use.bytes; ++index) {
                if (static_cast<std::uint8_t>(seen[index]) == atRecord[index]) continue;
                Require(false, "resident range ownership: " + what + ": the GPU read 0x" + Hex(static_cast<std::uint8_t>(seen[index])) + " at element byte " + std::to_string(index) + " where guest memory held 0x" + Hex(atRecord[index]));
            }
        }
        return Delta(before, AgcDriver::Graphics::ResidentCounts());
    }

    std::uint64_t Address(std::size_t offset) const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(guest + offset)); }

private:
    static std::string Hex(unsigned value) {
        char text[16];
        std::snprintf(text, sizeof(text), "%02x", value);
        return text;
    }

    Context context;
    std::uint8_t* guest;
};

void ExpectBytes(const std::uint8_t* at, std::size_t bytes, std::uint8_t value, const std::string& what) {
    for (std::size_t index = 0; index < bytes; ++index) {
        if (at[index] == value) continue;
        Require(false, "resident range ownership: " + what + ": byte " + std::to_string(index) + " reads " + std::to_string(at[index]) + ", expected " + std::to_string(value));
    }
}

int Run(AgcDriver::VulkanDevice& device) {
    const auto context = device.graphicsContext();
    if (context.hostImportAlignment == 0) {
        std::puts("resident range ownership: host imports unavailable; skipped");
        return VulkanTestSkipped;
    }
    GuestBlock block;
    auto* guest = block.Data();
    std::memset(guest, 0x10, BlockBytes);
    Harness harness(context, guest);
    const auto address = harness.Address(ElementOffset);
    {
        std::unique_lock gpu(AgcDriver::GuestMemory::GpuMutex());
        if (AgcDriver::Graphics::HostImportFor(context, address, ElementBytes) == nullptr) {
            std::puts("resident range ownership: the host import of the guest block was refused; skipped");
            return VulkanTestSkipped;
        }
    }
    if (IgnoringPageWrites) AgcDriver::GuestMemory::IgnorePageWritesForTest(harness.Address(0), BlockBytes);
    struct StopIgnoring {
        ~StopIgnoring() { AgcDriver::GuestMemory::IgnorePageWritesForTest(0, 0); }
    } stopIgnoring;
    const bool versioned = AgcDriver::GuestMemory::Watched(address, ElementBytes);
    if (IgnoringPageWrites && !versioned) {
        std::puts("resident range ownership: the guest block is not write-watched, so page writes cannot be ignored; skipped");
        return VulkanTestSkipped;
    }
    const bool wholeCopyBack = std::getenv("APS5_NO_CHANGED_COPY_BACK") != nullptr;
    const auto expectReuse = [&](const ResidentStatistics& delta, const std::string& what) {
        if (wholeCopyBack) Require(delta.skipped == 0 && delta.refused == 1 && delta.copied == 1, "resident range ownership: " + what + " skipped a copy-in although its whole copy-back would store over CPU bytes (" + Counts(delta) + ")");
        else Require(delta.skipped == 1 && delta.copied == 0, "resident range ownership: " + what + " did not skip the copy-in of the unchanged resident range (" + Counts(delta) + ")");
    };

    for (std::uint32_t use = 1; use < PromoteAfter; ++use) harness.Build({}, "staging " + std::to_string(use));
    auto delta = harness.Build({}, "the promoting staging");
    Require(delta.promoted == 1 && delta.copied == 1, "resident range ownership: the eighth staging did not make the range resident (" + Counts(delta) + ")");
    delta = harness.Build({}, "an unchanged staging");
    expectReuse(delta, "an unchanged staging");
    std::printf("resident range ownership: the range is resident (%s write versions)\n", versioned ? "with" : "without");

    delta = harness.Build({ElementOffset, ElementBytes, 0x2a2a2a2au, [&] { CpuStore(guest + ElementOffset + 8192, 0x77, 64); }, false, false}, "a staging with a CPU store between record and execution");
    ExpectBytes(guest + ElementOffset + 8192, 64, 0x77, "the CPU store made between the staging's record and its execution");
    ExpectBytes(guest + ElementOffset, FilledBytes, 0x2a, "the GPU write a CPU read follows");
    delta = harness.Build({}, "the staging after the CPU store");
    Require(delta.skipped == 0, "resident range ownership: the staging after a CPU store skipped the copy-in (" + Counts(delta) + ")");
    std::puts("resident range ownership: a CPU store between record and execution survives, and the GPU's write reaches the CPU");

    delta = harness.Build({ElementOffset, ElementBytes, 0x3b3b3b3bu}, "a GPU write of the resident range");
    ExpectBytes(guest + ElementOffset, FilledBytes, 0x3b, "the resident range's GPU write");
    if (versioned) Require(delta.kept >= 1, "resident range ownership: the range's own copy-back did not keep it in sync (" + Counts(delta) + ")");
    delta = harness.Build({}, "the staging after the range's own GPU write");
    expectReuse(delta, "the staging after the range's own GPU write");
    if (versioned && !wholeCopyBack) Require(delta.skipped == 1, "resident range ownership: a GPU-written range lost its residency to its own copy-back");
    std::puts("resident range ownership: the range stays resident across its own copy-back");

    delta = harness.Build({ElementOffset, ElementBytes, 0x5d5d5d5du, [&] { AgcDriver::GuestMemory::MarkWritten(address + 32768, 64); }, false, true}, "a GPU write with another stamped writer of the range in flight");
    delta = harness.Build({}, "the staging after the stamped writer");
    if (versioned) Require(delta.skipped == 0, "resident range ownership: a range written by another stamped writer while its build ran stayed in sync across its copy-back (" + Counts(delta) + ")");
    std::puts("resident range ownership: a stamped writer in flight over the range keeps it out of sync");

    harness.Build({ElementOffset + ElementBytes / 2, ElementBytes, 0x4c4c4c4cu, {}, false, true}, "a write through an overlapping alias");
    ExpectBytes(guest + ElementOffset + ElementBytes / 2, FilledBytes, 0x4c, "the alias's GPU write");
    delta = harness.Build({}, "the staging after the alias's write");
    Require(delta.skipped == 0, "resident range ownership: the staging after an alias's write skipped the copy-in (" + Counts(delta) + ")");
    std::puts("resident range ownership: a resident range sees an overlapping alias's writes");

    CpuStore(guest + ElementOffset + 16384, 0x99, 64);
    delta = harness.Build({ElementOffset, ElementBytes, std::nullopt, {}, true, false}, "a staging whose submit fails");
    Require(delta.discarded == 1, "resident range ownership: the failed submit did not drop the resident range (" + Counts(delta) + ")");
    CpuStore(guest + ElementOffset + 20480, 0x9a, 64);
    delta = harness.Build({ElementOffset, ElementBytes, std::nullopt, {}, true, false}, "a second staging whose submit fails in the batch the first left open");
    Require(delta.discarded == 1, "resident range ownership: the second failed submit in a row did not drop the resident range (" + Counts(delta) + ")");
    delta = harness.Build({}, "the staging after the failed submit");
    Require(delta.skipped == 0, "resident range ownership: the staging after a failed submit skipped the copy-in (" + Counts(delta) + ")");
    std::puts("resident range ownership: a failed submit gives up the range's residency");

    for (std::uint32_t use = 0; use < PromoteAfter + 1; ++use) harness.Build({}, "staging " + std::to_string(use) + " after the failed submit");
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(static_cast<std::uintptr_t>(address), ElementBytes);
    delta = harness.Build({}, "the staging after the range was forgotten");
    Require(delta.skipped == 0, "resident range ownership: the staging after ForgetPages skipped the copy-in (" + Counts(delta) + ")");
    std::puts("resident range ownership: forgetting the range's pages drops its residency");
    std::puts("resident range ownership tests passed");
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        IgnoringPageWrites = argc == 2 && std::string(argv[1]) == "--ignore-page-writes";
#ifdef _WIN32
        _putenv_s("APS5_NO_ADAPTIVE_STAGING", "1");
#else
        setenv("APS5_NO_ADAPTIVE_STAGING", "1", 1);
#endif
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        return Run(*device);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
