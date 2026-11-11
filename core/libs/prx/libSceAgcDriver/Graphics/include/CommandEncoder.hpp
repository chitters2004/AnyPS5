#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMANDENCODER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMANDENCODER_HPP

#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace AgcDriver::Graphics {

class CommandEncoder {
    struct Chunk;

public:
    using Run = void (*)(const std::byte* payload, VkCommandBuffer commands, const DeviceFunctions& functions);
    class Recording {
    public:
        Recording() = default;
        Recording(const Recording&) = delete;
        Recording& operator=(const Recording&) = delete;
        bool Failed() const { return failed.load(std::memory_order_acquire); }
        void Report() {
            if (Failed() && !reported) deliver();
        }
        void Finish() {
            if (Failed()) refuse();
        }

    private:
        friend class CommandEncoder;
        [[noreturn]] void deliver();
        [[noreturn]] void refuse();
        std::exception_ptr error;
        std::atomic<bool> failed{false};
        bool reported = false;
    };
    enum class PostCause : std::size_t { Window, Full, Recording, Await, Foreign, BatchEnd, Count };
    enum class SettleSite : std::uint8_t { Other, Present };
    class SubmitRequest {
    public:
        enum class State : std::uint32_t { Pending, Submitted, Failed, NotSubmitted };
        using Settled = void (*)(void* owner, const SubmitRequest& request, State state) noexcept;
        SubmitRequest();
        ~SubmitRequest();
        SubmitRequest(const SubmitRequest&) = delete;
        SubmitRequest& operator=(const SubmitRequest&) = delete;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore hold = VK_NULL_HANDLE;
        std::uint64_t holdValue = 0;
        VkSemaphore timeline = VK_NULL_HANDLE;
        std::uint64_t serial = 0;
        PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
        PFN_vkQueueSubmit queueSubmit = nullptr;
        AgcDriver::Mutex* queueMutex = nullptr;
        Recording* recording = nullptr;
        VkResult injectFailure = VK_SUCCESS;
        Settled settled = nullptr;
        void* owner = nullptr;
        State Current() const noexcept { return static_cast<State>(state.load(std::memory_order_acquire)); }
        State Wait() const noexcept;
        VkResult Result() const noexcept { return result; }
        std::exception_ptr Error() const noexcept { return error; }
        std::chrono::steady_clock::time_point SubmitStart() const noexcept { return submitStart; }
        std::chrono::steady_clock::time_point SubmittedAt() const noexcept { return submittedAt; }

    private:
        friend class CommandEncoder;
        std::unique_ptr<Chunk> marker;
        CommandEncoder* encoder = nullptr;
        std::atomic<std::uint32_t> state{0};
        VkResult result = VK_SUCCESS;
        std::exception_ptr error;
        std::chrono::steady_clock::time_point submitStart{};
        std::chrono::steady_clock::time_point submittedAt{};
    };
    static bool Enabled();
    static bool PostsChunks();
    static bool SubmitsAsync();
    static bool OnEncoderThread() noexcept;
    static constexpr std::size_t Slots = 4;
    static std::unique_ptr<CommandEncoder> Attach(VkDevice device, DeviceFunctions& table);
    ~CommandEncoder();
    CommandEncoder(const CommandEncoder&) = delete;
    CommandEncoder& operator=(const CommandEncoder&) = delete;
    void Open(VkCommandBuffer commands, Recording& recording);
    void Close();
    void Quiesce() noexcept;
    void FinishBatch() noexcept;
    bool Direct() noexcept;
    void Settle() noexcept;
    void PostSubmit(SubmitRequest& request) noexcept;
    void Adopt(VkCommandBuffer commands) noexcept;
    void SettleSubmits(SettleSite site = SettleSite::Other);
    [[noreturn]] static void RefuseForeignProducer();
    bool OpenOnThisThread() const { return target.load(std::memory_order_acquire) != VK_NULL_HANDLE && owner.load(std::memory_order_relaxed) == std::this_thread::get_id(); }
    bool Defers(VkCommandBuffer commands) const {
        if (commands == VK_NULL_HANDLE || target.load(std::memory_order_acquire) != commands) return false;
        if (owner.load(std::memory_order_relaxed) != std::this_thread::get_id()) RefuseForeignProducer();
        return true;
    }
    bool Behind(VkCommandBuffer commands) const { return commands == recent.load(std::memory_order_relaxed) && (parked.load(std::memory_order_acquire) != nullptr || executed.load(std::memory_order_acquire) < posted.load(std::memory_order_acquire)); }
    std::byte* Append(Run run, std::size_t bytes);
    AgcDriver::Mutex& PoolMutex() { return poolMutex; }
    const DeviceFunctions& Real() const { return real; }
    struct Statistics {
        std::uint64_t posts = 0;
        std::uint64_t commands = 0;
        std::uint64_t waits = 0;
        std::uint64_t waitNs = 0;
        std::uint64_t encodeNs = 0;
        std::uint64_t windows = 0;
        std::array<std::uint64_t, static_cast<std::size_t>(PostCause::Count)> postsBy{};
        std::uint64_t submits = 0;
        std::uint64_t submitFailures = 0;
        std::uint64_t settleWaits = 0;
        std::uint64_t settleWaitNs = 0;
        std::uint64_t presentSettleWaits = 0;
        std::uint64_t presentSettleWaitNs = 0;
    };
    Statistics Counts() const;
    static void SetDelayForTesting(std::function<void()> delay);
    class UnwindQuiesce {
    public:
        explicit UnwindQuiesce(CommandEncoder* encoder) : encoder(encoder), exceptions(std::uncaught_exceptions()) {}
        ~UnwindQuiesce() {
            if (encoder != nullptr && std::uncaught_exceptions() > exceptions) encoder->Quiesce();
        }
        UnwindQuiesce(const UnwindQuiesce&) = delete;
        UnwindQuiesce& operator=(const UnwindQuiesce&) = delete;

