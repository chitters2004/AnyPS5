#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#include <SDL_loadso.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace {

using AgcDriver::Graphics::Recorder;
using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::RuntimeSwitch;
namespace Switches = AgcDriver::Graphics::RuntimeSwitches;

constexpr std::size_t BlockBytes = 65536;

struct Block {
    Block() {
#ifdef _WIN32
        bytes = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        bytes = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(bytes != nullptr, "timeline reap gate: cannot allocate a host block");
        std::memset(bytes, 0, BlockBytes);
        GuestAllocations::Mutation().Add(bytes, BlockBytes, true, true, true);
    }
    ~Block() {
        GuestAllocations::Mutation().Remove(bytes);
#ifdef _WIN32
        VirtualFree(bytes, 0, MEM_RELEASE);
#else
        std::free(bytes);
#endif
    }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(bytes)); }
    std::uint8_t* bytes = nullptr;
};

class Gate {
public:
    explicit Gate(VkDevice device) : device(device) {
#ifdef _WIN32
        constexpr const char* libraryName = "vulkan-1.dll";
#elif defined(__APPLE__)
        constexpr const char* libraryName = "libvulkan.1.dylib";
#else
        constexpr const char* libraryName = "libvulkan.so.1";
#endif
        library.reset(SDL_LoadObject(libraryName));
        Require(library != nullptr, "timeline reap gate: cannot load Vulkan for the submission gate");
        resolve = reinterpret_cast<PFN_vkGetDeviceProcAddr>(SDL_LoadFunction(library.get(), "vkGetDeviceProcAddr"));
        Require(resolve != nullptr, "timeline reap gate: cannot resolve vkGetDeviceProcAddr");
        destroySemaphore = function<PFN_vkDestroySemaphore>("vkDestroySemaphore");
        signalSemaphore = reinterpret_cast<PFN_vkSignalSemaphore>(resolve(device, "vkSignalSemaphoreKHR"));
        if (signalSemaphore == nullptr) signalSemaphore = function<PFN_vkSignalSemaphore>("vkSignalSemaphore");
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        Require(function<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device, &info, nullptr, &semaphore) == VK_SUCCESS, "timeline reap gate: cannot create the submission gate");
    }

    ~Gate() {
        Release();
        destroySemaphore(device, semaphore, nullptr);
    }

    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;

    VkSemaphore Handle() const { return semaphore; }

    void Release() {
        if (std::exchange(released, true)) return;
        VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        info.semaphore = semaphore;
        info.value = 1;
        Require(signalSemaphore(device, &info) == VK_SUCCESS, "timeline reap gate: cannot release the submission gate");
    }

private:
    template <typename TFunction>
    TFunction function(const char* name) const {
        const auto found = reinterpret_cast<TFunction>(resolve(device, name));
        Require(found != nullptr, std::string("timeline reap gate: cannot resolve ") + name);
        return found;
    }

    VkDevice device;
    std::unique_ptr<void, decltype(&SDL_UnloadObject)> library{nullptr, &SDL_UnloadObject};
    PFN_vkGetDeviceProcAddr resolve = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
    PFN_vkSignalSemaphore signalSemaphore = nullptr;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    bool released = false;
};

void HoldBatch(Recorder& recorder, const Gate& gate) {
    recorder.Commands();
    recorder.HoldNextSubmit(gate.Handle(), 1);
    recorder.Submit();
}

}

int main() {
    try {
        Block block;
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::unique_lock lock(AgcDriver::GuestMemory::GpuMutex());
        const std::array<std::uint32_t, 4> pattern{1u, 2u, 3u, 4u};
        if (!device->FillBuffer(block.Address(), 4096, pattern)) {
            std::printf("skipped, the device does not import guest memory\n");
            return VulkanTestSkipped;
        }
        device->SubmitRecorded(false);
        auto* recorder = Recorder::Active();
        Require(recorder != nullptr, "timeline reap gate: no active recorder");
        if (!recorder->HasTimeline()) {
            std::printf("skipped, the recorder has no timeline semaphore\n");
            return VulkanTestSkipped;
        }
        recorder->Sync();
        Switches::Assign(RuntimeSwitch::NoTimelineReapGate, false);
        {
            Gate gate(recorder->Device());
            HoldBatch(*recorder, gate);
            Require(recorder->InFlightBatches() == 1, "timeline reap gate: the held batch is not in flight");
            const auto gatedBefore = Recorder::GatedReaps();
            Require(!recorder->ReapCompleted() && recorder->InFlightBatches() == 1 && Recorder::GatedReaps() == gatedBefore + 1, "timeline reap gate: a reap over a batch the progress watcher has not seen complete was not gated");
            Switches::Assign(RuntimeSwitch::NoTimelineReapGate, true);
            Require(!recorder->ReapCompleted() && recorder->InFlightBatches() == 1 && Recorder::GatedReaps() == gatedBefore + 1, "timeline reap gate: APS5_NO_TIMELINE_REAP_GATE did not check the fence");
            Switches::Assign(RuntimeSwitch::NoTimelineReapGate, false);
            gate.Release();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!recorder->ReapCompleted()) {
                Require(std::chrono::steady_clock::now() < deadline, "timeline reap gate: a batch the GPU completed was never reaped through the gate");
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            Require(recorder->InFlightBatches() == 0, "timeline reap gate: the completed batch is still in flight");
            const auto gatedEmpty = Recorder::GatedReaps();
            Require(recorder->ReapCompleted() && Recorder::GatedReaps() == gatedEmpty, "timeline reap gate: a reap with nothing in flight was gated");
        }
        double fenceNs = 0;
        double gateNs = 0;
        {
            Gate gate(recorder->Device());
            HoldBatch(*recorder, gate);
            constexpr int runs = 20000;
            const auto timed = [&](bool off) {
                Switches::Assign(RuntimeSwitch::NoTimelineReapGate, off);
                const auto start = std::chrono::steady_clock::now();
                for (int run = 0; run < runs; ++run) static_cast<void>(recorder->ReapCompleted());
                return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / runs;
            };
            fenceNs = timed(true);
            gateNs = timed(false);
            gate.Release();
            recorder->Sync();
        }
        Switches::Assign(RuntimeSwitch::NoTimelineReapGate, false);
        device->WaitIdle();
        std::printf("[reap-gate-bench] one unfinished batch in flight: fence check %.1f ns, gate %.1f ns per reap\n", fenceNs, gateNs);
        std::puts("timeline reap gate tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
