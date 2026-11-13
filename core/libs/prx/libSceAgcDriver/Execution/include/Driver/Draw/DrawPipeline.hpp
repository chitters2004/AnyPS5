#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPIPELINE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPIPELINE_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/SequenceRing.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

class DrawPipeline {
public:
    using Commit = std::function<void()>;
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    struct Watch {
        std::uint64_t retired = 0;
        std::size_t items = 0;
        std::vector<Range> writes;
    };
    enum class DrainReason : std::uint8_t { Packet, Flush, Labels, Capture, Indirect, Submission, Count };
    enum class CaptureSite : std::uint8_t { Decode, Validate, Query, Fill, Shader, Count };
    enum class QueuedWrite : std::uint8_t { None, Stored, Unknown };
    enum class CommitKind : std::uint8_t { Other, Draw, IndirectDraw, ColorMetadata, Dispatch, SampleDump, DmaFill, DmaWrite, DmaCopy, Store, EndOfPipe, Seal, SubmitEnd, Completion, BufferFill, Count };

    struct Totals {
        std::uint64_t commits = 0, commitNs = 0, drawCommits = 0, drawNs = 0, enqueued = 0, fullWaitNs = 0, wakes = 0, drains = 0, drainWaitNs = 0;
    };

    static DrawPipeline& Queue0();
    static std::size_t Depth();
    static std::size_t QueuedItems();
    static bool& Active();
    static bool SeesQueuedWrites();
    static std::atomic<std::uint64_t>& EpochToken();
    static std::array<std::atomic<std::uint64_t>, 5>& EpochTokenCauses();
    static void FollowEpoch(std::uint64_t token);
    static unsigned Committers();

    void Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress = 0, std::vector<std::byte> labelBytes = {}, std::vector<Range> reads = {}, bool draw = false);
    void Enqueue(CommitKind kind, Commit commit, std::vector<Range> writes, std::uint64_t labelAddress = 0, std::vector<std::byte> labelBytes = {}, std::vector<Range> reads = {}, bool draw = false);
    void EnqueueStore(Commit commit, std::uint64_t address, std::vector<std::byte> bytes);
    static void NoteDrawKey(std::uint64_t key, std::uint64_t shape);
    enum NullPixelKind : std::uint32_t { NullPixelRectList = 1u, NullPixelDepthClear = 2u, NullPixelStencilClear = 4u, NullPixelDepthWrite = 8u, NullPixelStencilTest = 16u, NullPixelNoDepthTarget = 32u, NullPixelIndirect = 64u, NullPixelKindCount = 7u };
    static void NoteDecodedDraw(bool nullPixel, std::uint32_t kinds);
    enum class DbClearBlocker : std::uint8_t { StencilTest, SampleMask, Layered, Unproven, Count };
    static DbClearBlocker DbClearBlockerOf(bool stencilClear, bool stencilTest, bool partialSampleMask, bool layered);
    static void NoteDbClear(bool depth, bool stencil, DbClearBlocker blocker, std::uint64_t vertexProgram);
    void Drain(DrainReason reason, std::uint32_t opcode = 0x100);
    void DrainThrough(std::span<const Range> reads, CaptureSite site, std::uint32_t opcode = 0x100);
    void Stop();
    void ReleaseRetired();
    static std::uint64_t DrainRequestsByOpcode(std::uint32_t opcode);
    static std::uint64_t DrainRequestsByReason(DrainReason reason);
    bool Busy() const;
    Totals LifetimeTotals();
    std::size_t Outstanding() const { return outstanding.load(std::memory_order_acquire); }
    bool Overlaps(std::uint64_t address, std::size_t bytes);
    bool OverlapsAny(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);
    Watch WatchWrites();
    bool WrittenSince(const Watch& watch, const std::vector<Range>& reads);
    std::uint64_t Retired() const { return retiredItems.load(std::memory_order_acquire); }
    std::uint64_t CommittedSequence() const { return committedSequence.load(std::memory_order_acquire); }
    void NoteDecodeDrain() { decodeDrains.fetch_add(1, std::memory_order_relaxed); }
    std::uint64_t DecodeDrains() const { return decodeDrains.load(std::memory_order_relaxed); }
    std::optional<std::uint64_t> PendingLabel(std::uint64_t address, std::size_t bytes);
    static bool HeldReads(std::uint64_t address, std::size_t bytes);
    static bool HoldForOlderReads();
    static std::uint64_t HeldReadQueries();
    void ReleaseRunningReads();
    QueuedWrite StoredBytes(std::uint64_t address, std::size_t bytes, std::span<std::byte> out);

    static SequenceRing::Publish TakePreparerOutbox();
    static void FlushPreparerOutbox();
    static bool PreparerDirect();
    static bool MetadataPassesPipelined();
    static void CountPipelinedMetadataPass();
    static std::uint64_t PipelinedMetadataPasses();
    std::uint64_t CommitBreaks(CommitKind kind, bool declaredWrites) const;
