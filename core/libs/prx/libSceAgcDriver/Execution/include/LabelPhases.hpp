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

struct Totals {
    std::uint64_t labels = 0;
    std::uint64_t holdNs = 0;
    std::array<std::uint64_t, PhaseCount> ns{};
    std::array<std::uint64_t, PhaseCount> counts{};
    std::array<std::uint64_t, static_cast<std::size_t>(ReapTry::Count)> reapTries{};
    std::array<std::uint64_t, static_cast<std::size_t>(ReapTry::Count)> reapTryNs{};
};

bool Enabled();
void SetEnabled(bool enabled);
void Begin();
void Mark(Phase phase);
void End();
void CountReapTry(ReapTry outcome, std::uint64_t ns);
Totals Take();
std::string Line(const Totals& totals);

}

#endif