    private:
        CommandEncoder* encoder;
        int exceptions;
    };

private:
    struct Chunk {
        std::vector<std::byte> bytes;
        std::size_t used = 0;
        std::size_t records = 0;
        std::size_t windows = 0;
        VkCommandBuffer target = VK_NULL_HANDLE;
        Recording* recording = nullptr;
        SubmitRequest* submit = nullptr;
        std::atomic<Chunk*> next{nullptr};
    };
    struct Window {
        VkCommandBuffer commands;
        Recording* recording;
    };
    CommandEncoder(VkDevice device, DeviceFunctions& table, std::size_t slot);
    static std::size_t claim();
    void post(PostCause cause) noexcept;
    void enqueue(Chunk* chunk) noexcept;
    Chunk* dequeue() noexcept;
    void publish(Chunk* chunk, PostCause cause) noexcept;
    void park() noexcept;
    void reclaim() noexcept;
    void flushParked() noexcept;
    Chunk* takeSpare() noexcept;
    void giveSpare(Chunk* chunk) noexcept;
    void run();
    void execute(Chunk& chunk);
    void submit(SubmitRequest& request) noexcept;
    void settle(SubmitRequest& request, SubmitRequest::State state) noexcept;
    void await() noexcept;
    void awaitSpare() noexcept;
    DeviceFunctions& table;
    DeviceFunctions real;
    VkDevice device = VK_NULL_HANDLE;
    std::size_t slot = Slots;
    std::vector<Window> windows;
    std::atomic<VkCommandBuffer> target{VK_NULL_HANDLE};
    std::atomic<VkCommandBuffer> recent{VK_NULL_HANDLE};
    Recording* recording = nullptr;
    Chunk* filling = nullptr;
    Chunk stub;
    Chunk stopMark;
    Chunk busyMark;
    std::atomic<Chunk*> tail{&stub};
    Chunk* head = &stub;
    std::atomic<Chunk*> parked{nullptr};
    std::atomic<Chunk*> spares{nullptr};
    Chunk* ownSpares = nullptr;
    std::size_t chunkCount = 0;
    std::atomic<std::uint64_t> posted{0};
    std::atomic<std::uint64_t> executed{0};
    std::atomic<bool> sleeping{false};
    std::atomic<std::uint32_t> awaiting{0};
    std::atomic<bool> stopping{false};
    AgcDriver::Mutex poolMutex;
    std::thread worker;
    std::atomic<std::thread::id> owner{};
    std::atomic<std::uint64_t> postCount{0};
    std::atomic<std::uint64_t> commandCount{0};
    std::uint64_t appended = 0;
    std::atomic<std::uint64_t> waitCount{0};
    std::atomic<std::uint64_t> waitNs{0};
    std::atomic<std::uint64_t> encodeNs{0};
    std::atomic<std::uint64_t> windowCount{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(PostCause::Count)> postsBy{};
    std::atomic<std::uint64_t> submitsPosted{0};
    std::atomic<std::uint64_t> submitsDone{0};
    std::atomic<std::uint32_t> settling{0};
    bool submitFailed = false;
    bool handedOff = false;
    std::atomic<std::uint64_t> submitCount{0};
    std::atomic<std::uint64_t> submitFailures{0};
    std::array<std::atomic<std::uint64_t>, 2> settleWaits{};
    std::array<std::atomic<std::uint64_t>, 2> settleWaitNs{};
};

}

#endif
