#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "ShaderCacheSourceVersion.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace AgcDriver::Graphics {

namespace {

constexpr std::size_t SwitchCount = static_cast<std::size_t>(RuntimeSwitch::Count);

constexpr std::array<const char*, SwitchCount> SwitchNames{
    "APS5_NO_BARRIER_BATCH",
    "APS5_NO_BARRIER_FUSION",
    "APS5_NO_WRITE_BACK_RUNS",
    "APS5_NO_GUARD_SET_PROOF",
    "APS5_NO_GUARD_SPANS",
    "APS5_NO_OWNED_DRAW_INPUTS",
    "APS5_HANDWRITTEN_PLAN_STATE",
    "APS5_NO_RESIDENT_RANGES",
    "APS5_NO_EARLY_DRAW_INPUTS",
    "APS5_NO_EMPTY_FETCH_SKIP",
    "APS5_NO_SHARED_VERTEX_LAYOUT",
    "APS5_CERTIFIED_PROOFS",
    "APS5_NO_CERTIFIED_PROOF_SCOPE",
    "APS5_SHARE_PUSH_DONOR",
    "APS5_NO_BAR_RING",
    "APS5_NO_TARGET_MEMO",
    "APS5_FOLLOW_OLDER_EPOCH_TOKENS",
    "APS5_UPLOAD_REFRESH_PROOF",
    "APS5_REFRESH_EXIT_EPOCH_STAMP",
    "APS5_NO_DEPTH_REPEAT_FIND",
    "APS5_PARTIAL_DRAINS",
    "APS5_PLAN_ATTACHMENTS",
    "APS5_NO_RESIDENT_TARGETS",
    "APS5_NO_DRAW_INPUT_RECYCLING",
    "APS5_NO_EARLY_UPLOAD_PREPARE",
    "APS5_NO_EARLY_REGION_COPIES",
    "APS5_NO_DRAW_INPUT_RING",
    "APS5_NO_RECORDED_CPU_UPLOAD",
    "APS5_KEEP_EMPTY_FRAGMENT_SET",
    "APS5_NO_RESIDENT_LINEAR",
    "APS5_NO_STAGING_PREDICTION",
    "APS5_ONE_STAGE_REQUEST",
    "APS5_STAGE_HANDLE_HINTS",
    "APS5_NO_COVERED_COMMITS",
    "APS5_NO_WRITE_OWNERS",
    "APS5_WHOLE_PENDING_BUMPS",
    "APS5_NO_CERTIFIED_DRAW_INPUTS",
    "APS5_WRITE_TICKETS",
    "APS5_SINGLE_RING_PREPARER",
    "APS5_RELOCATION_DRAIN",
};

struct SwitchValues {
    std::array<std::atomic<bool>, SwitchCount> set{};
    SwitchValues() {
        const std::array<bool, SwitchCount> environment{
            std::getenv("APS5_NO_BARRIER_BATCH") != nullptr,
            std::getenv("APS5_NO_BARRIER_FUSION") != nullptr,
            std::getenv("APS5_NO_WRITE_BACK_RUNS") != nullptr,
            std::getenv("APS5_NO_GUARD_SET_PROOF") != nullptr,
            std::getenv("APS5_NO_GUARD_SPANS") != nullptr,
            std::getenv("APS5_NO_OWNED_DRAW_INPUTS") != nullptr,
            std::getenv("APS5_HANDWRITTEN_PLAN_STATE") != nullptr,
            std::getenv("APS5_NO_RESIDENT_RANGES") != nullptr,
            std::getenv("APS5_NO_EARLY_DRAW_INPUTS") != nullptr,
            std::getenv("APS5_NO_EMPTY_FETCH_SKIP") != nullptr,
            std::getenv("APS5_NO_SHARED_VERTEX_LAYOUT") != nullptr,
            std::getenv("APS5_CERTIFIED_PROOFS") != nullptr,
            std::getenv("APS5_NO_CERTIFIED_PROOF_SCOPE") != nullptr,
            std::getenv("APS5_SHARE_PUSH_DONOR") != nullptr,
            std::getenv("APS5_NO_BAR_RING") != nullptr,
            std::getenv("APS5_NO_TARGET_MEMO") != nullptr,
            std::getenv("APS5_FOLLOW_OLDER_EPOCH_TOKENS") != nullptr,
            std::getenv("APS5_UPLOAD_REFRESH_PROOF") != nullptr,
            std::getenv("APS5_REFRESH_EXIT_EPOCH_STAMP") != nullptr,
            std::getenv("APS5_NO_DEPTH_REPEAT_FIND") != nullptr,
            std::getenv("APS5_PARTIAL_DRAINS") != nullptr,
            std::getenv("APS5_PLAN_ATTACHMENTS") != nullptr,
            std::getenv("APS5_NO_RESIDENT_TARGETS") != nullptr,
            std::getenv("APS5_NO_DRAW_INPUT_RECYCLING") != nullptr,
            std::getenv("APS5_NO_EARLY_UPLOAD_PREPARE") != nullptr,
            std::getenv("APS5_NO_EARLY_REGION_COPIES") != nullptr,
            std::getenv("APS5_NO_DRAW_INPUT_RING") != nullptr,
            std::getenv("APS5_NO_RECORDED_CPU_UPLOAD") != nullptr,
            std::getenv("APS5_KEEP_EMPTY_FRAGMENT_SET") != nullptr,
            std::getenv("APS5_NO_RESIDENT_LINEAR") != nullptr,
            std::getenv("APS5_NO_STAGING_PREDICTION") != nullptr,
            std::getenv("APS5_ONE_STAGE_REQUEST") != nullptr,
            std::getenv("APS5_STAGE_HANDLE_HINTS") != nullptr,
            std::getenv("APS5_NO_COVERED_COMMITS") != nullptr,
            std::getenv("APS5_NO_WRITE_OWNERS") != nullptr,
            std::getenv("APS5_WHOLE_PENDING_BUMPS") != nullptr,
            std::getenv("APS5_NO_CERTIFIED_DRAW_INPUTS") != nullptr,
            std::getenv("APS5_WRITE_TICKETS") != nullptr,
            std::getenv("APS5_SINGLE_RING_PREPARER") != nullptr,
            std::getenv("APS5_RELOCATION_DRAIN") != nullptr,
        };
        for (std::size_t index = 0; index < SwitchCount; ++index) set[index].store(environment[index], std::memory_order_relaxed);
    }
};

SwitchValues& Values() {
    static SwitchValues values;
    return values;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
    return text;
}

double ParseSeconds(std::string_view text, const char* what) {
    const std::string copy(Trim(text));
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (copy.empty() || end != copy.c_str() + copy.size() || !std::isfinite(value) || value < 0) throw std::invalid_argument(std::string(what) + ": '" + copy + "' is not a number of seconds");
    return value;
}

std::string FlippableList() {
    std::string list;
    for (const auto* name : SwitchNames) list += std::string(list.empty() ? "" : ", ") + name;
    return list;
}

}

