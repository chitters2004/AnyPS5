#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUARDSPANS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUARDSPANS_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

class GuardSpans {
public:
    std::uint64_t Generation() const { return generation; }

    template<typename TRanges>
    void Fill(std::uint64_t filled, const TRanges& ranges) {
        spans.clear();
        for (const auto& range : ranges) spans.emplace_back(range.address, range.address + range.bytes);
        std::ranges::sort(spans);
        std::uint64_t reach = 0;
        for (auto& span : spans) span.second = reach = std::max(reach, span.second);
        generation = filled;
    }

    bool Overlaps(std::uint64_t address, std::uint64_t bytes) const {
        const auto end = address + bytes;
        const auto after = std::ranges::lower_bound(spans, end, {}, &std::pair<std::uint64_t, std::uint64_t>::first);
        return after != spans.begin() && std::prev(after)->second > address;
    }

private:
    std::uint64_t generation = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
};

class GuardMemo {
public:
    static constexpr std::size_t Ways = 16;
    static constexpr std::size_t Ranges = 16;

    struct Key {
        std::uint64_t depth = 0;
        std::uint64_t color = 0;
        std::uint64_t htileAddress = 0;
        std::uint64_t htileBytes = 0;
        bool htile = false;
        bool fits = true;
        std::uint32_t count = 0;
        std::array<std::uint64_t, Ranges> addresses{};
        std::array<std::uint64_t, Ranges> bytes{};

        void Push(std::uint64_t address, std::uint64_t size) {
            if (count == Ranges) {
                fits = false;
                return;
            }
            addresses[count] = address;
            bytes[count] = size;
            ++count;
        }

        bool Same(const Key& other) const {
            if (depth != other.depth || color != other.color || htile != other.htile || htileAddress != other.htileAddress || htileBytes != other.htileBytes || count != other.count) return false;
            return std::equal(addresses.begin(), addresses.begin() + count, other.addresses.begin()) && std::equal(bytes.begin(), bytes.begin() + count, other.bytes.begin());
        }

        std::size_t Way() const {
            std::uint64_t mixed = (depth * 0x9e3779b97f4a7c15ull) ^ (color * 0xc2b2ae3d27d4eb4full) ^ htileAddress ^ (htileBytes << 1) ^ count;
            for (std::uint32_t index = 0; index < count; ++index) mixed = (mixed ^ addresses[index] ^ (bytes[index] << 7)) * 0x100000001b3ull;
            return static_cast<std::size_t>(mixed ^ (mixed >> 29)) % Ways;
        }
    };

    bool Covers(const Key& key, bool proofsPending) const {
        if (proofsPending || !key.fits) return false;
        const auto way = key.Way();
        return valid[way] && entries[way].Same(key);
    }

    void Store(const Key& key) {
        if (!key.fits) return;
        const auto way = key.Way();
        entries[way] = key;
        valid[way] = true;
    }

private:
    std::array<Key, Ways> entries{};
    std::array<bool, Ways> valid{};
};

enum class GuardSpanEvent : std::size_t { Clear, Refill, Overlap, Verified, Missed, Count };

void NoteGuardSpans(GuardSpanEvent event);
bool VerifyGuardSpans();
void ReportGuardSpanMiss(const char* list, std::uint64_t address, std::uint64_t bytes);
bool GuardMemoVerifyDue();
void ReportGuardMemoMiss();

}

#endif
