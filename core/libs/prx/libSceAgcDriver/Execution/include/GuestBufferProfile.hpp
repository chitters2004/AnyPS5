#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTBUFFERPROFILE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTBUFFERPROFILE_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace AgcDriver::GuestBufferProfile {

inline constexpr std::size_t PageShift = 16;
inline constexpr std::size_t WorkBins = 5;
inline constexpr std::size_t GroupBins = 4;
inline constexpr std::size_t PairLimit = 256;

bool Enabled();
void NoteCommandWrite(std::uint64_t address, std::size_t bytes);
void NoteSubmitted(std::uint64_t serial, std::uint32_t queue, const std::uint32_t* commands, std::size_t dwords, bool flip, bool cpuFlip);
void NoteRewindTail(std::uint64_t serial, std::uint32_t queue, const std::uint32_t* commands, std::size_t dwords, bool flip);
void NoteFlip();
void EndSegment();
enum class PacketEffect : std::uint8_t { Other, Draw, Dispatch, Wait, Label };

bool Recording();
void NotePacket(std::uint32_t opcode, std::span<const std::uint32_t> packet);
void NotePacketEffect(PacketEffect effect, std::uint64_t address = 0);
void NoteStoreSignal(std::uint64_t address);
void NoteRange(std::uint64_t address, std::size_t bytes, bool write);
void NoteWaitNs(std::uint64_t nanoseconds);
std::uint64_t Current();
void AddCommitterNs(std::uint64_t id, std::uint64_t nanoseconds);

class SubmitGroup {
public:
    explicit SubmitGroup(std::uint32_t count);
    ~SubmitGroup();
    SubmitGroup(const SubmitGroup&) = delete;
    SubmitGroup& operator=(const SubmitGroup&) = delete;
private:
    bool active = false;
};

class Execution {
public:
    explicit Execution(std::uint64_t serial);
    ~Execution();
    Execution(const Execution&) = delete;
    Execution& operator=(const Execution&) = delete;
private:
    bool active = false;
};

struct Totals {
    std::uint64_t frames = 0;
    std::uint64_t rewindTails = 0;
    std::array<std::uint64_t, 2> submissions{};
    std::array<std::uint64_t, 2> buffers{};
    std::array<std::uint64_t, 2> mostBuffers{};
    std::array<std::uint64_t, GroupBins> buffersPerSubmission{};
    std::array<std::uint64_t, WorkBins> draws{};
    std::array<std::uint64_t, WorkBins> dispatches{};
    std::uint64_t submitThreads = 0;
    std::uint64_t mostSubmitThreads = 0;
    std::uint64_t buildThreads = 0;
    std::uint64_t mostBuildThreads = 0;
    std::uint64_t builtElsewhere = 0;
    std::uint64_t unbuilt = 0;
    std::uint64_t pairs = 0;
    std::uint64_t conflictingPairs = 0;
    std::uint64_t conflictingBuffers = 0;
    std::uint64_t labelEdges = 0;
    std::uint64_t ownWaits = 0;
    std::uint64_t outsideWaits = 0;
    std::uint64_t largeFrames = 0;
    std::uint64_t cyclicFrames = 0;
    std::uint64_t truncatedRanges = 0;
    std::uint64_t workerNs = 0;
    std::uint64_t committerNs = 0;
    std::uint64_t lateCommitterNs = 0;
    std::uint64_t criticalNs = 0;
    std::uint64_t largestNs = 0;
    std::uint64_t coveringBuffers = 0;
};

struct HeldFrames {
    std::uint64_t frames = 0;
    std::uint64_t oldestAge = 0;
    std::uint64_t running = 0;
    std::uint64_t unexecuted = 0;
    std::uint64_t holderSerial = 0;
    std::uint64_t holderSegment = 0;
    std::uint32_t holderQueue = 0;
    std::uint64_t holderAge = 0;
    std::uint64_t holderNs = 0;
};

Totals Closed();
HeldFrames Held();
void CloseFinished();

}

#endif