namespace RuntimeSwitches {

const char* Name(RuntimeSwitch id) {
    return SwitchNames.at(static_cast<std::size_t>(id));
}

bool Set(RuntimeSwitch id) {
    return Values().set.at(static_cast<std::size_t>(id)).load(std::memory_order_relaxed);
}

void Assign(RuntimeSwitch id, bool set) {
    Values().set.at(static_cast<std::size_t>(id)).store(set, std::memory_order_relaxed);
}

std::optional<RuntimeSwitch> Find(std::string_view name) {
    for (std::size_t index = 0; index < SwitchCount; ++index) {
        if (name == SwitchNames[index]) return static_cast<RuntimeSwitch>(index);
    }
    return std::nullopt;
}

}

std::vector<RuntimeSwitch> AbSchedule::Config::Toggled() const {
    std::vector<RuntimeSwitch> toggled;
    for (const auto& arm : arms) {
        for (const auto id : arm) {
            if (std::find(toggled.begin(), toggled.end(), id) == toggled.end()) toggled.push_back(id);
        }
    }
    return toggled;
}

AbSchedule::Config AbSchedule::Parse(std::string_view ab, std::string_view start, std::string_view settle) {
    Config config;
    ab = Trim(ab);
    const auto colon = ab.find(':');
    if (colon == std::string_view::npos) throw std::invalid_argument("APS5_AB: expected <seconds>:<switch>[+<switch>...][|<switch>[+<switch>...]...], got '" + std::string(ab) + "'");
    config.phaseSeconds = ParseSeconds(ab.substr(0, colon), "APS5_AB phase length");
    if (config.phaseSeconds < 1) throw std::invalid_argument("APS5_AB: a phase must last at least 1 s");
    auto list = ab.substr(colon + 1);
    if (Trim(list).empty()) throw std::invalid_argument("APS5_AB: no switch listed after the phase length");
    for (;;) {
        const auto bar = list.find('|');
        auto members = list.substr(0, bar);
        if (Trim(members).empty()) throw std::invalid_argument("APS5_AB: an empty arm in '" + std::string(ab) + "'");
        std::vector<RuntimeSwitch> arm;
        for (;;) {
            const auto plus = members.find('+');
            const auto name = Trim(members.substr(0, plus));
            if (name.empty()) throw std::invalid_argument("APS5_AB: an empty switch name in '" + std::string(ab) + "'");
            const auto found = RuntimeSwitches::Find(name);
            if (!found.has_value()) {
                const auto& keyed = ShaderRecompiler::ShaderDiskCache::Generated::RecompilerSwitches;
                if (std::find(keyed.begin(), keyed.end(), name) != keyed.end()) throw std::invalid_argument("APS5_AB: " + std::string(name) + " is a shader recompiler switch, read into the shader disk cache key when shaders compile, so it cannot change while the title runs; switches the A/B can change: " + FlippableList());
                throw std::invalid_argument("APS5_AB: " + std::string(name) + " cannot change while the title runs; switches the A/B can change: " + FlippableList());
            }
            if (std::find(arm.begin(), arm.end(), *found) != arm.end()) throw std::invalid_argument("APS5_AB: " + std::string(name) + " is listed twice");
            arm.push_back(*found);
            if (plus == std::string_view::npos) break;
            members = members.substr(plus + 1);
        }
        const auto same = [&](std::vector<RuntimeSwitch> other) {
            auto sorted = arm;
            std::sort(sorted.begin(), sorted.end());
            std::sort(other.begin(), other.end());
            return sorted == other;
        };
        if (std::any_of(config.arms.begin(), config.arms.end(), same)) throw std::invalid_argument("APS5_AB: arm " + std::string(1, Letter(config.arms.size() + 1)) + " lists the same switches as an earlier arm");
        config.arms.push_back(std::move(arm));
        if (config.arms.size() > MaxArms) throw std::invalid_argument("APS5_AB: at most " + std::to_string(MaxArms) + " arms (B to Z) besides phase A");
        if (bar == std::string_view::npos) break;
        list = list.substr(bar + 1);
    }
    start = Trim(start);
    if (!start.empty()) {
        if (start.back() == 's') {
            config.startSeconds = ParseSeconds(start.substr(0, start.size() - 1), "APS5_AB_START");
        } else {
            const std::string copy(start);
            char* end = nullptr;
            errno = 0;
            const auto value = std::strtoull(copy.c_str(), &end, 10);
            if (end != copy.c_str() + copy.size() || errno == ERANGE || copy.front() < '0' || copy.front() > '9') throw std::invalid_argument("APS5_AB_START: '" + copy + "' is neither a present index nor <n>s");
            config.startPresent = value;
        }
    }
    if (!Trim(settle).empty()) config.settleSeconds = ParseSeconds(settle, "APS5_AB_SETTLE");
    if (config.settleSeconds >= config.phaseSeconds) throw std::invalid_argument("APS5_AB_SETTLE must be shorter than the phase");
    return config;
}

