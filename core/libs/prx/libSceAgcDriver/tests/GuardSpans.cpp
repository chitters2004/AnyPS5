#include "prx/libSceAgcDriver/Graphics/include/GuardSpans.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::GuardMemo;
using AgcDriver::Graphics::GuardSpans;

struct Range {
    std::uint64_t address;
    std::uint64_t bytes;
};

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool Overlapping(const std::vector<Range>& ranges, std::uint64_t address, std::uint64_t bytes) {
    for (const auto& range : ranges) {
        if (address < range.address + range.bytes && range.address < address + bytes) return true;
    }
    return false;
}

void CheckEmpty() {
    GuardSpans spans;
    Require(spans.Generation() == std::numeric_limits<std::uint64_t>::max(), "a new copy claims a generation");
    Require(!spans.Overlaps(0x1000, 0x1000), "a new copy overlaps");
    spans.Fill(0, std::vector<Range>{});
    Require(spans.Generation() == 0, "the filled generation was not kept");
    Require(!spans.Overlaps(0, std::numeric_limits<std::uint32_t>::max()), "an empty list overlaps");
}

void CheckEdges() {
    const std::vector<Range> ranges{{0x10000, 0x1000}, {0x20000, 0}, {0x30000, 0x8000}, {0x31000, 0x100}};
    GuardSpans spans;
    spans.Fill(7, ranges);
    Require(spans.Generation() == 7, "the filled generation was not kept");
    Require(!spans.Overlaps(0xf000, 0x1000), "a range ending at a guard's start overlaps");
    Require(spans.Overlaps(0xf000, 0x1001), "a range reaching a guard's first byte does not overlap");
    Require(spans.Overlaps(0x10fff, 1), "a guard's last byte does not overlap");
    Require(!spans.Overlaps(0x11000, 0x1000), "a range starting at a guard's end overlaps");
    Require(spans.Overlaps(0x1ff00, 0x200), "a range around an empty guard does not overlap as the list check does");
    Require(!spans.Overlaps(0x20000, 0x100), "a range starting at an empty guard overlaps");
    Require(spans.Overlaps(0x37fff, 0x10), "the end of a guard that holds another does not overlap");
    Require(!spans.Overlaps(0x38000, 0x10000), "a range after nested guards overlaps");
}

void CheckAgainstList() {
    std::uint64_t state = 0x9e3779b97f4a7c15ull;
    const auto next = [&] {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        return state;
    };
    for (int round = 0; round < 200; ++round) {
        std::vector<Range> ranges;
        const auto count = next() % 24u;
        for (std::uint64_t index = 0; index < count; ++index) ranges.push_back({(next() % 0x100000u) & ~0xfull, next() % 3u == 0 ? next() % 0x40u : next() % 0x20000u});
        GuardSpans spans;
        spans.Fill(static_cast<std::uint64_t>(round), ranges);
        for (int query = 0; query < 2000; ++query) {
            const auto address = next() % 0x140000u;
            const auto bytes = 1u + next() % (next() % 2u == 0 ? 0x80u : 0x40000u);
            Require(spans.Overlaps(address, bytes) == Overlapping(ranges, address, bytes), "round " + std::to_string(round) + ": the copy disagrees with the list for " + std::to_string(bytes) + " bytes at " + std::to_string(address));
        }
    }
}

void CheckRefill() {
    GuardSpans spans;
    spans.Fill(1, std::vector<Range>{{0x1000, 0x1000}});
    Require(spans.Overlaps(0x1800, 4), "the first list does not overlap");
    spans.Fill(2, std::vector<Range>{{0x8000, 0x1000}});
    Require(!spans.Overlaps(0x1800, 4), "a refill kept the old list");
    Require(spans.Overlaps(0x8800, 4), "a refill lost the new list");
    Require(spans.Generation() == 2, "a refill kept the old generation");
}

}

void CheckMemo() {
    const auto make = [](std::uint64_t depth, std::uint64_t color) {
        GuardMemo::Key key;
        key.depth = depth;
        key.color = color;
        key.Push(0x10000, 0x1000);
        key.Push(0x40000, 0x200);
        return key;
    };
    GuardMemo memo;
    const auto key = make(3, 5);
    Require(!memo.Covers(key, false), "an empty memo covers a buffer set");
    memo.Store(key);
    Require(memo.Covers(key, false), "a stored all-clear set is not covered");
    Require(memo.Covers(make(3, 5), false), "an equal set built again is not covered");
    Require(!memo.Covers(key, true), "a pending overwrite proof was skipped");
    Require(!memo.Covers(make(4, 5), false), "a depth guard generation bump kept the memo");
    Require(!memo.Covers(make(3, 6), false), "a color guard generation bump kept the memo");
    std::uint64_t depth = 4;
    while (make(depth, 5).Way() != key.Way()) ++depth;
    Require(!memo.Covers(make(depth, 5), false), "a depth guard generation bump in the same way kept the memo");
    std::uint64_t color = 6;
    while (make(3, color).Way() != key.Way()) ++color;
    Require(!memo.Covers(make(3, color), false), "a color guard generation bump in the same way kept the memo");
    auto moved = make(3, 5);
    moved.addresses[1] = 0x40100;
    Require(!memo.Covers(moved, false), "a moved buffer kept the memo");
    auto resized = make(3, 5);
    resized.bytes[0] = 0x2000;
    Require(!memo.Covers(resized, false), "a grown buffer kept the memo");
    GuardMemo::Key prefix;
    prefix.depth = 3;
    prefix.color = 5;
    prefix.Push(0x10000, 0x1000);
    memo.Store(prefix);
    auto longer = prefix;
    std::uint64_t extra = 0x80000;
    longer.Push(extra, 0x100);
    while (longer.Way() != prefix.Way()) {
        extra += 0x1000;
        longer.addresses[1] = extra;
    }
    Require(memo.Covers(prefix, false), "a one-buffer set was not covered");
    Require(!memo.Covers(longer, false), "a set with one more buffer than a remembered set was covered");
    auto htile = make(3, 5);
    htile.htile = true;
    htile.htileAddress = 0x10000;
    htile.htileBytes = 0x1000;
    Require(!memo.Covers(htile, false), "an HTILE clear range kept the memo");
    memo.Store(htile);
    Require(memo.Covers(htile, false), "a set with an HTILE clear range was not covered");
    auto otherClear = htile;
    otherClear.htileBytes = 0x800;
    Require(!memo.Covers(otherClear, false), "a different HTILE clear range kept the memo");
    GuardMemo::Key full;
    for (std::size_t index = 0; index <= GuardMemo::Ranges; ++index) full.Push(0x100000 + index * 0x1000, 0x100);
    Require(!full.fits, "a set past the range limit still fits");
    memo.Store(full);
    Require(!memo.Covers(full, false), "a set past the range limit was covered");
}

int main() {
    try {
        CheckEmpty();
        CheckEdges();
        CheckAgainstList();
        CheckRefill();
        CheckMemo();
        std::cout << "guard spans tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
