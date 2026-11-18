#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTMEMORY_HPP

#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::GuestMemory {

void CheckRange(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable = false);
// CheckRange without throwing: whether the whole range is mapped with the requested access.
bool Accessible(const void* pointer, std::size_t bytes, bool writable = false);
// The accessible parts [begin, end) of a range, in address order. GPU heaps are often bound whole while
// the guest commits their pages on demand, so a binding can cover reserved but uncommitted pages.
std::vector<std::pair<std::uint64_t, std::uint64_t>> CommittedRanges(std::uint64_t address, std::size_t bytes, bool writable = false);
// Both facts from one walk of the page tables: the accessible parts of a range and whether they cover
// all of it (`whole`, what Accessible answers). Reserved pages are queried each time, so asking twice
// costs two VirtualQuery per run.
struct Commitment {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    bool whole = false;
};
Commitment DescribeCommitted(std::uint64_t address, std::size_t bytes, bool writable = false);
void CopyGuestBytes(void* destination, const void* source, std::size_t bytes);
void Read(std::uint64_t address, std::span<std::byte> destination, std::size_t alignment = 1);
// Variants for resources in heaps the guest commits on demand (partially resident textures, GPU
// scratch heaps): uncommitted pages read as zeros, are not compared, and are never stored.
void ReadCommitted(std::uint64_t address, std::span<std::byte> destination);
void ReadCommittedUnsynced(std::uint64_t address, std::span<std::byte> destination);
bool EqualsCommitted(std::uint64_t address, std::span<const std::byte> bytes);
// EqualsCommitted without the flush hook: the caller has decided that the recorded GPU work
// writing the range need not be waited for (the dispatch-cache validation in Driver.cpp).
bool EqualsCommittedUnsynced(std::uint64_t address, std::span<const std::byte> bytes);
// EqualsCommittedUnsynced for a caller that treats an unmapped page as a miss (the dispatch-cache
// validation's per-run compare): one walk of the page states, one memcmp per mapped run, no profile
// counter and no reason string. Unmapped: some page of the range is not readable (or the address
// space cannot be queried).
enum class Compare : std::uint8_t { Equal, Differs, Unmapped };
Compare CompareMapped(std::uint64_t address, std::span<const std::byte> bytes);
void SetCompareMappedHook(void (*hook)(std::uint64_t address, std::size_t bytes));
Compare CopyMapped(std::uint64_t address, std::span<std::byte> out);
void WriteChangedCommitted(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original);
void Write(std::uint64_t address, std::span<const std::byte> source, std::size_t alignment = 1);
// Stores the parts of `current` that differ from `original` (the guest bytes the GPU started from), so
// guest writes made meanwhile to untouched bytes survive. Compares in 256-byte blocks.
void WriteChanged(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original);
void WriteChanged(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original, std::uint32_t byteStoreLimit);
std::size_t WriteChangedIfHeld(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original, std::uint64_t unchangedSince);
std::uint32_t ChangedByteStoreLimit();
void WriteChangedKeepingGuest(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original);

