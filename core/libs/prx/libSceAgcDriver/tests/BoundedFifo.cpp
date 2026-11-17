#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/BoundedFifo.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::atomic<std::size_t> allocations{0};

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("bounded fifo: " + message);
}

struct Held {
    std::function<void()> run;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> writes;
};

template <typename Queue>
std::size_t Churn(Queue& queue, std::size_t rounds) {
    const auto before = allocations.load();
    for (std::size_t round = 0; round < rounds; ++round) {
        while (queue.size() < 12) queue.push_back(Held{[] {}, {}});
        while (queue.size() > 4) queue.pop_front();
    }
    return allocations.load() - before;
}

}

void* operator new(std::size_t bytes) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    try {
        AgcDriver::DriverDetail::BoundedFifo<Held> fifo(16);
        const auto fifoAllocations = Churn(fifo, 256);
        std::deque<Held> deque;
        const auto dequeAllocations = Churn(deque, 256);
        std::printf("bounded fifo: %zu allocations over 256 rounds of 8 pushes and pops; a deque made %zu\n", fifoAllocations, dequeAllocations);
        Require(fifoAllocations == 0, "the ring allocated " + std::to_string(fifoAllocations) + " times while it cycled under its capacity");
        Require(dequeAllocations > 0, "the deque baseline made no allocations, so the churn does not reach the case the ring replaces");

        AgcDriver::DriverDetail::BoundedFifo<int> order(4);
        const int* first = nullptr;
        for (int value = 0; value < 4; ++value) {
            order.push_back(value);
            if (value == 0) first = &order.front();
        }
        Require(&order.front() == first, "pushes moved the front element");
        Require(order.full(), "four pushes did not fill a ring of four");
        bool refused = false;
        try {
            order.push_back(4);
        } catch (const std::logic_error&) {
            refused = true;
        }
        Require(refused, "a push past the capacity was not refused");
        order.pop_front();
        order.push_back(4);
        std::vector<int> seen(order.begin(), order.end());
        Require(seen == std::vector<int>{1, 2, 3, 4}, "the ring did not keep first-in, first-out order across the wrap");
        std::vector<int> reversed(order.rbegin(), order.rend());
        Require(reversed == std::vector<int>{4, 3, 2, 1}, "reverse iteration did not walk from the back");
        Require(order[0] == 1 && order[3] == 4, "indexing is not relative to the front");
        std::puts("bounded fifo tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
