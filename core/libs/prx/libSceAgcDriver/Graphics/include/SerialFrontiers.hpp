#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SERIALFRONTIERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SERIALFRONTIERS_HPP

#include <cstdint>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>

namespace AgcDriver::Graphics {

enum class BatchState : std::uint8_t {
    Reserved,
    Recorded,
    Submitted,
    Completed,
    Failed,
    Cancelled
};

class SerialFrontiers {
public:
    std::uint64_t Reserve() {
        states.push_back(BatchState::Reserved);
        return newest();
    }

    void Retract(std::uint64_t serial) {
        if (serial != newest() || serial < base || states.empty()) throw std::logic_error("a batch serial is retracted out of order: " + std::to_string(serial) + " with " + std::to_string(newest()) + " reserved");
        const auto state = states.back();
        if (state != BatchState::Reserved && state != BatchState::Recorded) throw std::logic_error("a submitted batch serial is retracted: " + std::to_string(serial));
        states.pop_back();
    }

    void Mark(std::uint64_t serial, BatchState state) {
        if (serial < base || serial > newest()) throw std::logic_error("a batch serial outside the reserved range is marked: " + std::to_string(serial));
        auto& current = states[serial - base];
        if (!allowed(current, state)) throw std::logic_error("batch serial " + std::to_string(serial) + " moves from state " + std::to_string(static_cast<int>(current)) + " to " + std::to_string(static_cast<int>(state)));
        current = state;
        prune();
    }

    BatchState State(std::uint64_t serial) const {
        if (serial == 0 || serial > newest()) throw std::logic_error("a batch serial that was never reserved is read: " + std::to_string(serial));
        if (serial < base) {
            const auto found = retiredFailures.find(serial);
            return found != retiredFailures.end() ? found->second : BatchState::Completed;
        }
        return states[serial - base];
    }

    std::uint64_t Newest() const { return newest(); }

    std::uint64_t Through(BatchState state) const {
        if (state == BatchState::Failed || state == BatchState::Cancelled) throw std::logic_error("a success frontier is asked for a failure state");
        if (completedPrefix + 1 < base) return completedPrefix;
        std::uint64_t through = base - 1;
        for (const auto current : states) {
            if (!reaches(current, state)) break;
            ++through;
        }
        return through;
    }

    std::uint64_t Terminal() const {
        std::uint64_t through = base - 1;
        for (const auto current : states) {
            if (!terminal(current)) break;
            ++through;
        }
        return through;
    }

private:
    static bool terminal(BatchState state) { return state == BatchState::Completed || state == BatchState::Failed || state == BatchState::Cancelled; }
    static int rank(BatchState state) { return state == BatchState::Reserved ? 0 : state == BatchState::Recorded ? 1 : state == BatchState::Submitted ? 2 : state == BatchState::Completed ? 3 : -1; }
    static bool reaches(BatchState current, BatchState wanted) { return rank(current) >= rank(wanted) && rank(current) >= 0; }
    static bool allowed(BatchState from, BatchState to) {
        if (terminal(from)) return false;
        if (to == BatchState::Failed) return true;
        if (to == BatchState::Cancelled) return from == BatchState::Reserved || from == BatchState::Recorded;
        return rank(to) == rank(from) + 1;
    }
    std::uint64_t newest() const { return base - 1 + states.size(); }
    void prune() {
        while (!states.empty() && terminal(states.front())) {
            const auto state = states.front();
            if (state == BatchState::Completed && completedPrefix + 1 == base) ++completedPrefix;
            else if (state != BatchState::Completed) retiredFailures.emplace(base, state);
            states.pop_front();
            ++base;
        }
    }

    std::uint64_t base = 1;
    std::uint64_t completedPrefix = 0;
    std::map<std::uint64_t, BatchState> retiredFailures;
    std::deque<BatchState> states;
};

}

#endif