// Write tracking. The guest arena is reserved with page write watching, so CollectWrites learns which
// pages of a range the CPU wrote since they were last collected, stamps their 64 KiB blocks with a new
// generation and returns it; a snapshot of the range taken after that call is current while
// UnchangedSince(range, that generation) holds. Every validation collects its own range first, so
// resources sharing pages see each other's writes. GPU writes into host-imported memory bypass the
// page tables and are reported with MarkWritten. Without write watching CollectWrites returns 0 and
// UnchangedSince is always false, so callers fall back to comparing bytes.
bool WriteWatched();
void Unwatch(std::uint64_t address, std::size_t bytes);
void Rewatch(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);
bool ImportWatched(std::uint64_t address, std::size_t bytes, const std::function<bool()>& import, bool staysWatched = true, bool importMayWriteGuest = true);
void SetUnwatchHook(void (*hook)(std::uint64_t address, std::size_t bytes));
bool UnwatchWritesProven();
void SetImportWalkFailureForTest(int walk, std::uint64_t span = 0);
#ifdef _WIN32
using FreshCollectHook = bool (*)(std::uintptr_t address, std::size_t bytes, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context);
void SetFreshCollectHook(FreshCollectHook hook);
#endif
bool Watched(std::uint64_t address, std::size_t bytes);
bool OnlyRewatchedSince(std::uint64_t address, std::size_t bytes, std::uint64_t generation);
bool Rewatched(std::uint64_t address, std::size_t bytes);
void RewatchedBlocks(std::uint64_t address, std::size_t bytes, std::span<const std::uint64_t> since, std::span<std::uint8_t> rewatched);
std::uint64_t CollectWrites(std::uint64_t address, std::size_t bytes);
struct WriteSubscription;
enum class WriteBumpSource : std::uint8_t { Cpu, Driver, Pending, Unwatch, Forget, Release, Import, Mirror, Depth, Count };
enum class WriteChange : std::uint8_t { Bytes, Authority, Capability, Identity, Mirror, Count };
constexpr std::uint32_t WriteChangeBit(WriteChange change) {
    return std::uint32_t{1} << static_cast<unsigned>(change);
}
constexpr std::uint32_t WriteChangeAll = WriteChangeBit(WriteChange::Bytes) | WriteChangeBit(WriteChange::Authority) | WriteChangeBit(WriteChange::Capability) | WriteChangeBit(WriteChange::Identity);
WriteChange WriteChangeOf(WriteBumpSource source);
enum class WriteDomain : std::uint8_t { Proof, Unwatch, Mapping, CertifiedProof, Depth, Mirror, Count };
constexpr std::uint32_t WriteDomainBit(WriteDomain domain) {
    return std::uint32_t{1} << static_cast<unsigned>(domain);
}
constexpr std::size_t CertificateInlineOwners = 4;
struct WriteCertificate {
    std::uint64_t version = 0;
    std::uint64_t owners = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(WriteDomain::Count)> domains{};
    const std::atomic<std::uint64_t>* cell = nullptr;
    std::uint32_t typeMask = 0;
    std::uint32_t ownerCount = 0;
    std::array<std::uint64_t, CertificateInlineOwners> ownerHandles{};
    std::uint32_t blockCount = 0;
    bool handles = true;
    std::array<const std::atomic<std::uint64_t>*, CertificateInlineOwners> blockChanges{};
};
bool WriteIndexEnabled();
std::shared_ptr<WriteSubscription> SubscribeWrites(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uint32_t typeMask = WriteChangeAll, bool history = false, bool owned = true);
std::uint64_t SubscriptionGeneration(const WriteSubscription& subscription);
std::uint64_t ObserveSubscription(const WriteSubscription& subscription);
std::uint64_t DomainVersion(WriteDomain domain);
void BumpDomainVersion(WriteDomain domain);
void BumpWrites(std::uint64_t address, std::uint64_t bytes, WriteBumpSource source);
std::uint64_t RegisterWriteOwner(std::uint64_t base, std::uint64_t bytes);
void UnregisterWriteOwner(std::uint64_t owner);
class WriteOwner {
public:
    WriteOwner() = default;
    WriteOwner(std::uint64_t base, std::uint64_t bytes) : handle(RegisterWriteOwner(base, bytes)) {}
    WriteOwner(const WriteOwner&) = delete;
    WriteOwner& operator=(const WriteOwner&) = delete;
    WriteOwner(WriteOwner&& other) noexcept : handle(std::exchange(other.handle, 0)) {}
    WriteOwner& operator=(WriteOwner&& other) noexcept {
        if (this != &other) {
            UnregisterWriteOwner(handle);
            handle = std::exchange(other.handle, 0);
        }
        return *this;
    }
    ~WriteOwner() { UnregisterWriteOwner(handle); }
    std::uint64_t Handle() const { return handle; }

private:
    std::uint64_t handle = 0;
};
WriteCertificate CertifySubscription(const WriteSubscription& subscription, std::uint32_t domainMask);
bool CertificateHolds(const WriteSubscription& subscription, const WriteCertificate& certificate, std::uint32_t domainMask);
constexpr std::uint32_t CertificateVersionMissed = std::uint32_t{1} << static_cast<unsigned>(WriteDomain::Count);
std::uint32_t CertificateMisses(const WriteSubscription& subscription, const WriteCertificate& certificate, std::uint32_t domainMask);
enum class CertificateMutant : std::uint8_t { None, IgnoreVersion, HideUnwatch, HideMapping, HideCertifiedProof, HideMarkWrittenBumps, HideOwnStoreBumps, ReverseDomainOrder, HidePipelineCommitBumps, IgnoreOwners, HideOwnerBumps, ObserveIgnoresOwners, CertificateDropsOwners, HideBlockBumps, CertificateDropsBlocks, CertificateSkipsVersion, HideRecordedPendingBumps };
void SetCertificateMutantForTest(CertificateMutant mutant);
void SetWriteIndexCountingForTest(bool counting);
void SetWriteOwnersIgnoredForTest(bool ignored);
using DomainReadObserver = void (*)(WriteDomain domain);
void SetDomainReadObserverForTest(DomainReadObserver observer);
class CoveredCommitScope {
public:
    explicit CoveredCommitScope(bool covered);
    ~CoveredCommitScope();
    CoveredCommitScope(const CoveredCommitScope&) = delete;
    CoveredCommitScope& operator=(const CoveredCommitScope&) = delete;

private:
    bool muted;
};
using WriteBackingResolver = bool (*)(std::uint64_t address, std::uint64_t bytes, int* file, std::uint64_t* offset);
void SetWriteBackingResolver(WriteBackingResolver resolver);
std::span<const std::pair<std::uint64_t, std::uint64_t>> SubscriptionSpans(const WriteSubscription& subscription);
bool SweepSubscription(const WriteSubscription& subscription);
bool SubscriptionBumpsSince(const WriteSubscription& subscription, std::uint64_t generation, std::vector<std::uint64_t>& blocks, std::vector<std::uint64_t>& lookupBlocks);
bool SubscriptionSourcesSince(const WriteSubscription& subscription, std::uint64_t generation, std::uint32_t& sources);
void TrimSubscriptionBumps(WriteSubscription& subscription, std::uint64_t generation);
std::uint64_t WriteBlockOf(std::uint64_t address);
void NoteGpuPending(std::uint64_t address, std::size_t bytes);
void NoteRecordedPending(std::uint64_t address, std::size_t bytes);
void NoteTextureReleased(std::uint64_t address);
constexpr std::size_t WriteBumpSizeBuckets = 7;
struct WriteIndexStats {
    std::uint64_t calls = 0;
    std::uint64_t subscriptions = 0;
    std::uint64_t blocks = 0;
    std::uint64_t bumps = 0;
    std::uint64_t lookups = 0;
    std::uint64_t sweeps = 0;
    std::uint64_t sweepFailures = 0;
    std::uint64_t overflows = 0;
    std::uint64_t lookupNanoseconds = 0;
    std::uint64_t ownerBumps = 0;
    std::uint64_t ownerFastBumps = 0;
    std::uint64_t ownerDependencies = 0;
    std::uint64_t blockDependencies = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(WriteBumpSource::Count)> bySource{};
    std::array<std::array<std::uint64_t, WriteBumpSizeBuckets>, static_cast<std::size_t>(WriteBumpSource::Count)> bySize{};
};
WriteIndexStats WriteIndexCounted();
void ResetWriteIndexCounts();
bool UnchangedSince(std::uint64_t address, std::size_t bytes, std::uint64_t generation);
// UnchangedSince for several ranges under one tracker lock: true only when every one holds.
struct UnchangedQuery {
    std::uint64_t address;
    std::size_t bytes;
    std::uint64_t generation;
};
bool WrittenSince(std::uint64_t address, std::size_t bytes, std::uint64_t generation);
bool UnchangedSinceAll(std::span<const UnchangedQuery> queries);
std::uint64_t CollectCommittedWrites(std::uint64_t address, std::size_t bytes, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges);
std::uint64_t WriteGeneration();
std::uint64_t CollectWrittenBlocks(std::uint64_t address, std::size_t bytes, std::uint64_t since, std::vector<std::pair<std::uint64_t, std::uint64_t>>& written);
bool UnchangedCommittedSince(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uint64_t generation);
// Returns the generation the blocks were stamped with (0 when the arena is not write-watched):
// UnchangedSince(range, it) holds until the next store over the range.
std::uint64_t MarkWritten(std::uint64_t address, std::size_t bytes);
std::uint64_t MarkPossiblyWritten(std::uint64_t address, std::size_t bytes);
std::uint64_t MarkPossiblyWritten(std::uint64_t address, std::size_t bytes, std::uint64_t& order);
void ForgetKnownZero(std::uint64_t address, std::size_t bytes);
void MaterializeKnownZero(std::uint64_t address, std::size_t bytes);
std::uint64_t StoreOwnBytes(std::uint64_t address, std::size_t bytes, const std::function<void()>& store);
std::uint64_t StoreOwnBytes(std::uint64_t address, std::size_t bytes, const std::function<void(std::span<std::byte>)>& store);
std::uint64_t StoreOwnBytes(std::uint64_t address, std::span<const std::byte> source);
void SetOwnStoreCompare(bool compare);
bool StoredOver(std::uint64_t address, std::size_t bytes, std::uint64_t generation);
bool StoredAllSince_nid_no_patch(std::uint64_t address, std::size_t bytes, std::uint64_t generation);
// Collect epoch: within one epoch a range already collected is not walked again, CollectWrites
// returns the current generation instead. A guest write landing between two collects of the same
// epoch is seen by the next epoch, which is the ordering real hardware gives a CPU write made while a
// submission runs. The driver's own writes (Write, WriteChanged, MarkWritten) stamp their blocks, so
// they are never hidden. APS5_NO_COLLECT_MEMO=1 disables the memo. CollectWritesUncached always walks:
// for decisions about which blocks the CPU wrote while a GPU result was pending (a stale answer there
// stores old texels over reused memory). The collect that starts such a pending period
// (StorageTexture::markLayersPending) may be memoized: its caller reads TrackerGeneration after it,
// so a store the memoized walk missed is stamped above that generation by a later walk (at the
// latest the decision's own uncached one) and counts as made while the result was pending, while a
// store ordered before the draw was made before the epoch's ordering point, so the memoized walk,
// which started after that point, stamped it. APS5_NO_MEMO_PENDING_COLLECT walks there again.
// The epoch is per queue worker: BumpCollectEpoch advances the calling thread's epoch to a value no
// other thread ever holds. Each 64 KiB block records the counter value read before its last complete
// walk, and a request of epoch e is answered by any walk of the block made at or after e, by
// whichever thread: such a walk started after the bump that produced e. A mapping or unwatch change
// invalidates every recorded walk. The calling thread's epoch advances at the hardware
// ordering points of its queue: the start of a submission, a WAIT_REG_MEM the CPU may have satisfied
// (at entry, polled, timed out, or by another queue's recorded label; a label this queue recorded
// itself orders no CPU write), a device drain, and before reaping completions outside a submission.
// A thread that never bumps (the presenter, game threads inside the flush hook) gets a fresh epoch
// for every collect: nothing is reused without an ordering point. APS5_PACKET_EPOCH=1 makes the
// workers bump before every packet as before. CollectEpochBumps counts the bumps ([guestmem] line).
enum class EpochBumpSource : std::uint8_t { Unlabelled, Submission, Packet, Drain, Wait, Reap, Followed, Count };
const char* EpochBumpSourceName(EpochBumpSource source);
void BumpCollectEpoch();
void BumpCollectEpoch(EpochBumpSource source);
EpochBumpSource LastCollectEpochSource();
void AdoptLatestCollectEpoch();
std::uint64_t ProofSerial();
enum class ProofBreakSite : std::uint8_t { Other, DriverStore, Written, GpuMutex, WriteBack, WriteBackIfHeld, Packet, CompletionStore, RecordedWork, WrittenBack, KeptDraw, ResourceWriteBack, PacketCovered, GpuLabel, HeldState };
const char* ProofBreakSiteName(ProofBreakSite site);
inline constexpr std::size_t ProofBreakSiteCount = static_cast<std::size_t>(ProofBreakSite::HeldState) + 1;
struct ProofBreakCounts {
    std::array<std::uint64_t, ProofBreakSiteCount> sites{};
    std::uint64_t emptyWriteBacks = 0;
    std::uint64_t unchangedWriteBacks = 0;
    std::uint64_t offThreadHolds = 0;
    std::uint64_t idleHolds = 0;
};
bool ProofBreaksCounted();
void NoteHoldPendingBump();
std::uint64_t SkippedHoldBreaks();
void HoldProofBreaksForTest(bool held);
ProofBreakCounts TakeProofBreakCounts();
bool ProofBreakSitesTracked();
std::array<std::uint64_t, ProofBreakSiteCount> ProofBreakSiteTotals();
void BreakProofs(ProofBreakSite site = ProofBreakSite::Other, std::uint64_t address = 0, std::uint64_t bytes = 0);
bool ProofBreakVoidsCertificates(ProofBreakSite site);
std::uint64_t CertificateBreakSerial();
void BreakProofsOver(ProofBreakSite site, std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uint8_t detail = 0);
void BreakProofsWithoutWrites(ProofBreakSite site, std::uint8_t detail = 0);
class RangeStampScope {
public:
    RangeStampScope();
    ~RangeStampScope();
    RangeStampScope(const RangeStampScope&) = delete;
    RangeStampScope& operator=(const RangeStampScope&) = delete;
};
void StampPendingEntering(std::uint64_t begin, std::uint64_t end, std::uint64_t serial);
std::uint64_t UnrangedProofSerial();
std::uint64_t UnrangedPendingSerial();
struct RangeVersionBaseline {
    std::uint64_t generation = 0;
    std::uint64_t proof = 0;
    std::uint64_t pending = 0;
    std::uint64_t unrangedProof = 0;
    std::uint64_t unrangedPending = 0;
};
enum class RangeVersionDecline : std::uint8_t { None, Baseline, Wrapped, Uncovered, CollectUnwatched, Collect, Changed, ChangedPossibly, ChangedBlock, ChangedRefused, InFlight, UnrangedProof, UnrangedPending, Effect, Pending, Count };
const char* RangeVersionDeclineName(RangeVersionDecline decline);
bool SnapshotUnchangedSince(std::uint64_t address, std::span<std::byte> out, std::uint64_t generation);
std::uint64_t SnapshotMatches(std::uint64_t address, std::span<const std::byte> snapshot);
struct RangeValidation {
    std::function<bool(std::size_t index, std::uint64_t begin, std::uint64_t end)> bytesUnchanged;
    std::span<const std::uint64_t> generations;
    std::size_t passedOnBytes = 0;
    std::size_t passedOnRebase = 0;
    std::pair<std::uint64_t, std::uint64_t> declined{0, 0};
};
RangeVersionDecline ValidateProofRanges(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, const RangeVersionBaseline& baseline, RangeValidation* validation = nullptr);
enum class RangeWriteSource : std::uint8_t { CpuPage, WalkPage, DriverBlock, DriverPiece, Count };
struct RangeWriteShape {
    std::uint64_t rangeBegin = 0;
    std::uint64_t rangeEnd = 0;
    std::uint64_t writeBegin = 0;
    std::uint64_t writeEnd = 0;
    std::uint64_t writtenBytes = 0;
    std::array<std::uint32_t, static_cast<std::size_t>(RangeWriteSource::Count)> units{};
    bool straddlesOnly = false;
};
RangeWriteShape DescribeRangeWrite(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uint64_t generation);
inline constexpr std::size_t ProofBreakDetails = 16;
enum class PacketWrites : std::uint8_t { Declared, None, Empty, Writeless, Count };
bool ProofBreakCensusEnabled();
std::array<std::array<std::uint64_t, static_cast<std::size_t>(PacketWrites::Count)>, ProofBreakDetails> TakePacketBreakCounts();
struct ProofBreakCensus {
    bool complete = false;
    bool overwritten = false;
    std::uint64_t breaks = 0;
    std::uint32_t noWritePackets = 0;
    std::uint32_t emptyRangePackets = 0;
    std::uint32_t overlapPackets = 0;
    std::uint32_t unrangedSites = 0;
    std::uint32_t overlapSites = 0;
    std::uint32_t outsideUnscopedSites = 0;
};
ProofBreakCensus ProofBreakCensusSince(std::uint64_t serial, std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);
bool ProofBreakScoped(ProofBreakSite site);
struct ProofBreakWindow {
    bool complete = false;
    bool clear = false;
    bool unranged = false;
    ProofBreakSite blocker = ProofBreakSite::Other;
    std::uint64_t breaks = 0;
};
ProofBreakWindow ProofBreaksSince(std::uint64_t serial, std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);
ProofBreakWindow ProofBreaksSinceSimulated(std::uint64_t serial, std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);
struct ProofBreak {
    ProofBreakSite site = ProofBreakSite::Other;
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
};
class ProofBreakTrace {
public:
    ProofBreakTrace();
    ~ProofBreakTrace();
    ProofBreakTrace(const ProofBreakTrace&) = delete;
    ProofBreakTrace& operator=(const ProofBreakTrace&) = delete;
    void Note(const ProofBreak& entry);
    const std::vector<ProofBreak>& Breaks() const { return breaks; }
    std::uint64_t Count() const { return count; }

private:
    std::vector<ProofBreak> breaks;
    std::uint64_t count = 0;
    ProofBreakTrace* previous = nullptr;
};
void MarkProofThread();
bool OnProofThread();
std::uint64_t CollectEpoch();
std::uint64_t CollectEpochBumps();
void NoteFollowedEpoch();
std::uint64_t FollowedEpochBumps();
std::uint64_t ThreadCollectEpoch();
std::uint64_t UnwatchSerial();
std::uint64_t CollectWritesUncached(std::uint64_t address, std::size_t bytes);
struct Observation {
    enum class Status { Complete, Refused, Uncovered, Failed };
    Status status = Status::Failed;
    std::uint64_t generation = 0;
};
Observation Observe(std::uint64_t address, std::size_t bytes);
void SetObservationHookForTest(void (*hook)(int point));
// The tracker's current generation (every collect and MarkWritten bumps it): a stamp taken after
// a set of driver stores, for UnchangedSinceCollected to compare against later. Read under the
// tracker mutex, so every collect that bumped before it has finished its walk and every later
// collect stamps newer.
std::uint64_t TrackerGeneration();
void AdvanceTrackerGenerationForTest(std::uint64_t generation);
std::uint64_t NewestStamp(std::uint64_t address, std::size_t bytes);
std::uint64_t UntrackedHint(std::uint64_t address, std::size_t bytes);
// Whether no CPU write touched the range since `generation`: an uncached resetting collect of the
// range's pages first (a game store not collected yet becomes a block stamp), then the compare over
// the stamps collects make (dirty pages), not the MarkWritten stamps of the driver's own GPU label
// records, which share the title's label blocks. False when the arena is not write-watched.
bool UnchangedSinceCollected(std::uint64_t address, std::size_t bytes, std::uint64_t generation);
bool CpuWroteSince(std::uint64_t address, std::size_t bytes, std::uint64_t generation);
bool UnchangedSinceLastCollect(std::uint64_t address, std::size_t bytes, std::uint64_t generation);
// Per 64 KiB tracker block of [address, address + bytes) (block 0 holds `address`): `changed[k]`
// receives whether the block was stamped after generations[k] (UnchangedSince over that block
// alone), `cpu[k]`, when given, whether a collect stamped it after (a CPU store, as
// UnchangedSinceCollected tells them apart from the driver's MarkWritten stamps, but without its
// walk: the caller collected the range first). One tracker lock for the whole range. Outside the
// watched arena, or for a generation of 0, a block reads as changed.
constexpr std::uint8_t BlockUnchanged = 0;
constexpr std::uint8_t BlockWritten = 1;
bool ChangedBlocks(std::uint64_t address, std::size_t bytes, std::span<const std::uint64_t> generations, std::span<std::uint8_t> changed, std::span<std::uint8_t> cpu = {});

