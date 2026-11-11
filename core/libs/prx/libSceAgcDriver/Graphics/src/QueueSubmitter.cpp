#include "prx/libSceAgcDriver/Graphics/include/QueueSubmitter.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/ThreadPriority.hpp"
#include "prx/libSceAgcDriver/Execution/include/TracyZone.hpp"
#include "prx/libSceAgcDriver/Graphics/include/CommandEncoder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include <chrono>
#ifdef _WIN32
#include "prx/libc/include/general/ThreadName.hpp"
#endif
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace AgcDriver::Graphics {

namespace {

struct Submitters {
    std::mutex mutex;
    std::map<VkQueue, std::weak_ptr<QueueSubmitter>> byQueue;
};

Submitters& AllSubmitters() {
    static Submitters* const submitters = new Submitters();
    return *submitters;
}

}

bool QueueSubmitter::Enabled() {
    return RuntimeSwitches::Set(RuntimeSwitch::AsyncSubmit);
}

std::shared_ptr<QueueSubmitter> QueueSubmitter::Acquire(VkDevice device, VkQueue queue, PFN_vkQueueSubmit submit, PFN_vkDeviceWaitIdle waitIdle) {
    auto& all = AllSubmitters();
    std::shared_ptr<QueueSubmitter> replaced;
    std::lock_guard lock(all.mutex);
    auto& entry = all.byQueue[queue];
    replaced = entry.lock();
    if (replaced != nullptr && replaced->device == device) return replaced;
    std::shared_ptr<QueueSubmitter> created(new QueueSubmitter(device, queue, submit, waitIdle));
    entry = created;
    return created;
}

std::shared_ptr<QueueSubmitter> QueueSubmitter::Existing(VkQueue queue) {
    auto& all = AllSubmitters();
    std::lock_guard lock(all.mutex);
    const auto found = all.byQueue.find(queue);
    return found != all.byQueue.end() ? found->second.lock() : nullptr;
}

QueueSubmitter::QueueSubmitter(VkDevice device, VkQueue queue, PFN_vkQueueSubmit submit, PFN_vkDeviceWaitIdle waitIdle) :
    device(device), queue(queue), submit(submit), waitIdle(waitIdle) {
    thread = std::thread([this] {
        TracyZone::NameThread("queue submitter");
#ifdef _WIN32
        ThreadName::SetCurrent("queue submitter");
#endif
        PerformanceTrace::BindThread("queue submitter");
        RaiseWorkerThreadPriority("queue submitter");
        run();
    });
}

QueueSubmitter::~QueueSubmitter() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    ready.notify_all();
    thread.join();
    auto& all = AllSubmitters();
    std::lock_guard lock(all.mutex);
    const auto found = all.byQueue.find(queue);
    if (found != all.byQueue.end() && found->second.expired()) all.byQueue.erase(found);
}

void SubmitOutcome::Wait() const {
    for (auto current = state.load(std::memory_order_acquire); current == Pending; current = state.load(std::memory_order_acquire)) state.wait(current, std::memory_order_acquire);
}

void SubmitOutcome::Report() const {
    if (Failed()) failures->reported.fetch_add(1, std::memory_order_acq_rel);
}

void SubmitOutcome::resolve(std::uint8_t outcome, std::exception_ptr reason) {
    error = std::move(reason);
    state.store(outcome, std::memory_order_release);
    state.notify_all();
}

std::shared_ptr<SubmitOutcome> QueueSubmitter::Enqueue(Job job) {
    bool wake = false;
    job.outcome = std::make_shared<SubmitOutcome>();
    job.outcome->failures = job.failures;
    auto outcome = job.outcome;
    {
        std::lock_guard lock(mutex);
        jobs.push_back(std::move(job));
        wake = sleeping;
    }
    if (wake) ready.notify_one();
    return outcome;
}

void QueueSubmitter::Drain() {
    std::unique_lock lock(mutex);
    drained.wait(lock, [&] { return jobs.empty() && !busy; });
}

void QueueSubmitter::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex);
            while (jobs.empty()) {
                if (stopping) return;
                sleeping = true;
                ready.wait(lock);
                sleeping = false;
            }
            job = std::move(jobs.front());
            jobs.pop_front();
            busy = true;
        }
        VkCommandBuffer commands = job.commands;
        std::exception_ptr failure;
        if (job.encode != nullptr) {
            if (!job.encode->Finished()) {
                PerformanceTimer waiting(TraceFormat::SubmitWaitScope);
                const auto waitStart = std::chrono::steady_clock::now();
                job.encode->Wait();
                CommandEncoders::CountWait(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count()));
            }
            if (job.encode->Failed()) failure = job.encode->Error();
            else commands = job.encode->Commands();
        }
        if (failure == nullptr && job.failures->Unreported()) failure = std::make_exception_ptr(std::runtime_error("AGC graphics: vkQueueSubmit recorder (queue submitter): an earlier batch failed"));
        if (failure == nullptr) {
            VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submission.commandBufferCount = 1;
            submission.pCommandBuffers = &commands;
            VkTimelineSemaphoreSubmitInfoKHR timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR};
            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            if (job.wait != VK_NULL_HANDLE) {
                submission.waitSemaphoreCount = 1;
                submission.pWaitSemaphores = &job.wait;
                submission.pWaitDstStageMask = &waitStage;
                timeline.waitSemaphoreValueCount = 1;
                timeline.pWaitSemaphoreValues = &job.waitValue;
            }
            if (job.signal != VK_NULL_HANDLE) {
                submission.signalSemaphoreCount = 1;
                submission.pSignalSemaphores = &job.signal;
                timeline.signalSemaphoreValueCount = 1;
                timeline.pSignalSemaphoreValues = &job.signalValue;
            }
            if (job.wait != VK_NULL_HANDLE || job.signal != VK_NULL_HANDLE) submission.pNext = &timeline;
            VkResult result;
            {
                std::lock_guard queueLock(queueMutex);
                result = submit(queue, 1, &submission, job.fence);
            }
            if (result == VK_ERROR_DEVICE_LOST && waitIdle != nullptr) {
                const auto idle = waitIdle(device);
                if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST) std::terminate();
            }
            if (result != VK_SUCCESS) failure = std::make_exception_ptr(std::runtime_error("AGC graphics: vkQueueSubmit recorder (queue submitter): Vulkan result " + std::to_string(static_cast<int>(result))));
        }
        job.encode.reset();
        if (failure != nullptr) {
            if (job.signal != VK_NULL_HANDLE) job.failures->Fail(job.signalValue);
            job.failures->raised.fetch_add(1, std::memory_order_acq_rel);
        }
        job.outcome->resolve(failure == nullptr ? SubmitOutcome::Submitted : SubmitOutcome::Failure, failure);
        {
            std::lock_guard lock(mutex);
            busy = false;
        }
        drained.notify_all();
    }
}

QueueAccess::QueueAccess(VkQueue queue) : submitter(QueueSubmitter::Existing(queue)) {
    if (submitter == nullptr) return;
    submitter->Drain();
    submitter->QueueMutex().lock();
}

QueueAccess::~QueueAccess() {
    if (submitter != nullptr) submitter->QueueMutex().unlock();
}

}
