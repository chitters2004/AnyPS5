#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMMANDPUBLICATION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMMANDPUBLICATION_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver {

class CommandPublication {
public:
    using Accessible = std::function<bool(const void* address, std::size_t bytes)>;
    using Visit = std::function<void(std::span<const std::uint32_t> packet)>;

    struct Range {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        bool operator==(const Range&) const = default;
    };
    struct Tail {
        std::uint64_t word = 0;
        Range range;
    };
    struct Reach {
        std::vector<Range> segments;
        std::vector<Tail> tails;
    };

    static constexpr std::size_t MaximumDepth = 64;
    static constexpr std::size_t WordBudget = std::size_t{1} << 26u;

    static Reach Walk(const std::uint32_t* commands, std::size_t words, const Accessible& accessible, const Visit& visit = {});

    class Held {
    public:
        explicit Held(std::mutex& mutex) : lock(mutex) {}

    private:
        std::unique_lock<std::mutex> lock;
    };

    explicit CommandPublication(Accessible accessible);

    void Note(std::uint64_t address, std::size_t bytes, std::uint64_t thread);
    void PublishSubmission(const std::uint32_t* commands, std::size_t words, std::uint64_t thread);
    std::size_t SweepStale();
    void Disable();

    Held Hold();
    void PublishReleasedTails(const Held& held);
    std::vector<Range> Unpublished(const Held& held, std::uint64_t begin, std::uint64_t end) const;

    std::size_t PendingRanges();
    std::size_t PendingTails();

private:
    struct Entry {
        std::uint64_t end = 0;
        std::uint64_t thread = 0;
        std::uint64_t epoch = 0;
    };

    void subtract(std::uint64_t begin, std::uint64_t end);
    void publishReach(const Reach& reach);

    Accessible accessible;
    std::mutex mutex;
    std::map<std::uint64_t, Entry> pending;
    std::vector<Tail> tails;
    std::uint64_t epoch = 0;
    std::atomic<bool> enabled{true};
};

}

#endif
