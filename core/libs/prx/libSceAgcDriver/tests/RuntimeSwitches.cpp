#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "ShaderCacheSourceVersion.hpp"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <iterator>

namespace ShaderRecompiler::ShaderDiskCache::Generated {

namespace {

constexpr std::string_view FixtureRecompilerSwitches[] = {"APS5_NO_CODE_HASH_KEY", "APS5_NO_RESULT_MEMO"};

}

constinit const std::span<const std::string_view> RecompilerSwitches = FixtureRecompilerSwitches;

}

using AgcDriver::Graphics::AbSchedule;
using AgcDriver::Graphics::RuntimeSwitch;
namespace RuntimeSwitches = AgcDriver::Graphics::RuntimeSwitches;

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

std::string Refusal(const char* ab, const char* start = "", const char* settle = "") {
    try {
        AbSchedule::Parse(ab, start, settle);
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return {};
}

void registry() {
    for (std::size_t index = 0; index < static_cast<std::size_t>(RuntimeSwitch::Count); ++index) {
        const auto id = static_cast<RuntimeSwitch>(index);
        Require(RuntimeSwitches::Find(RuntimeSwitches::Name(id)) == id, std::string("the name of a switch does not find it: ") + RuntimeSwitches::Name(id));
        Require(RuntimeSwitches::Set(id) == (std::getenv(RuntimeSwitches::Name(id)) != nullptr), std::string("a switch does not start as the environment sets it: ") + RuntimeSwitches::Name(id));
    }
    Require(RuntimeSwitches::Find("APS5_NO_BARRIER_BATCH") == RuntimeSwitch::NoBarrierBatch, "APS5_NO_BARRIER_BATCH is not a runtime switch");
    Require(RuntimeSwitches::Set(RuntimeSwitch::NoBarrierBatch), "a switch set in the environment did not start set");
    Require(!RuntimeSwitches::Find("APS5_FULL_BARRIERS").has_value() && !RuntimeSwitches::Find("APS5_NO_BARRIER_BATCH ").has_value(), "a name that is not a runtime switch was found");
    RuntimeSwitches::Assign(RuntimeSwitch::NoBarrierBatch, false);
    Require(!RuntimeSwitches::Set(RuntimeSwitch::NoBarrierBatch), "an assigned switch did not read back unset");
    RuntimeSwitches::Assign(RuntimeSwitch::NoBarrierBatch, true);
    Require(RuntimeSwitches::Set(RuntimeSwitch::NoBarrierBatch), "an assigned switch did not read back set");
    const auto ring = AbSchedule::Parse("30:APS5_NO_TARGET_MEMO+APS5_FOLLOW_OLDER_EPOCH_TOKENS+APS5_UPLOAD_REFRESH_PROOF+APS5_REFRESH_EXIT_EPOCH_STAMP+APS5_NO_DEPTH_REPEAT_FIND+APS5_SINGLE_RING_PREPARER", "", "");
    Require(ring.arms.size() == 1 && ring.arms[0].size() == 6 && ring.arms[0].front() == RuntimeSwitch::NoTargetMemo && ring.arms[0].back() == RuntimeSwitch::SingleRingPreparer, "the ring's target, epoch and preparer switches are not all flippable by APS5_AB");
}

void parse() {
    const auto config = AbSchedule::Parse(" 30 : APS5_NO_BARRIER_BATCH ", "815", "3");
    Require(config.phaseSeconds == 30 && config.settleSeconds == 3 && config.startPresent == 815 && !config.startSeconds.has_value(), "the phase settings were not parsed");
    Require(config.arms.size() == 1 && config.arms[0] == std::vector<RuntimeSwitch>{RuntimeSwitch::NoBarrierBatch}, "the switch list was not parsed");
    const auto bySeconds = AbSchedule::Parse("60:APS5_NO_BARRIER_BATCH", "90s", "");
    Require(bySeconds.startSeconds == 90.0 && bySeconds.settleSeconds == 2 && bySeconds.startPresent == 0, "a start in seconds was not parsed");
    Require(Refusal("60:APS5_FULL_BARRIERS").find("APS5_FULL_BARRIERS cannot change while the title runs; switches the A/B can change: APS5_NO_BARRIER_BATCH") != std::string::npos, "a startup-only variable was not refused with the list of switches");
    Require(Refusal("60:APS5_NO_BARRIER_BATCH+APS5_NO_BARRIER_BATCH").find("listed twice") != std::string::npos, "a duplicate switch was not refused");
    Require(Refusal("60:+APS5_NO_BARRIER_BATCH").find("empty switch name") != std::string::npos && Refusal("60:APS5_NO_BARRIER_BATCH+").find("empty switch name") != std::string::npos, "an empty switch name was not refused");
    Require(Refusal("60:").find("no switch listed") != std::string::npos, "an empty switch list was not refused");
    Require(!Refusal("APS5_NO_BARRIER_BATCH").empty(), "a missing phase length was not refused");
    Require(!Refusal("0.5:APS5_NO_BARRIER_BATCH").empty() && !Refusal("x:APS5_NO_BARRIER_BATCH").empty() && !Refusal("-5:APS5_NO_BARRIER_BATCH").empty() && !Refusal("inf:APS5_NO_BARRIER_BATCH").empty(), "a bad phase length was not refused");
    Require(!Refusal("10:APS5_NO_BARRIER_BATCH", "", "10").empty() && !Refusal("10:APS5_NO_BARRIER_BATCH", "", "x").empty(), "a bad settle was not refused");
    Require(Refusal("60:APS5_NO_CODE_HASH_KEY").find("APS5_NO_CODE_HASH_KEY is a shader recompiler switch, read into the shader disk cache key when shaders compile, so it cannot change while the title runs; switches the A/B can change: APS5_NO_BARRIER_BATCH") != std::string::npos, "a shader recompiler switch was not refused as one");
    Require(Refusal("60:APS5_NO_BARRIER_BATCH|APS5_NO_RESULT_MEMO").find("APS5_NO_RESULT_MEMO is a shader recompiler switch") != std::string::npos, "a shader recompiler switch in a later arm was not refused as one");
    const auto arms = AbSchedule::Parse("20: APS5_NO_GUARD_SPANS+APS5_NO_BARRIER_BATCH | APS5_NO_OWNED_DRAW_INPUTS|APS5_NO_GUARD_SPANS", "", "");
    Require(arms.arms.size() == 3 && arms.arms[0] == std::vector<RuntimeSwitch>{RuntimeSwitch::NoGuardSpans, RuntimeSwitch::NoBarrierBatch}
            && arms.arms[1] == std::vector<RuntimeSwitch>{RuntimeSwitch::NoOwnedDrawInputs} && arms.arms[2] == std::vector<RuntimeSwitch>{RuntimeSwitch::NoGuardSpans}, "the arms were not parsed in the listed order");
    Require(arms.Toggled() == std::vector<RuntimeSwitch>{RuntimeSwitch::NoGuardSpans, RuntimeSwitch::NoBarrierBatch, RuntimeSwitch::NoOwnedDrawInputs}, "the toggled switches are not the arms' union in first-listed order");
    for (const char* ab : {"20:APS5_NO_BARRIER_BATCH||APS5_NO_GUARD_SPANS", "20:|APS5_NO_BARRIER_BATCH", "20:APS5_NO_BARRIER_BATCH|", "20:APS5_NO_BARRIER_BATCH| |APS5_NO_GUARD_SPANS"}) {
        Require(Refusal(ab).find("an empty arm") != std::string::npos, std::string("an empty arm was not refused in ") + ab);
    }
    Require(Refusal("20:APS5_NO_BARRIER_BATCH+APS5_NO_GUARD_SPANS|APS5_NO_GUARD_SPANS+APS5_NO_BARRIER_BATCH").find("arm C lists the same switches as an earlier arm") != std::string::npos, "two arms with the same switches were not refused");
    Require(Refusal("20:APS5_NO_BARRIER_BATCH|APS5_NO_GUARD_SPANS+APS5_NO_GUARD_SPANS").find("listed twice") != std::string::npos, "a duplicate switch inside a later arm was not refused");
    Require(Refusal("20:APS5_NO_BARRIER_BATCH|APS5_NO_GUARD_SPANS+").find("empty switch name") != std::string::npos, "an empty switch name inside a later arm was not refused");
    for (const char* start : {"-3", "abc", "s", "+3", "3x", "99999999999999999999999"}) {
        Require(Refusal("10:APS5_NO_BARRIER_BATCH", start).find("APS5_AB_START") != std::string::npos, std::string("the start '") + start + "' was not refused");
    }
}

void schedule() {
    AbSchedule plan(AbSchedule::Parse("60:APS5_NO_BARRIER_BATCH", "10", "2"));
    using Event = AbSchedule::Event;
    const auto expect = [&](std::uint64_t present, double seconds, Event event, std::uint64_t phase, bool b) {
        const auto step = plan.AtPresent(present, seconds);
        Require(step.event == event && (event == Event::None || (step.phase == phase && step.arm == (b ? 1u : 0u))), "unexpected step at present " + std::to_string(present));
    };
    expect(1, 0.0, Event::None, 0, false);
    expect(9, 100.0, Event::None, 0, false);
    expect(10, 102.0, Event::Begin, 0, false);
    expect(11, 103.0, Event::None, 0, false);
    expect(12, 104.5, Event::Settled, 0, false);
    expect(13, 150.0, Event::None, 0, false);
    expect(14, 161.9, Event::None, 0, false);
    expect(15, 162.0, Event::Begin, 1, true);
    expect(16, 163.0, Event::None, 1, true);
    expect(17, 300.0, Event::Begin, 2, false);
    expect(18, 301.0, Event::None, 2, false);
    expect(19, 302.5, Event::Settled, 2, false);
    expect(20, 362.5, Event::Begin, 3, true);

    AbSchedule timed(AbSchedule::Parse("30:APS5_NO_BARRIER_BATCH", "90s", ""));
    Require(timed.AtPresent(500, 1000.0).event == Event::None, "a start in seconds began at the first present");
    Require(timed.AtPresent(501, 1089.0).event == Event::None, "a start in seconds began early");
    const auto begin = timed.AtPresent(502, 1090.0);
    Require(begin.event == Event::Begin && begin.phase == 0 && begin.arm == 0, "a start in seconds did not begin phase A");

    AbSchedule rotation(AbSchedule::Parse("10:APS5_NO_BARRIER_BATCH|APS5_NO_GUARD_SPANS|APS5_NO_OWNED_DRAW_INPUTS", "1", "2"));
    const std::size_t order[] = {0, 1, 2, 3, 0, 1, 2, 3, 0};
    double seconds = 0;
    for (std::uint64_t phase = 0; phase < std::size(order); ++phase, seconds += 10) {
        const auto opened = rotation.AtPresent(1 + phase * 2, seconds);
        Require(opened.event == Event::Begin && opened.phase == phase && opened.arm == order[phase], "the arms do not rotate A, B, C, D at phase " + std::to_string(phase));
        const auto settled = rotation.AtPresent(2 + phase * 2, seconds + 2.5);
        Require(settled.event == Event::Settled && settled.phase == phase && settled.arm == order[phase], "a settled line names another arm at phase " + std::to_string(phase));
    }
}

void glue() {
    Require(AgcDriver::Graphics::AbConfigured(), "APS5_AB is not seen");
    Require(AgcDriver::Graphics::AbAtPresent(1, 0.0).empty(), "the A/B printed before its start");
    RuntimeSwitches::Assign(RuntimeSwitch::NoBarrierBatch, false);
    auto lines = AgcDriver::Graphics::AbAtPresent(3, 1.0);
    Require(lines.size() == 2 && lines[0] == "[ab] in-run A/B: phases of 5.0 s from present 3, settle 2.0 s; A: APS5_NO_BARRIER_BATCH set; B: APS5_NO_BARRIER_BATCH unset\n" && lines[1] == "[ab] phase A 0 begins at present 3 (t 1.000 s): APS5_NO_BARRIER_BATCH set\n", "the start lines differ: " + (lines.empty() ? std::string() : lines.front()));
    Require(RuntimeSwitches::Set(RuntimeSwitch::NoBarrierBatch), "phase A does not have the environment's setting");
    lines = AgcDriver::Graphics::AbAtPresent(4, 3.5);
    Require(lines.size() == 1 && lines[0] == "[ab] phase A 0 settled at present 4 (t 3.500 s)\n", "the settle line differs");
    lines = AgcDriver::Graphics::AbAtPresent(5, 6.0);
    Require(lines.size() == 1 && lines[0] == "[ab] phase B 1 begins at present 5 (t 6.000 s): APS5_NO_BARRIER_BATCH unset\n", "the phase B line differs");
    Require(!RuntimeSwitches::Set(RuntimeSwitch::NoBarrierBatch), "phase B did not toggle the listed switch");
    Require(AgcDriver::Graphics::AbAtPresent(6, 7.0).empty(), "a present inside the settle printed");
    lines = AgcDriver::Graphics::AbAtPresent(7, 11.5);
    Require(lines.size() == 1 && lines[0] == "[ab] phase A 2 begins at present 7 (t 11.500 s): APS5_NO_BARRIER_BATCH set\n", "phase A did not come back");
    Require(RuntimeSwitches::Set(RuntimeSwitch::NoBarrierBatch), "phase A did not restore the environment's setting");
}

int multiArm() {
    try {
        SetEnvironment("APS5_NO_BARRIER_BATCH", "1");
        SetEnvironment("APS5_NO_GUARD_SPANS", nullptr);
        SetEnvironment("APS5_NO_OWNED_DRAW_INPUTS", nullptr);
        SetEnvironment("APS5_AB", "4:APS5_NO_BARRIER_BATCH|APS5_NO_GUARD_SPANS+APS5_NO_OWNED_DRAW_INPUTS|APS5_NO_GUARD_SPANS");
        SetEnvironment("APS5_AB_START", "1");
        SetEnvironment("APS5_AB_SETTLE", "1");
        const auto settings = [] {
            return std::string(RuntimeSwitches::Set(RuntimeSwitch::NoBarrierBatch) ? "1" : "0") + (RuntimeSwitches::Set(RuntimeSwitch::NoGuardSpans) ? "1" : "0") + (RuntimeSwitches::Set(RuntimeSwitch::NoOwnedDrawInputs) ? "1" : "0");
        };
        auto lines = AgcDriver::Graphics::AbAtPresent(1, 0.0);
        Require(lines.size() == 2 && lines[0] == "[ab] in-run A/B: phases of 4.0 s from present 1, settle 1.0 s; "
                "A: APS5_NO_BARRIER_BATCH set, APS5_NO_GUARD_SPANS unset, APS5_NO_OWNED_DRAW_INPUTS unset; "
                "B: APS5_NO_BARRIER_BATCH unset, APS5_NO_GUARD_SPANS unset, APS5_NO_OWNED_DRAW_INPUTS unset; "
                "C: APS5_NO_BARRIER_BATCH set, APS5_NO_GUARD_SPANS set, APS5_NO_OWNED_DRAW_INPUTS set; "
                "D: APS5_NO_BARRIER_BATCH set, APS5_NO_GUARD_SPANS set, APS5_NO_OWNED_DRAW_INPUTS unset\n", "the multi-arm header differs: " + (lines.empty() ? std::string() : lines.front()));
        Require(lines[1] == "[ab] phase A 0 begins at present 1 (t 0.000 s): APS5_NO_BARRIER_BATCH set, APS5_NO_GUARD_SPANS unset, APS5_NO_OWNED_DRAW_INPUTS unset\n" && settings() == "100", "phase A did not keep the environment's settings");
        Require(AgcDriver::Graphics::AbAtPresent(2, 1.5) == std::vector<std::string>{"[ab] phase A 0 settled at present 2 (t 1.500 s)\n"}, "the multi-arm settle line differs");
        const char* expected[] = {"000", "111", "110", "100", "000"};
        const char letters[] = {'B', 'C', 'D', 'A', 'B'};
        for (std::uint64_t phase = 1; phase <= 5; ++phase) {
            lines = AgcDriver::Graphics::AbAtPresent(2 + phase, 4.0 * static_cast<double>(phase));
            Require(lines.size() == 1 && lines[0].rfind(std::string("[ab] phase ") + letters[phase - 1] + " " + std::to_string(phase) + " begins", 0) == 0, "phase " + std::to_string(phase) + " is not arm " + letters[phase - 1] + ": " + (lines.empty() ? std::string() : lines.front()));
            Require(settings() == expected[phase - 1], "phase " + std::to_string(phase) + " has switches " + settings() + ", expected " + expected[phase - 1]);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
    std::printf("multi-arm A/B tests passed\n");
    return 0;
}

int refused() {
    SetEnvironment("APS5_AB", "5:APS5_FULL_BARRIERS");
    try {
        AgcDriver::Graphics::AbAtPresent(1, 0.0);
    } catch (const std::invalid_argument& error) {
        if (std::string_view(error.what()).find("APS5_FULL_BARRIERS cannot change while the title runs") != std::string_view::npos) {
            std::printf("runtime switch refusal test passed\n");
            return 0;
        }
        std::fprintf(stderr, "FAILED: unexpected refusal: %s\n", error.what());
        return 1;
    }
    std::fprintf(stderr, "FAILED: APS5_AB naming a startup-only variable was not refused at the first present\n");
    return 1;
}

}

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "refused") return refused();
    if (argc > 1 && std::string_view(argv[1]) == "multi") return multiArm();
    try {
        SetEnvironment("APS5_NO_BARRIER_BATCH", "1");
        SetEnvironment("APS5_AB", "5:APS5_NO_BARRIER_BATCH");
        SetEnvironment("APS5_AB_START", "3");
        SetEnvironment("APS5_AB_SETTLE", nullptr);
        registry();
        parse();
        schedule();
        glue();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
    std::printf("runtime switch tests passed\n");
    return 0;
}
