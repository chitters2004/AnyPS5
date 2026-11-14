#include "prx/libSceAgcDriver/Execution/include/CommandPublication.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <algorithm>
#include <atomic>
#include <iterator>
#include <vector>

namespace AgcDriver {
namespace {

std::uint64_t addressOf(const void* pointer) {
    return reinterpret_cast<std::uintptr_t>(pointer);
}

const std::uint32_t* commandsAt(std::uintptr_t address) {
    return reinterpret_cast<const std::uint32_t*>(address);
}

bool walkSegment(const std::uint32_t* guest, std::size_t words, std::size_t depth, std::size_t& budget, const CommandPublication::Accessible& accessible,
                 const CommandPublication::Visit& visit, CommandPublication::Reach& reach) {
    if (depth >= CommandPublication::MaximumDepth) return true;
    for (;;) {
        if (words > budget || !accessible(guest, words * sizeof(std::uint32_t))) return true;
        budget -= words;
        reach.segments.push_back({addressOf(guest), addressOf(guest + words)});
        const std::uint32_t* chained = nullptr;
        std::size_t chainedWords = 0;
        std::vector<std::size_t> guarded;
        const auto reachCursor = [&](std::size_t cursor, std::size_t next) {
            std::erase(guarded, cursor);
            return std::ranges::all_of(guarded, [&](std::size_t end) { return end >= next; });
        };
        std::size_t cursor = 0;
        for (; cursor < words;) {
            const auto* packet = guest + cursor;
            const auto header = *packet;
            if (Pm4::FillerPacket(header)) {
                if (!reachCursor(cursor, cursor + 1)) return true;
                if (visit) visit(std::span(packet, 1));
                ++cursor;
                continue;
            }
            const auto count = Pm4::PacketWords(header);
            if ((header & 0xc0000000u) != 0xc0000000u || count > words - cursor) {
                if (visit) visit(std::span(packet, words - cursor));
                return false;
            }
            if (!reachCursor(cursor, cursor + count)) return true;
            const auto opcode = (header >> 8u) & 0xffu;
            if (opcode == Pm4::IndirectBufferOpcode && count == Pm4::ConditionalBufferWords) {
                const auto* target = commandsAt(Pm4::LinkAddress(packet[8], packet[9]));
                const std::size_t targetWords = Pm4::LinkWords(packet[10]);
                if (targetWords != 0) {
                    if (walkSegment(target, targetWords, depth + 1, budget, accessible, visit, reach)) return true;
                    if (guarded.empty()) return false;
                }
                cursor += count;
                continue;
            }
            if (opcode == Pm4::IndirectBufferOpcode) {
                if (count != Pm4::IndirectBufferWords) return true;
                const auto* target = commandsAt(Pm4::LinkAddress(packet[1], packet[2]));
                const std::size_t targetWords = Pm4::LinkWords(packet[3]);
                const bool chain = Pm4::LinkChains(packet[3]);
                if (chain && !guarded.empty()) return true;
                if (Pm4::Predicated(header)) {
                    if (chain) return true;
                    if (visit) visit(std::span(packet, count));
                } else if (chain) {
                    chained = target;
                    chainedWords = targetWords;
                    break;
                }
                if (walkSegment(target, targetWords, depth + 1, budget, accessible, visit, reach)) return true;
                cursor += count;
                continue;
            }
            if (opcode == Pm4::RewindOpcode) {
                if (count != Pm4::RewindWords || !guarded.empty()) return true;
                if (visit) visit(std::span(packet, count));
                reach.tails.push_back({addressOf(packet + 1), {addressOf(packet + count), addressOf(guest + words)}});
                return true;
            }
            if (opcode == Pm4::ConditionalExecOpcode && count == Pm4::ConditionalExecWords) guarded.push_back(cursor + count + Pm4::ConditionalWords(std::span(packet, count)));
            if (visit) visit(std::span(packet, count));
            cursor += count;
        }
        if (chained == nullptr) return !reachCursor(words, words) || !guarded.empty();
        guest = chained;
        words = chainedWords;
    }
}

}

CommandPublication::Reach CommandPublication::Walk(const std::uint32_t* commands, std::size_t words, const Accessible& accessible, const Visit& visit) {
    Reach reach;
    std::size_t budget = WordBudget;
    if (commands != nullptr && words != 0) walkSegment(commands, words, 0, budget, accessible, visit, reach);
    return reach;
}

CommandPublication::CommandPublication(Accessible accessible) : accessible(std::move(accessible)) {}

void CommandPublication::Note(std::uint64_t address, std::size_t bytes, std::uint64_t thread) {
    if (bytes == 0 || !enabled.load(std::memory_order_relaxed)) return;
    const auto end = address + bytes;
    std::lock_guard lock(mutex);
    subtract(address, end);
    auto at = pending.emplace(address, Entry{end, thread, epoch}).first;
    if (at != pending.begin()) {
        const auto previous = std::prev(at);
        if (previous->second.end == address && previous->second.thread == thread) {
            previous->second.end = end;
            previous->second.epoch = epoch;
            pending.erase(at);
            at = previous;
        }
    }
    const auto next = std::next(at);
    if (next != pending.end() && next->first == at->second.end && next->second.thread == thread) {
        at->second.end = next->second.end;
        pending.erase(next);
    }
}

void CommandPublication::PublishSubmission(const std::uint32_t* commands, std::size_t words, std::uint64_t thread) {
    if (!enabled.load(std::memory_order_relaxed)) return;
    const auto reach = Walk(commands, words, accessible);
    std::lock_guard lock(mutex);
    publishReach(reach);
    for (auto at = pending.begin(); at != pending.end();) at = at->second.thread == thread ? pending.erase(at) : std::next(at);
}

std::size_t CommandPublication::SweepStale() {
    std::lock_guard lock(mutex);
    ++epoch;
    std::size_t swept = 0;
    for (auto at = pending.begin(); at != pending.end();) {
        if (at->second.epoch + 2 > epoch) {
            ++at;
            continue;
        }
        at = pending.erase(at);
        ++swept;
    }
    return swept;
}

void CommandPublication::Disable() {
    enabled.store(false, std::memory_order_relaxed);
}

CommandPublication::Held CommandPublication::Hold() {
    return Held(mutex);
}

void CommandPublication::PublishReleasedTails(const Held&) {
    for (bool released = true; released;) {
        released = false;
        for (std::size_t index = 0; index < tails.size(); ++index) {
            auto& word = *reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(tails[index].word));
            if ((std::atomic_ref<std::uint32_t>(word).load(std::memory_order_acquire) & Pm4::RewindValid) == 0) continue;
            const auto range = tails[index].range;
            tails.erase(tails.begin() + static_cast<std::ptrdiff_t>(index));
            subtract(range.begin, range.end);
            publishReach(Walk(commandsAt(range.begin), static_cast<std::size_t>((range.end - range.begin) / sizeof(std::uint32_t)), accessible));
            released = true;
            break;
        }
    }
}

