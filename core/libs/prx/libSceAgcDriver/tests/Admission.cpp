#include "prx/libSceAgcDriver/Graphics/include/Admission.hpp"
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::Admission;

void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class Waiter {
public:
    Waiter(Admission& admission, std::uint64_t bytes) : thread([this, &admission, bytes] {
        try {
            ticket = admission.Admit(bytes);
        } catch (const std::exception& error) {
            message = error.what();
        }
        done.store(true, std::memory_order_release);
    }) {}

    ~Waiter() {
        if (thread.joinable()) thread.join();
    }

    bool Done() const { return done.load(std::memory_order_acquire); }

    bool DoneWithin(std::chrono::milliseconds bound) const {
        const auto deadline = std::chrono::steady_clock::now() + bound;
        while (!Done() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return Done();
    }

    Admission::Ticket Ticket() {
        thread.join();
        return ticket;
    }

    std::string Message() {
        thread.join();
        return message;
    }

private:
    Admission::Ticket ticket;
    std::string message;
    std::atomic<bool> done{false};
    std::thread thread;
};

constexpr auto Blocked = std::chrono::milliseconds(50);
constexpr auto Bound = std::chrono::seconds(5);

void TestWithinBudget() {
    Admission admission(100);
    const auto first = admission.Admit(60);
    const auto second = admission.Admit(40);
    Check(admission.Outstanding() == 100 && !first.alone && !second.alone, "two items that fit the budget were not both admitted");
    Waiter third(admission, 1);
    Check(!third.DoneWithin(Blocked), "an item past the budget was admitted without a release");
    admission.Release(first);
    Check(third.DoneWithin(Bound), "a release did not admit the waiting item");
    const auto ticket = third.Ticket();
    Check(ticket.bytes == 1 && admission.Outstanding() == 41, "the admitted item was charged wrongly");
    admission.Release(second);
    admission.Release(ticket);
    Check(admission.Outstanding() == 0, "released items left bytes charged");
}

void TestOrderAndOversized() {
    Admission admission(100);
    const auto held = admission.Admit(50);
    Waiter large(admission, 250);
    Check(!large.DoneWithin(Blocked), "an oversized item was admitted while other bytes were charged");
    Waiter small(admission, 10);
    Check(!small.DoneWithin(Blocked), "a later small item overtook a waiting oversized one");
    admission.Release(held);
    Check(large.DoneWithin(Bound), "an oversized item was not admitted once nothing was charged");
    const auto alone = large.Ticket();
    Check(alone.alone && alone.bytes == 250, "an oversized item was not admitted alone");
    Check(!small.DoneWithin(Blocked), "an item was admitted beside an oversized one");
    admission.Release(alone);
    Check(small.DoneWithin(Bound), "the item after an oversized one was not admitted once it was released");
    admission.Release(small.Ticket());
    Check(admission.Outstanding() == 0, "the order test left bytes charged");
}

void TestOversizedNearTheLimit() {
    Admission admission(100);
    const auto huge = admission.Admit(std::numeric_limits<std::uint64_t>::max());
    Check(huge.alone && admission.Outstanding() == std::numeric_limits<std::uint64_t>::max(), "the largest item was not admitted alone");
    Waiter small(admission, 1);
    Check(!small.DoneWithin(Blocked), "an item was admitted beside the largest oversized one");
    admission.Release(huge);
    Check(small.DoneWithin(Bound), "the item after the largest oversized one was not admitted once it was released");
    const auto ticket = small.Ticket();
    Check(ticket.bytes == 1 && admission.Outstanding() == 1, "the item after the largest oversized one was charged wrongly");
    admission.Release(ticket);
}

void TestFailure() {
    Admission admission(100);
    const auto held = admission.Admit(100);
    Waiter waiter(admission, 1);
    Check(!waiter.DoneWithin(Blocked), "an item past the budget was admitted");
    admission.Fail(std::make_exception_ptr(std::runtime_error("injected device loss")));
    Check(waiter.DoneWithin(Bound) && waiter.Message() == "injected device loss", "a failure did not wake a waiting producer with its error");
    std::string refused;
    try {
        static_cast<void>(admission.Admit(1));
    } catch (const std::exception& error) {
        refused = error.what();
    }
    Check(refused == "injected device loss", "an admission after a failure was not refused");
    admission.Release(held);
}

void TestTeardown() {
    auto admission = std::make_unique<Admission>(100);
    const auto held = admission->Admit(100);
    Waiter waiter(*admission, 1);
    Check(!waiter.DoneWithin(Blocked), "an item past the budget was admitted");
    admission.reset();
    Check(waiter.DoneWithin(Bound) && waiter.Message().find("torn down") != std::string::npos, "a teardown did not wake a waiting producer, or did not say so");
    static_cast<void>(held);
}

void TestZeroBudget() {
    bool refused = false;
    try {
        Admission admission(0);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    Check(refused, "a zero budget was accepted");
}

}

int main() {
    try {
        TestWithinBudget();
        TestOrderAndOversized();
        TestOversizedNearTheLimit();
        TestFailure();
        TestTeardown();
        TestZeroBudget();
        std::cout << "admission tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