AbSchedule::AbSchedule(Config config) : config(std::move(config)) {}

AbSchedule::Step AbSchedule::AtPresent(std::uint64_t present, double seconds) {
    if (!firstSeconds.has_value()) firstSeconds = seconds;
    if (!started) {
        const bool due = config.startSeconds.has_value() ? seconds - *firstSeconds >= *config.startSeconds : present >= config.startPresent;
        if (!due) return {};
        started = true;
        settled = false;
        phase = 0;
        phaseStart = seconds;
        return {Event::Begin, 0, 0};
    }
    const auto arm = [&] { return static_cast<std::size_t>(phase % (config.arms.size() + 1)); };
    if (seconds - phaseStart >= config.phaseSeconds) {
        ++phase;
        settled = false;
        phaseStart = seconds;
        return {Event::Begin, phase, arm()};
    }
    if (!settled && seconds - phaseStart >= config.settleSeconds) {
        settled = true;
        return {Event::Settled, phase, arm()};
    }
    return {};
}

namespace {

struct AbState {
    std::mutex mutex;
    std::optional<AbSchedule> schedule;
    std::array<bool, SwitchCount> environment{};
};

std::atomic<AbObserver> abObserver{nullptr};

AbState& Ab() {
    static AbState* state = [] {
        auto* created = new AbState();
        const char* ab = std::getenv("APS5_AB");
        const char* start = std::getenv("APS5_AB_START");
        const char* settle = std::getenv("APS5_AB_SETTLE");
        if (ab != nullptr) {
            created->schedule.emplace(AbSchedule::Parse(ab, start != nullptr ? start : "", settle != nullptr ? settle : ""));
            for (std::size_t index = 0; index < SwitchCount; ++index) created->environment[index] = RuntimeSwitches::Set(static_cast<RuntimeSwitch>(index));
        }
        return created;
    }();
    return *state;
}

bool Toggles(const AbSchedule::Config& config, std::size_t arm, RuntimeSwitch id) {
    return arm != 0 && std::find(config.arms[arm - 1].begin(), config.arms[arm - 1].end(), id) != config.arms[arm - 1].end();
}

std::string SwitchSettings(const AbState& state, std::size_t arm) {
    const auto& config = state.schedule->Settings();
    std::string text;
    for (const auto id : config.Toggled()) {
        const bool set = state.environment[static_cast<std::size_t>(id)] != Toggles(config, arm, id);
        text += std::string(text.empty() ? "" : ", ") + RuntimeSwitches::Name(id) + (set ? " set" : " unset");
    }
    return text;
}

}