// Serializes device work: draws, dispatches, presentation and the deferred write-backs below. The
// mutex is recursive; it is wrapped so every acquisition (std::lock_guard at any site) measures how
// long it waited: the graphics worker's frame is mostly spent waiting for this lock behind the
// compute workers, and that wait has to stay visible. Under APS5_PROFILE_DRAW each thread reports its
// waits in a [lock] line every 10 s (APS5_NO_LOCK_PROFILE=1 turns the timing off); a queue worker tags
// its thread with TagGpuLockThread so the line names the queue.
class GpuMutexType {
public:
    void lock();
    bool try_lock();
    void unlock();
    // Whether the calling thread holds the mutex (at any depth): the APS5_ASSERT_GPU_LOCK checks
    // (AssertGpuLockHeld) and code that must know whether it runs inside the device lock.
    bool HeldByThisThread() const;
    // The calling thread's recursion depth (0 when it does not hold the mutex): 1 means its
    // current acquisition is the outermost, so code that releases the mutex around a wait
    // (Recorder::SyncThrough for the flush hook) gives up no caller's hold.
    std::uint32_t DepthOnThisThread() const;
    std::uint32_t Waiters() const { return waiters.load(std::memory_order_relaxed); }

private:
    void enter();
    void block();
    bool tryEnter();
    void acquired();
    Mutex mutex;
    std::atomic<std::uint32_t> waiters{0};
    // The holder's thread token and recursion depth, written by the holder only; another thread
    // reads the owner just to see that it is not itself.
    std::atomic<const void*> owner{nullptr};
    std::uint32_t depth = 0;
};
GpuMutexType& GpuMutex();
std::function<bool()> YieldToGpuMutexWaiters(std::function<bool()> stop);
// Called on the thread that just gave up its outermost hold, right after the mutex was released:
// work the hold deferred to run without it (the recorder's release of the objects completed batches
// kept). One hook, set once; it must not take GpuMutex itself.
void SetGpuUnlockHook(void (*hook)());
std::uint64_t GpuHoldSerial();
// Debug aid (APS5_ASSERT_GPU_LOCK=1): aborts with a [lock] line naming `where` when the calling
// thread does not hold GpuMutex. Recording, submits, command batches and the detiler's pools must
// only be reached under it (a resource build's stage A runs without it, see ShaderResources).
void AssertGpuLockHeld(const char* where);
void TagGpuLockThread(std::uint32_t queue);
// The presenter stays untagged (its tag is a queue id to the recorder) but takes its own column
// of the [lock] holder matrix, apart from the game threads.
void MarkPresenterThread();
// The queue tag of the calling thread (0xffffffff when untagged), for reports that name the thread.
std::uint32_t GpuLockThreadTag();
// Lock sites (APS5_PROFILE_DRAW): a caller names the site of its next lock() so the [lock] line
// splits acquisitions and waits per site (whose holds queue 0 waits behind is then measurable). The
// tag is consumed by that acquisition, so a nested lock inside the flush hook is attributed to the
// hook, and an acquisition nobody tagged counts as "other". Try-only acquisitions are not counted
// as acquisitions; the hold they start is timed under "try". Holds are timed per site as well (the
// outermost acquisition of a thread until its unlock; APS5_NO_HOLD_PROFILE=1 turns that off), so
// the line shows who holds the mutex, not only who waits for it.
enum class GpuLockSite : std::uint8_t { Other = 0, Dispatch, Indirect, Draw, Hook, Wait, Label, Flush, Present, Fill, Copy, End, Try, Count };
void TagGpuLockSite(GpuLockSite site);
// A GPU wait (a fence or timeline wait that found its work unfinished) made while the calling
// thread holds GpuMutex: counted on the thread's [lock] line as 'locked GPU waits' (APS5_PROFILE_DRAW).
void NoteLockedGpuWait(double ms);
// Bytes the calling thread's write-watch collects walked so far (0 without the memory profile):
// a caller reads the difference across a span of its own work.
std::uint64_t ThreadCollectedBytes();
// A code address (a return address) as an offset into its module, in the form of the [guestmem]
// callers (symbolized with nm against the driver's image). Takes the loader lock: for reports only.
unsigned long long CodeOffset(const void* address);

