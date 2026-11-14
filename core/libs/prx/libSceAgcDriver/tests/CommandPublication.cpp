#include "prx/libSceAgcDriver/Execution/include/CommandPublication.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::CommandPublication;
using Range = CommandPublication::Range;

constexpr std::uint32_t Nop = 0xc0001000u;
constexpr std::uint32_t Filler = 0x80000000u;
constexpr std::uint32_t IndirectHeader = 0xc0023f00u;
constexpr std::uint32_t PredicatedIndirectHeader = 0xc0023f01u;
constexpr std::uint32_t ConditionalHeader = 0xc00c3f00u;
constexpr std::uint32_t RewindHeader = 0xc0005900u;
constexpr std::uint32_t ConditionalExecHeader = 0xc0032200u;
constexpr std::uint64_t Thread = 1;
constexpr std::uint64_t Other = 2;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool anywhere(const void*, std::size_t) {
    return true;
}

std::uint64_t at(const void* pointer) {
    return reinterpret_cast<std::uintptr_t>(pointer);
}

Range whole(const std::vector<std::uint32_t>& words) {
    return {at(words.data()), at(words.data() + words.size())};
}

std::uint32_t low(const void* pointer) {
    return static_cast<std::uint32_t>(at(pointer));
}

std::uint32_t high(const void* pointer) {
    return static_cast<std::uint32_t>(at(pointer) >> 32u);
}

void indirect(std::vector<std::uint32_t>& commands, const std::vector<std::uint32_t>& target, bool chain = false, std::uint32_t header = IndirectHeader) {
    commands.insert(commands.end(), {header, low(target.data()), high(target.data()), static_cast<std::uint32_t>(target.size()) | (chain ? 1u << 20u : 0u)});
}

void conditional(std::vector<std::uint32_t>& commands, const std::vector<std::uint32_t>& target) {
    commands.insert(commands.end(), {ConditionalHeader, 1u, 0u, 0u, 0u, 0u, 0u, 0u, low(target.data()), high(target.data()), static_cast<std::uint32_t>(target.size()), 0u, 0u, 0u});
}

std::vector<std::uint32_t> visited(const std::vector<std::uint32_t>& root) {
    std::vector<std::uint32_t> words;
    CommandPublication::Walk(root.data(), root.size(), anywhere, [&](std::span<const std::uint32_t> packet) { words.insert(words.end(), packet.begin(), packet.end()); });
    return words;
}

void testWalk() {
    const std::vector<std::uint32_t> nested{Nop, 7u, Filler};
    const std::vector<std::uint32_t> skipped{Nop, 8u};
    const std::vector<std::uint32_t> chained{Nop, 9u};
    std::vector<std::uint32_t> root{Nop, 1u};
    indirect(root, nested);
    conditional(root, skipped);
    indirect(root, chained, true);
    root.insert(root.end(), {Nop, 2u});
    const auto reach = CommandPublication::Walk(root.data(), root.size(), anywhere);
    check(reach.segments == std::vector<Range>{whole(root), whole(nested), whole(skipped)}, "the walk did not reach the root, the nested and the conditional buffers in order, or went on past the COND_INDIRECT_BUFFER");
    check(reach.tails.empty(), "a walk without REWIND recorded a tail");

    std::vector<std::uint32_t> chaining{Nop, 1u};
    indirect(chaining, nested);
    indirect(chaining, chained, true);
    chaining.insert(chaining.end(), {Nop, 2u});
    const auto chainReach = CommandPublication::Walk(chaining.data(), chaining.size(), anywhere);
    check(chainReach.segments == std::vector<Range>{whole(chaining), whole(nested), whole(chained)}, "the walk did not follow a chained INDIRECT_BUFFER");
    check(visited(chaining) == std::vector<std::uint32_t>{Nop, 1u, Nop, 7u, Filler, Nop, 9u}, "the walk visited packets after a chained INDIRECT_BUFFER or left out a target's packets");

    std::vector<std::uint32_t> predicated{Nop, 3u};
    indirect(predicated, nested, false, PredicatedIndirectHeader);
    const auto predicatedReach = CommandPublication::Walk(predicated.data(), predicated.size(), anywhere);
    check(predicatedReach.segments == std::vector<Range>{whole(predicated), whole(nested)}, "a predicated INDIRECT_BUFFER target was not reached");
    std::vector<std::uint32_t> predicatedWords{Nop, 3u};
    predicatedWords.insert(predicatedWords.end(), predicated.begin() + 2, predicated.end());
    predicatedWords.insert(predicatedWords.end(), {Nop, 7u, Filler});
    check(visited(predicated) == predicatedWords, "a predicated INDIRECT_BUFFER was not visited before its target");

    std::vector<std::uint32_t> tailCall{Nop, 1u};
    conditional(tailCall, skipped);
    tailCall.insert(tailCall.end(), {Nop, 2u});
    check(visited(tailCall) == std::vector<std::uint32_t>{Nop, 1u, Nop, 8u}, "a COND_INDIRECT_BUFFER outside a COND_EXEC range did not end its buffer");
    std::vector<std::uint32_t> guarded{Nop, 1u, ConditionalExecHeader, 0u, 0u, 0u, static_cast<std::uint32_t>(AgcDriver::Pm4::ConditionalBufferWords)};
    conditional(guarded, skipped);
    guarded.insert(guarded.end(), {Nop, 2u});
    std::vector<std::uint32_t> guardedWords(guarded.begin(), guarded.begin() + 7);
    guardedWords.insert(guardedWords.end(), {Nop, 8u, Nop, 2u});
    check(visited(guarded) == guardedWords, "a COND_INDIRECT_BUFFER inside a COND_EXEC range ended its buffer");
    std::vector<std::uint32_t> overhanging{ConditionalExecHeader, 0u, 0u, 0u, 1u, Nop, 1u};
    check(CommandPublication::Walk(overhanging.data(), overhanging.size(), anywhere).segments.size() == 1 && visited(overhanging).size() == 5, "a COND_EXEC range ending inside a packet did not stop the walk");

    std::vector<std::uint32_t> unknown{Nop, 4u, 0x12345678u, 5u};
    check(visited(unknown) == unknown, "words that are not a type-3 packet were not visited as one run");
    std::vector<std::uint32_t> truncated{Nop, 4u, 0xc0051000u, 5u};
    check(visited(truncated) == truncated, "a packet longer than its buffer was not visited as one run");
}

