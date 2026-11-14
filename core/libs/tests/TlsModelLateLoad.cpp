#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <thread>
#include <vector>

namespace {
using Bytes = std::size_t (*)();
using Store = void (*)(std::uint64_t);
using Check = int (*)(std::uint64_t);

struct Library {
    Bytes bytes = nullptr;
    Store store = nullptr;
    Check check = nullptr;
};

constexpr unsigned Threads = 8;
constexpr unsigned Rounds = 4;

bool RunRounds(const Library& library, unsigned first) {
    std::atomic<unsigned> failures{0};
    std::barrier stored(Threads);
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < Threads; ++thread) {
        workers.emplace_back([&, thread] {
            for (unsigned round = 0; round < Rounds; ++round) {
                const std::uint64_t seed = (std::uint64_t(first + thread) << 8) | round;
                library.store(seed);
                stored.arrive_and_wait();
                if (!library.check(seed)) ++failures;
                stored.arrive_and_wait();
            }
        });
    }
    for (auto& worker : workers) worker.join();
    return failures == 0;
}
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <library>\n", argv[0]);
        return 2;
    }
    std::barrier ready(Threads + 1);
    std::barrier loaded(Threads + 1);
    std::barrier stored(Threads);
    std::atomic<unsigned> failures{0};
    Library library;
    std::vector<std::thread> early;
    for (unsigned thread = 0; thread < Threads; ++thread) {
        early.emplace_back([&, thread] {
            ready.arrive_and_wait();
            loaded.arrive_and_wait();
            if (!library.store) return;
            const std::uint64_t seed = 0x1000 + thread;
            library.store(seed);
            stored.arrive_and_wait();
            if (!library.check(seed)) ++failures;
        });
    }
    ready.arrive_and_wait();
    void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle) {
        library.bytes = reinterpret_cast<Bytes>(dlsym(handle, "TlsModelBytes"));
        library.store = reinterpret_cast<Store>(dlsym(handle, "TlsModelStore"));
        library.check = reinterpret_cast<Check>(dlsym(handle, "TlsModelCheck"));
    }
    if (!handle || !library.bytes || !library.store || !library.check) {
        std::fprintf(stderr, "late dlopen of %s failed: %s\n", argv[1], dlerror());
        library = {};
        loaded.arrive_and_wait();
        for (auto& thread : early) thread.join();
        return 1;
    }
    loaded.arrive_and_wait();
    for (auto& thread : early) thread.join();
    if (failures) {
        std::fprintf(stderr, "%u threads that existed before the load saw another thread's thread-local values\n", failures.load());
        return 1;
    }
    if (library.bytes() < 64 * 1024) {
        std::fprintf(stderr, "the fixture has %zu bytes of thread-local storage, too few to overflow the static TLS surplus\n", library.bytes());
        return 1;
    }
    library.store(1);
    if (!RunRounds(library, 2) || !library.check(1)) {
        std::fprintf(stderr, "threads started after the load saw another thread's thread-local values\n");
        return 1;
    }
    return 0;
}