bool AbConfigured() {
    static const bool configured = std::getenv("APS5_AB") != nullptr;
    return configured;
}

void SetAbObserver(AbObserver observer) {
    abObserver.store(observer, std::memory_order_release);
}

std::vector<std::string> AbAtPresent(std::uint64_t present, double seconds, AbSchedule::Step* taken) {
    if (!AbConfigured()) return {};
    auto& state = Ab();
    std::lock_guard lock(state.mutex);
    std::vector<std::string> lines;
    char text[512];
    const auto& config = state.schedule->Settings();
    const auto step = state.schedule->AtPresent(present, seconds);
    if (taken != nullptr) *taken = step;
    if (const auto observe = abObserver.load(std::memory_order_acquire)) observe(step.event, AbSchedule::Letter(step.arm), step.phase, present, seconds);
    if (step.event == AbSchedule::Event::None) return lines;
    if (step.event == AbSchedule::Event::Begin) {
        if (step.phase == 0) {
            char from[96];
            if (config.startSeconds.has_value()) std::snprintf(from, sizeof(from), "%.1f s after the first present", *config.startSeconds);
            else std::snprintf(from, sizeof(from), "present %llu", static_cast<unsigned long long>(config.startPresent));
            std::snprintf(text, sizeof(text), "[ab] in-run A/B: phases of %.1f s from %s, settle %.1f s", config.phaseSeconds, from, config.settleSeconds);
            std::string header = text;
            for (std::size_t arm = 0; arm <= config.arms.size(); ++arm) header += std::string("; ") + AbSchedule::Letter(arm) + ": " + SwitchSettings(state, arm);
            lines.push_back(header + "\n");
        }
        for (const auto id : config.Toggled()) RuntimeSwitches::Assign(id, state.environment[static_cast<std::size_t>(id)] != Toggles(config, step.arm, id));
        std::snprintf(text, sizeof(text), "[ab] phase %c %llu begins at present %llu (t %.3f s): ", AbSchedule::Letter(step.arm), static_cast<unsigned long long>(step.phase), static_cast<unsigned long long>(present), seconds);
        lines.push_back(text + SwitchSettings(state, step.arm) + "\n");
        return lines;
    }
    std::snprintf(text, sizeof(text), "[ab] phase %c %llu settled at present %llu (t %.3f s)\n", AbSchedule::Letter(step.arm), static_cast<unsigned long long>(step.phase), static_cast<unsigned long long>(present), seconds);
    lines.emplace_back(text);
    return lines;
}

}