std::vector<CommandPublication::Range> CommandPublication::Unpublished(const Held&, std::uint64_t begin, std::uint64_t end) const {
    std::vector<Range> result;
    const auto clip = [&](std::uint64_t first, std::uint64_t last) {
        first = std::max(first, begin);
        last = std::min(last, end);
        if (first < last) result.push_back({first, last});
    };
    auto at = pending.upper_bound(begin);
    if (at != pending.begin()) --at;
    for (; at != pending.end() && at->first < end; ++at) clip(at->first, at->second.end);
    for (const auto& tail : tails) clip(tail.range.begin, tail.range.end);
    std::ranges::sort(result, {}, &Range::begin);
    std::vector<Range> merged;
    for (const auto& range : result) {
        if (!merged.empty() && range.begin <= merged.back().end) merged.back().end = std::max(merged.back().end, range.end);
        else merged.push_back(range);
    }
    return merged;
}

std::size_t CommandPublication::PendingRanges() {
    std::lock_guard lock(mutex);
    return pending.size();
}

std::size_t CommandPublication::PendingTails() {
    std::lock_guard lock(mutex);
    return tails.size();
}

void CommandPublication::subtract(std::uint64_t begin, std::uint64_t end) {
    auto at = pending.upper_bound(begin);
    if (at != pending.begin()) --at;
    while (at != pending.end() && at->first < end) {
        const auto first = at->first;
        const auto entry = at->second;
        if (entry.end <= begin) {
            ++at;
            continue;
        }
        at = pending.erase(at);
        if (first < begin) pending.emplace(first, Entry{begin, entry.thread, entry.epoch});
        if (entry.end > end) {
            pending.emplace(end, Entry{entry.end, entry.thread, entry.epoch});
            break;
        }
    }
}

void CommandPublication::publishReach(const Reach& reach) {
    for (const auto& segment : reach.segments) subtract(segment.begin, segment.end);
    for (const auto& tail : reach.tails) {
        if (std::ranges::none_of(tails, [&](const Tail& known) { return known.word == tail.word; })) tails.push_back(tail);
    }
}

}