// Deferred GPU writes. The graphics layer keeps the results of storage images on the GPU until guest
// memory is read; it registers a hook that stores every pending result overlapping a range, which
// every read and store through this interface calls first. Code that reads guest memory directly
// calls FlushGpuWrites itself.
void SetFlushHook(void (*hook)(std::uint64_t address, std::size_t bytes));
auto CurrentFlushHook() -> void (*)(std::uint64_t address, std::size_t bytes);
void SetReadGuard(void (*guard)(std::uint64_t address, std::size_t bytes));
void GuardRead(std::uint64_t address, std::size_t bytes);
void FlushGpuWrites(std::uint64_t address, std::size_t bytes);
void PrepareHostWrite(std::uintptr_t address, std::size_t bytes);
bool StopHostWrites(std::uint32_t boundMs);
std::uint64_t LockedHostWrites();
struct HostWriteCounts {
    std::uint64_t calls = 0;
    std::uint64_t waited = 0;
    double waitedMs = 0;
    double maxWaitedMs = 0;
};
HostWriteCounts HostWritesCounted();
void SetWaitedMsProvider(double (*provider)());
std::string HostWriteReport();
// Moves with every ForgetPages call (the libc invalidator: memory unmapped or re-registered): odd
// while the call stores its page states, moved past it after, so a reader that loads an even
// value before consulting the page states and reading directly, and the same value after, knows
// the states it consulted were current and no page it read changed its mapping meanwhile.
std::uint64_t ForgetSerial();
std::uint64_t ForgetStamp(std::uint64_t address, std::size_t bytes);

