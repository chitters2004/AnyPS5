#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SUBMISSION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SUBMISSION_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/SubmissionProgress.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AgcDriver::DriverDetail {

struct FlipWork {
    bool finished = false;
    std::uint64_t serial = 0;
    std::shared_ptr<FlipWork> previous;
};

class RegisterLists {
public:
    void clear() noexcept {
        entries.clear();
        words.clear();
    }

    [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }

    template <typename Fill>
    void Emplace(std::size_t cursor, Fill&& fill) {
        if (!entries.empty() && entries.back().cursor >= cursor) throw std::logic_error("register lists are added in cursor order");
        const auto offset = words.size();
        fill(words);
        entries.push_back({cursor, offset, words.size() - offset});
    }

    void Add(std::size_t cursor, std::span<const std::uint32_t> pairs) {
        Emplace(cursor, [&](std::vector<std::uint32_t>& out) { out.insert(out.end(), pairs.begin(), pairs.end()); });
    }

    [[nodiscard]] std::optional<std::span<const std::uint32_t>> Find(std::size_t cursor) const noexcept {
        const auto found = std::lower_bound(entries.begin(), entries.end(), cursor, [](const Entry& entry, std::size_t key) { return entry.cursor < key; });
        if (found == entries.end() || found->cursor != cursor) return std::nullopt;
        return std::span<const std::uint32_t>(words).subspan(found->offset, found->count);
    }

    [[nodiscard]] std::span<const std::uint32_t> At(std::size_t cursor) const {
        const auto found = Find(cursor);
        if (!found) throw std::out_of_range("no register list at this cursor");
        return *found;
    }

    void Splice(std::size_t start, std::size_t end, std::ptrdiff_t delta, const RegisterLists& inserted) {
        RegisterLists spliced;
        spliced.entries.reserve(entries.size() + inserted.entries.size());
        spliced.words.reserve(words.size() + inserted.words.size());
        const auto copy = [&](const RegisterLists& from, const Entry& entry, std::size_t cursor) {
            const auto pairs = std::span<const std::uint32_t>(from.words).subspan(entry.offset, entry.count);
            spliced.Add(cursor, pairs);
        };
        for (const auto& entry : entries) {
            if (entry.cursor >= start) break;
            copy(*this, entry, entry.cursor);
        }
        for (const auto& entry : inserted.entries) copy(inserted, entry, start + entry.cursor);
        for (const auto& entry : entries) {
            if (entry.cursor < end) continue;
            copy(*this, entry, static_cast<std::size_t>(static_cast<std::ptrdiff_t>(entry.cursor) + delta));
        }
        *this = std::move(spliced);
    }

private:
    struct Entry {
        std::size_t cursor;
        std::size_t offset;
        std::size_t count;
    };
    std::vector<Entry> entries;
    std::vector<std::uint32_t> words;
};

struct Submission {
    std::uint64_t serial;
    std::uint32_t queue;
    std::vector<std::uint32_t> commands;
    std::shared_ptr<SubmissionProgress> progress;

    std::shared_ptr<const ShaderRegistry> shaders;
    std::map<std::size_t, std::shared_ptr<IFlipRequest>> flips;
    std::map<std::size_t, std::uint32_t> flipHandles;
    std::map<std::size_t, std::shared_ptr<IRenderingWait>> renderingWaits;
    std::map<std::size_t, std::size_t> conditionalEnds;
    RegisterLists registerLists;
    bool suspend = false;
    bool waitFree = false;
    bool holdsFlip = false;
    std::shared_ptr<FlipWork> flipWork;

    std::uint64_t received = 0;
    std::vector<std::uint64_t> labelWrites;
    std::set<std::size_t> heldAtSubmit;
    std::chrono::steady_clock::time_point receivedAt{};
    std::chrono::steady_clock::time_point copiedAt{};
    std::chrono::steady_clock::time_point validatedAt{};
    std::chrono::steady_clock::time_point roomReadyAt{};
    std::chrono::steady_clock::time_point enqueuedAt{};
    std::chrono::steady_clock::time_point dequeuedAt{};
    std::chrono::steady_clock::time_point orderedAt{};
    const std::uint32_t* rewindTail = nullptr;
    std::size_t rewindWords = 0;
    using PreparedWaits = std::map<std::pair<std::uint32_t, std::uint32_t>, std::shared_ptr<IRenderingWait>>;
    struct IndirectSegment {
        std::size_t start;
        const std::uint32_t* target;
        std::size_t words;
        std::shared_ptr<const PreparedWaits> waits{};
    };
    std::vector<IndirectSegment> indirectSegments;
};

struct QueueWorker {
    std::deque<Submission> pending;

    std::atomic<std::uint64_t> queued{0};
    std::unordered_map<std::uint64_t, std::uint32_t> unfinishedWrites;
    std::uint64_t executingWaitFree = 0;
    bool executingWaits = false;
    alignas(64) std::atomic<std::uint64_t> executingPacket{0};
    std::thread thread;
};

struct ReferencedProgramScan {
    std::vector<std::uint64_t> addresses;
    bool complete = true;
};

ReferencedProgramScan ReferencedPrograms(std::span<const std::uint32_t> commands, const RegisterLists& registerLists);

}

#endif
