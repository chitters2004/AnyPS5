#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_LABELPHASES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_LABELPHASES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace AgcDriver::LabelPhases {

enum class Phase : std::size_t { Pre, Reap, Lookup, Record, Notes, Tail, Count };

constexpr std::size_t PhaseCount = static_cast<std::size_t>(Phase::Count);

enum class ReapTry : std::size_t { Reaped, Failed, Count };

enum class PreSlot : std::size_t { DrawnLinear, Readback, Count };

constexpr std::size_t PreSlotCount = static_cast<std::size_t>(PreSlot::Count);

struct Totals {
    std::uint64_t labels = 0;
    std::uint64_t holdNs = 0;
    std::array<std::uint64_t, PhaseCount> ns{};
    std::array<std::uint64_t, PhaseCount> counts{};
    std::array<std::uint64_t, static_cast<std::size_t>(ReapTry::Count)> reapTries{};
    std::array<std::uint64_t, static_cast<std::size_t>(ReapTry::Count)> reapTryNs{};
    std::array<std::uint64_t, PreSlotCount> preNs{};
    std::array<std::uint64_t, PreSlotCount> preCalls{};
    std::uint64_t drawnStored = 0;
    std::uint64_t readbackScanned = 0;
    std::uint64_t readbackFlushed = 0;
    std::uint64_t labelReaps = 0;
    std::uint64_t labelReapsWithWork = 0;
    std::uint64_t labelReapRetired = 0;
    std::uint64_t labelReapInFlight = 0;
    std::uint64_t labelReapsGated = 0;
};

bool Enabled();
void SetEnabled(bool enabled);
void Begin();
void Mark(Phase phase);
void End();
void CountReapTry(ReapTry outcome, std::uint64_t ns);
bool Active();
void CountDrawnStored(std::uint64_t stored);
void CountReadback(std::uint64_t scanned, std::uint64_t flushed);
void CountLabelReap(std::uint64_t inFlightBefore, std::uint64_t inFlightAfter, bool gated = false);

class PreTimer {
public:
    explicit PreTimer(PreSlot slot);
    ~PreTimer();
    PreTimer(const PreTimer&) = delete;
    PreTimer& operator=(const PreTimer&) = delete;

private:
    PreSlot slot;
    std::uint64_t started = 0;
    bool on = false;
};
Totals Take();
std::string Line(const Totals& totals);

}

#endif
