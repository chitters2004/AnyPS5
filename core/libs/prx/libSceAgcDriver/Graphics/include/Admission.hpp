#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_ADMISSION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_ADMISSION_HPP

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>

namespace AgcDriver::Graphics {

class Admission {
public:
    struct Ticket {
        std::uint64_t bytes = 0;
        bool alone = false;
    };

    explicit Admission(std::uint64_t budget) : budget(budget) {
        if (budget == 0) throw std::invalid_argument("an admission budget of zero bytes admits nothing");
    }

    ~Admission() {
        Fail(std::make_exception_ptr(std::runtime_error("AGC graphics: the admission budget was torn down while a producer waited")));
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return waiters == 0; });
    }

    Admission(const Admission&) = delete;
    Admission& operator=(const Admission&) = delete;

    Ticket Admit(std::uint64_t bytes) {
        std::unique_lock lock(mutex);
        rethrow();
        const auto turn = nextTurn++;
        ++waiters;
        struct Leave {
            Admission& admission;
            ~Leave() {
                --admission.waiters;
                admission.changed.notify_all();
            }
        } leave{*this};
        const bool alone = bytes > budget;
        changed.wait(lock, [&] { return error != nullptr || (serving == turn && (alone ? outstanding == 0 : outstanding <= budget && bytes <= budget - outstanding)); });
        rethrow();
        outstanding += bytes;
        ++serving;
        return Ticket{bytes, alone};
    }

    void Release(const Ticket& ticket) noexcept {
        if (ticket.bytes == 0) return;
        {
            std::lock_guard lock(mutex);
            outstanding -= ticket.bytes;
        }
        changed.notify_all();
    }

    void Fail(std::exception_ptr failure) noexcept {
        {
            std::lock_guard lock(mutex);
            if (error == nullptr) error = failure;
        }
        changed.notify_all();
    }

    std::uint64_t Outstanding() const {
        std::lock_guard lock(mutex);
        return outstanding;
    }

    std::uint64_t Budget() const { return budget; }

private:
    void rethrow() const {
        if (error != nullptr) std::rethrow_exception(error);
    }

    const std::uint64_t budget;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t outstanding = 0;
    std::uint64_t nextTurn = 0;
    std::uint64_t serving = 0;
    std::uint64_t waiters = 0;
    std::exception_ptr error;
};

}

#endif
