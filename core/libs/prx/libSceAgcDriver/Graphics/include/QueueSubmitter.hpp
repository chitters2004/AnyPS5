#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_QUEUESUBMITTER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_QUEUESUBMITTER_HPP

#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace AgcDriver::Graphics {

class EncodeTicket;

struct SubmitFailures {
    std::atomic<std::uint64_t> raised{0};
    std::atomic<std::uint64_t> reported{0};
    bool Unreported() const { return raised.load(std::memory_order_acquire) > reported.load(std::memory_order_acquire); }
    // The timeline values of the batches that failed: nothing ever signals them, so a wait for one of
    // them must give up instead of waiting out VK_TIMEOUT.
    void Fail(std::uint64_t serial) {
        std::lock_guard lock(failedMutex);
        failedSerials.push_back(serial);
    }
    bool Failed(std::uint64_t serial) const {
        std::lock_guard lock(failedMutex);
        return std::find(failedSerials.begin(), failedSerials.end(), serial) != failedSerials.end();
    }

private:
    mutable std::mutex failedMutex;
    std::vector<std::uint64_t> failedSerials;
};

class SubmitOutcome {
public:
    bool Resolved() const { return state.load(std::memory_order_acquire) != Pending; }
    bool Failed() const { return state.load(std::memory_order_acquire) == Failure; }
    void Wait() const;
    std::exception_ptr Error() const { return error; }
    void Report() const;

private:
    friend class QueueSubmitter;
    static constexpr std::uint8_t Pending = 0, Submitted = 1, Failure = 2;
    void resolve(std::uint8_t outcome, std::exception_ptr reason);

    std::atomic<std::uint8_t> state{Pending};
    std::exception_ptr error;
    std::shared_ptr<SubmitFailures> failures;
};

class QueueSubmitter {
public:
    struct Job {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore wait = VK_NULL_HANDLE;
        std::uint64_t waitValue = 0;
        VkSemaphore signal = VK_NULL_HANDLE;
        std::uint64_t signalValue = 0;
        std::shared_ptr<EncodeTicket> encode;
        std::shared_ptr<SubmitFailures> failures;
        std::shared_ptr<SubmitOutcome> outcome;
        // The failures reported when the job was queued (set by Enqueue): a failure raised after that
        // is one this job was queued behind, whether or not it has been reported since.
        std::uint64_t reportedAtQueue = 0;
    };

    static bool Enabled();
    static std::shared_ptr<QueueSubmitter> Acquire(VkDevice device, VkQueue queue, PFN_vkQueueSubmit submit, PFN_vkDeviceWaitIdle waitIdle);
    static std::shared_ptr<QueueSubmitter> Existing(VkQueue queue);

    ~QueueSubmitter();
    QueueSubmitter(const QueueSubmitter&) = delete;
    QueueSubmitter& operator=(const QueueSubmitter&) = delete;

    std::shared_ptr<SubmitOutcome> Enqueue(Job job);
    void Drain();
    AgcDriver::Mutex& QueueMutex() { return queueMutex; }

private:
    QueueSubmitter(VkDevice device, VkQueue queue, PFN_vkQueueSubmit submit, PFN_vkDeviceWaitIdle waitIdle);
    void run();

    VkDevice device;
    VkQueue queue;
    PFN_vkQueueSubmit submit;
    PFN_vkDeviceWaitIdle waitIdle;
    AgcDriver::Mutex queueMutex;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable drained;
    std::deque<Job> jobs;
    bool busy = false;
    bool sleeping = false;
    bool stopping = false;
    std::thread thread;
};

class QueueAccess {
public:
    explicit QueueAccess(VkQueue queue);
    ~QueueAccess();
    QueueAccess(const QueueAccess&) = delete;
    QueueAccess& operator=(const QueueAccess&) = delete;

private:
    std::shared_ptr<QueueSubmitter> submitter;
};

inline VkResult QueueBindSparse(const Context& context, const VkBindSparseInfo& bind, VkFence fence) {
    const auto bindSparse = context.Function<PFN_vkQueueBindSparse>("vkQueueBindSparse");
    if (context.sparseQueue != VK_NULL_HANDLE && context.sparseQueueMutex != nullptr) {
        const std::lock_guard lock(*context.sparseQueueMutex);
        return bindSparse(context.sparseQueue, 1, &bind, fence);
    }
    const QueueAccess access(context.queue);
    return bindSparse(context.queue, 1, &bind, fence);
}

}

#endif