private:
    struct Item {
        Commit commit;
        std::vector<Range> writes;
        std::uint64_t labelAddress = 0;
        std::vector<std::byte> labelBytes;
        std::vector<Range> reads;
        bool draw = false;
        bool store = false;
        CommitKind kind = CommitKind::Other;
        std::uint64_t sequence = 0;
    };
    DrawPipeline() = default;
    void enqueue(Item item);
    void admit(Item item);
    static std::vector<Item>& preparerOutbox();
    static bool& preparerDirect();
    void run();
    void rethrowFailure();
    void report(std::chrono::steady_clock::time_point now);
    void countWrites(const std::vector<Range>& writes, bool added);

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Item> items;
    std::vector<std::pair<Range, std::uint32_t>> queuedWrites;
    std::vector<Item> retired;
    std::exception_ptr failure;
    std::atomic<std::size_t> outstanding{0};
    std::atomic<std::uint64_t> retiredItems{0};
    std::atomic<std::uint64_t> committedSequence{0};
    std::uint64_t admittedSequence = 0;
    std::atomic<std::uint64_t> decodeDrains{0};
    std::uint64_t reportedDecodeDrains = 0;
    std::uint32_t idleWaiters = 0;
    std::uint32_t throughWaiters = 0;
    std::size_t resumeAt = 0;
    bool committerWaiting = false;
    bool stopped = false;
    bool closing = false;
    std::uint64_t wakes = 0;
    std::atomic<std::uint64_t> preparerStarved{0};
    std::atomic<std::uint64_t> preparerStarvedNs{0};
    std::atomic<std::uint64_t> workerStarved{0};
    std::atomic<std::uint64_t> workerStarvedNs{0};
    std::uint64_t lockWaitNs = 0;
    std::thread thread;
    std::uint64_t commits = 0;
    std::uint64_t commitNs = 0;
    std::uint64_t commitErrors = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t fullWaitNs = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrainReason::Count)> drains{};
    std::array<std::uint64_t, static_cast<std::size_t>(DrainReason::Count)> drainWaitNs{};
    std::array<std::uint64_t, 257> drainOpcodes{};
    std::array<std::atomic<std::uint64_t>, 257> drainRequests{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DrainReason::Count)> reasonRequests{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(CommitKind::Count)> kindCommits{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(CommitKind::Count)> kindNs{};
    std::array<std::array<std::atomic<std::uint64_t>, 2>, static_cast<std::size_t>(CommitKind::Count)> kindBreaks{};
    std::array<std::array<std::uint64_t, 2>, static_cast<std::size_t>(CommitKind::Count)> reportedKindBreaks{};
    std::uint64_t depthSum = 0;
    Totals reported;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

class DrawFinisher {
public:
    using Job = std::function<void()>;
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    using Pending = SequenceRing::Held;
    enum class LabelLookup : std::uint8_t { NotHeld, Value, Unknown };
    static bool Overlap();
    bool PendingOverlaps(std::uint64_t begin, std::uint64_t end);
    LabelLookup PendingLabel(std::uint64_t address, std::size_t bytes, std::uint64_t& value);
    bool PendingOverlapsAny(std::span<const Range> ranges);
    bool PendingKey(std::uint64_t key, std::uint64_t shape);
    enum class DrainSite : std::uint32_t { Lookup, Reads, Packet, Kept, End, Count };
    static void NoteDrain(DrainSite site, bool drained);
    static std::string DrainText();
    static void NoteDrainedOpcode(std::uint32_t opcode, bool besideCommits);
    static DrawFinisher& Get();
    static bool Enabled();
    static bool OnFinisherThread();
    static bool Sequenced();
    static std::size_t Preparers();
    static std::string AbWindow(bool begins, char arm, std::uint64_t phase);
    struct PreparerReads {
        std::uint64_t taken = 0;
        std::uint64_t validated = 0;
        std::uint64_t missed = 0;
        std::uint64_t refused = 0;
        std::uint64_t changed = 0;
    };
    static bool CheckPreparerReads(GuestMemory::WriteTicket ticket, std::span<const Range> ranges, std::span<const std::span<const std::byte>> captured, bool turnWaited);
    static PreparerReads PreparerReadsCounted();
    static GuestMemory::WriteTicket PreparerTicket();
    static void HoldNextItemForTest(std::function<void()> hold);
    static void ClearHoldForTest();
    static bool VerifyPreparerReads();
    static bool Buffered();
    void AwaitTurn();
    bool OlderUnpublished();
    static bool Busy() { return Sequenced() ? !Get().Ring().Idle() : outstanding.load(std::memory_order_acquire) != 0; }
    bool Producing();
    SequenceRing& Ring();
    void Enqueue(Job job);
    void Enqueue(Job job, Pending held);
    void Enqueue(Job job, Pending held, std::uint8_t tag);
    std::uint64_t Bypass();
    DrawPipeline::QueuedWrite StoredBytes(std::uint64_t address, std::size_t bytes, std::span<std::byte> out);
    enum class WaitSite : std::uint8_t { Relocation, Lookup, Reads, Kept, Packet, PacketDispatch, PacketSampleDump, Dispatch, End, EnqueueReads, Enqueue, Capture, Pipeline, Shutdown, Other, Count };
    struct SiteWait {
        std::uint64_t waits = 0;
        std::uint64_t waitNs = 0;
        std::uint64_t queued = 0;
    };
    static constexpr std::array<const char*, static_cast<std::size_t>(WaitSite::Count)> WaitSiteNames{"relocation", "lookup", "reads", "kept", "packet", "packet_dispatch", "packet_sample_dump", "dispatch", "end", "enqueue_reads", "enqueue", "capture", "pipeline", "shutdown", "other"};
    static WaitSite PacketWaitSite(std::span<const std::uint32_t> packet);
    void Drain(WaitSite site = WaitSite::Other);
    static std::pair<std::uint64_t, std::uint64_t> DrainWaitTotals();
    static std::array<SiteWait, static_cast<std::size_t>(WaitSite::Count)> SiteWaitTotals();
    static std::uint64_t PreparerRoomWaitNs();
    void Settle();
    void ReleaseRan();
    void RethrowFailure();

private:
    DrawFinisher() = default;
    template<typename TVisit>
    bool visitPending(TVisit&& visit);
    void run();
    inline static std::atomic<std::size_t> outstanding{0};
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Job> jobs;
    std::deque<Pending> pending;
    std::vector<Job> ran;
    std::exception_ptr failure;
    bool running = false;
    bool sleeping = false;
    std::thread thread;
    std::unique_ptr<SequenceRing> ring;
};

}

#endif
