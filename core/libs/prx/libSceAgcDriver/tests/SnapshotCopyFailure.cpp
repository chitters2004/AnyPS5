#include "prx/libSceAgcDriver/Graphics/include/SnapshotCopies.hpp"
#include "AllocationFailure.hpp"
#include <array>
#include <cstdio>
#include <new>
#include <vector>

namespace {
using AgcDriver::Graphics::Detail::SnapshotCopies;
using AgcDriver::Tests::SetAllocationFailureCountdown;

bool Consistent(const SnapshotCopies& copies) {
    if (copies.entries.size() != copies.recency.size()) return false;
    std::size_t bytes = 0;
    for (auto it = copies.recency.begin(); it != copies.recency.end(); ++it) {
        const auto found = copies.entries.find(*it);
        if (found == copies.entries.end() || found->second.recent != it) return false;
        bytes += found->second.bytes.size();
    }
    return bytes == copies.bytes;
}

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void CheckInsertionFailures() {
    const std::array<std::byte, 64> data{std::byte{0x37}};
    unsigned failures = 0;
    for (int point = 0; point < 4; ++point) {
        SnapshotCopies copies;
        const SnapshotCopies::Key first{0x1000, data.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        const SnapshotCopies::Key second{0x2000, data.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        auto owner = std::make_shared<int>(7);
        const std::shared_ptr<AgcDriver::Graphics::Buffer> buffer(owner, nullptr);
        copies.Insert(first, buffer, data);
        bool failed = false;
        SetAllocationFailureCountdown(point);
        try {
            copies.Insert(second, buffer, data);
        } catch (const std::bad_alloc&) {
            failed = true;
            ++failures;
        }
        SetAllocationFailureCountdown(-1);
        Require(Consistent(copies), "failed snapshot insertion left inconsistent eviction metadata");
        Require(copies.entries.at(first).bytes == std::vector<std::byte>(data.begin(), data.end()), "failed insertion changed an existing snapshot");
        Require(owner.use_count() == (failed ? 3 : 4), "snapshot insertion leaked or lost a buffer reference");
        if (failed) {
            Require(copies.entries.size() == 1 && copies.bytes == data.size(), "failed insertion changed cache accounting");
            copies.Insert(second, buffer, data);
        }
        Require(Consistent(copies) && copies.entries.size() == 2, "cache could not recover after allocation failure");
        const auto oldest = copies.entries.find(copies.recency.front());
        copies.bytes -= oldest->second.bytes.size();
        copies.entries.erase(oldest);
        copies.recency.pop_front();
        Require(Consistent(copies) && copies.entries.contains(second), "eviction after insertion failure corrupted another snapshot");
        copies.Clear();
        Require(Consistent(copies) && copies.bytes == 0 && owner.use_count() == 2, "snapshot teardown retained buffers or stale accounting");
    }
    Require(failures == 3, "did not exercise every snapshot insertion allocation");
}

void CheckMovedInsertion() {
    const SnapshotCopies::Key key{0x3000, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    auto owner = std::make_shared<int>(7);
    const std::shared_ptr<AgcDriver::Graphics::Buffer> buffer(owner, nullptr);
    unsigned failures = 0;
    for (int point = 0; point < 3; ++point) {
        SnapshotCopies copies;
        std::vector<std::byte> data(4096, std::byte{0x5a});
        const auto* storage = data.data();
        bool failed = false;
        SetAllocationFailureCountdown(point);
        try {
            copies.Insert(key, buffer, std::move(data));
        } catch (const std::bad_alloc&) {
            failed = true;
            ++failures;
        }
        SetAllocationFailureCountdown(-1);
        Require(Consistent(copies), "moved snapshot insertion left inconsistent eviction metadata");
        if (failed) {
            Require(copies.entries.empty() && copies.bytes == 0 && data.data() == storage && data.size() == 4096, "failed moved insertion lost the snapshot or changed accounting");
            continue;
        }
        const auto& kept = copies.entries.at(key).bytes;
        Require(kept.data() == storage, "a snapshot cache insert copied the bytes instead of taking them");
        Require(kept.size() == 4096 && kept.front() == std::byte{0x5a} && copies.bytes == 4096, "moved snapshot insertion changed the bytes or the accounting");
        Require(owner.use_count() == 3, "moved snapshot insertion leaked or lost a buffer reference");
        copies.Insert(key, buffer, std::vector<std::byte>(16));
        Require(Consistent(copies) && copies.entries.size() == 1 && copies.bytes == 4096, "a repeated key replaced or double counted a snapshot");
    }
    Require(failures == 2, "did not exercise every moved snapshot insertion allocation");
}
}

int main() {
    try {
        CheckInsertionFailures();
        CheckMovedInsertion();
        std::puts("Snapshot insertion allocation failures preserve eviction metadata and buffer ownership");
    } catch (const std::exception& error) {
        SetAllocationFailureCountdown(-1);
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