// Attribution of guest memory accesses (the [hooksync] line in Recorder.cpp and the read-site
// counts of the [guestmem] line, APS5_PROFILE_DRAW): a sync the flush hook makes for an access has
// to be charged to the PM4 packet and the driver code that made the access. A queue worker names
// the packet it executes at the top of its packet loop (SetCurrentPacket; opcode 0xffff is the
// flip); a thread that runs no packets (the presenter, game threads) reads back NoPacket. The main
// read paths name themselves with a ReadSiteScope; an access made by none of them is attributed by
// its return addresses instead (CaptureCallerOffsets).
struct PacketTag {
    std::uint32_t opcode;
    std::uint32_t queue;
};
constexpr std::uint32_t NoPacket = 0xfffffffeu;
void SetCurrentPacket(std::uint32_t opcode, std::uint32_t queue);
PacketTag CurrentPacket();

// Store: a CPU write (Write/WriteChanged) rather than a read; a hook sync made for one orders a
// write after a pending GPU write, so whether the bytes change says nothing about its necessity.
// MirrorRefresh: an address-based build bringing an image mirror up to date with guest memory
// (GuestBufferMemory.cpp: the flush before a writable mirror's compare, a new mirror's fill).
// DrawCache: a draw entry's per-stage value validation (Driver.cpp drawCache), where a draw
// capture's hook waits reappear on a hit.
// Every enumerator needs its name in ReadSiteName's table (GuestMemory.cpp).
enum class ReadSite : std::uint8_t { Unknown = 0, Capture, DispatchCache, TextureCompare, TextureRead, BufferUpload, IndexBuffer, VertexBuffer, Registers, IndirectArguments, Wait, Label, Scanout, Store, MirrorRefresh, DrawCache, HostWrite, Count };
const char* ReadSiteName(ReadSite site);
bool WritesGuestMemory(ReadSite site);
// Sets the calling thread's read site and returns the previous one (nested scopes restore it).
ReadSite SetReadSite(ReadSite site);
ReadSite CurrentReadSite();
class ReadSiteScope {
public:
    explicit ReadSiteScope(ReadSite site) : previous(SetReadSite(site)) {}
    ~ReadSiteScope() { SetReadSite(previous); }
    ReadSiteScope(const ReadSiteScope&) = delete;
    ReadSiteScope& operator=(const ReadSiteScope&) = delete;

private:
    ReadSite previous;
};
// Return addresses above the caller as offsets into the driver's module (the same form as the
// [guestmem] callers, symbolized with nm against the driver's image; an address outside the module
// is written as 0), newest first, skipping `skip` frames above the caller; returns how many were
// written. Stack unwinding, so only for rare, profiled events.
std::size_t CaptureCallerOffsets(std::span<unsigned long long> frames, unsigned skip);

}

extern "C" void AgcDriverCheckGuestMemory_nid_postfix(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable = false);
extern "C" void AgcDriverStoreGuestLabel_nid_postfix(std::uint64_t* pointer, std::uint64_t value);

#endif