void testRewind() {
    std::vector<std::uint32_t> root{Nop, 1u, RewindHeader, 0u, Nop, 2u, Nop, 3u};
    const auto reach = CommandPublication::Walk(root.data(), root.size(), anywhere);
    check(reach.segments == std::vector<Range>{whole(root)}, "a REWIND changed the segments the walk reached");
    check(reach.tails.size() == 1 && reach.tails[0].word == at(root.data() + 3) && reach.tails[0].range == Range{at(root.data() + 4), at(root.data() + root.size())}, "a REWIND's tail was not the words after it, gated by its second dword");
    check(visited(root) == std::vector<std::uint32_t>{Nop, 1u, RewindHeader, 0u}, "the walk visited past a REWIND");

    std::vector<std::uint32_t> inner{Nop, 5u, RewindHeader, 0u, Nop, 6u};
    std::vector<std::uint32_t> outer{Nop, 1u};
    indirect(outer, inner);
    outer.insert(outer.end(), {Nop, 2u});
    const auto stopped = CommandPublication::Walk(outer.data(), outer.size(), anywhere);
    check(stopped.segments == std::vector<Range>{whole(outer), whole(inner)} && stopped.tails.size() == 1, "a REWIND inside a nested buffer did not stop the whole walk");
}

void testWalkLimits() {
    std::vector<std::uint32_t> self(4);
    self = {IndirectHeader, low(self.data()), high(self.data()), 4u};
    const auto nested = CommandPublication::Walk(self.data(), self.size(), anywhere);
    check(nested.segments.size() == CommandPublication::MaximumDepth, "a buffer that nests itself was not stopped at the depth limit");

    constexpr std::size_t LoopWords = std::size_t{1} << 19u;
    std::vector<std::uint32_t> loop(LoopWords, Filler);
    const std::array<std::uint32_t, 4> link{IndirectHeader, low(loop.data()), high(loop.data()), static_cast<std::uint32_t>(LoopWords) | (1u << 20u)};
    std::copy(link.begin(), link.end(), loop.end() - 4);
    const auto looped = CommandPublication::Walk(loop.data(), loop.size(), anywhere);
    check(looped.segments.size() == CommandPublication::WordBudget / LoopWords, "a buffer that chains to itself was not stopped by the word budget");

    const std::vector<std::uint32_t> target{Nop, 1u};
    std::vector<std::uint32_t> root{Nop, 2u};
    indirect(root, target);
    const auto refused = CommandPublication::Walk(root.data(), root.size(), [&](const void* address, std::size_t) { return address != target.data(); });
    check(refused.segments == std::vector<Range>{whole(root)}, "the walk read a target the accessibility check refused");
}

void testPublication() {
    CommandPublication publication(anywhere);
    std::vector<std::uint32_t> mine(64, Nop);
    std::vector<std::uint32_t> theirs(64, Nop);
    const auto mineBegin = at(mine.data());
    publication.Note(mineBegin, 64, Thread);
    publication.Note(mineBegin + 64, 64, Thread);
    publication.Note(at(theirs.data()), theirs.size() * 4, Other);
    check(publication.PendingRanges() == 2, "contiguous writes of one thread were not merged");
    {
        const auto held = publication.Hold();
        check(publication.Unpublished(held, mineBegin, mineBegin + 256) == std::vector<Range>{{mineBegin, mineBegin + 128}}, "an unpublished command range was not reported, or was reported past its end");
    }

    std::vector<std::uint32_t> root(mine.begin(), mine.begin() + 16);
    publication.PublishSubmission(mine.data(), 16, Other);
    {
        const auto held = publication.Hold();
        check(publication.Unpublished(held, mineBegin, mineBegin + 256) == std::vector<Range>{{mineBegin + 64, mineBegin + 128}}, "a submission did not publish exactly the words it submits");
        check(publication.Unpublished(held, at(theirs.data()), at(theirs.data()) + 256).empty(), "a submission did not publish its own thread's ranges");
    }
    publication.PublishSubmission(nullptr, 0, Thread);
    {
        const auto held = publication.Hold();
        check(publication.Unpublished(held, mineBegin, mineBegin + 256).empty(), "a thread's empty submission did not publish its ranges");
    }

    std::vector<std::uint32_t> target(16, Nop);
    std::vector<std::uint32_t> submitted{Nop, 0u};
    indirect(submitted, target);
    publication.Note(at(target.data()), target.size() * 4, Thread);
    publication.PublishSubmission(submitted.data(), submitted.size(), Other);
    {
        const auto held = publication.Hold();
        check(publication.Unpublished(held, at(target.data()), at(target.data()) + 64).empty(), "an INDIRECT_BUFFER target written by another thread was not published by the submission");
    }
}

void testTailPublication() {
    CommandPublication publication(anywhere);
    std::vector<std::uint32_t> root{Nop, 1u, RewindHeader, 0u, Nop, 2u, Nop, 3u};
    const auto tail = Range{at(root.data() + 4), at(root.data() + root.size())};
    publication.Note(at(root.data()), root.size() * 4, Thread);
    publication.PublishSubmission(root.data(), root.size(), Thread);
    check(publication.PendingTails() == 1, "a submitted REWIND did not leave its tail pending");
    {
        const auto held = publication.Hold();
        publication.PublishReleasedTails(held);
        check(publication.Unpublished(held, at(root.data()), at(root.data() + root.size())) == std::vector<Range>{tail}, "a REWIND tail was published before its gate word was set");
    }
    publication.Note(tail.begin, 8, Thread);
    publication.PublishSubmission(nullptr, 0, Thread);
    {
        const auto held = publication.Hold();
        check(publication.Unpublished(held, tail.begin, tail.end) == std::vector<Range>{tail}, "the tail writer's own submission published a gated REWIND tail");
    }
    std::atomic_ref<std::uint32_t>(root[3]).store(AgcDriver::Pm4::RewindValid, std::memory_order_release);
    {
        const auto held = publication.Hold();
        publication.PublishReleasedTails(held);
        check(publication.Unpublished(held, at(root.data()), at(root.data() + root.size())).empty(), "a released REWIND tail stayed unpublished");
    }
    check(publication.PendingTails() == 0, "a released REWIND tail stayed pending");
}

void testSweep() {
    CommandPublication publication(anywhere);
    std::vector<std::uint32_t> words(16, Nop);
    publication.Note(at(words.data()), 64, Thread);
    check(publication.SweepStale() == 0, "a range written during the frame that just ended was swept");
    check(publication.SweepStale() == 1 && publication.PendingRanges() == 0, "a range left unpublished for a whole frame was not swept");
    publication.Disable();
    publication.Note(at(words.data()), 64, Thread);
    check(publication.PendingRanges() == 0, "a disabled publication still recorded command writes");
}

}

int main() {
    try {
        testWalk();
        testRewind();
        testWalkLimits();
        testPublication();
        testTailPublication();
        testSweep();
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "%s\n", failure.what());
        return 1;
    }
    std::puts("command publication tests passed");
    return 0;
}
