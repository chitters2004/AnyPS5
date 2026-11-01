#include "prx/libc/include/HostThreadSlot.hpp"
#include <atomic>
#include <cstdio>
#include <thread>

struct Certificate {
    unsigned long long growth = 0;
    std::atomic<bool> alive{true};
};

using Slot = HostThreadSlot<Certificate*, Certificate>;
Certificate retired{};
std::atomic<int> deadReads{0}, sentinelReads{0}, liveReads{0};

#ifdef FIXED
struct Release {
    ~Release() {
        auto* certificate = Slot::Get();
        Slot::Set(&retired);
        if (certificate != &retired) delete certificate;
    }
};
void remember() {
    auto* certificate = Slot::Get();
    if (certificate == &retired) return;
    if (certificate == nullptr) {
        [[maybe_unused]] thread_local Release release;
        certificate = new Certificate{};
        Slot::Set(certificate);
    }
    certificate->growth = 1;
}
#else
struct Owned {
    Certificate certificate;
    ~Owned() { certificate.alive.store(false, std::memory_order_seq_cst); }
};
void remember() {
    auto* certificate = Slot::Get();
    if (certificate == nullptr) {
        thread_local Owned owned;
        certificate = &owned.certificate;
        Slot::Set(certificate);
    }
    certificate->growth = 1;
}
#endif

void read() {
    const auto* certificate = Slot::Get();
    if (certificate == &retired) sentinelReads.fetch_add(1);
    else if (certificate != nullptr && !certificate->alive) deadReads.fetch_add(1);
    else liveReads.fetch_add(1);
}

struct TeardownReads {
    bool armed = false;
    ~TeardownReads() {
        if (armed) read();
    }
};

int main() {
    for (int index = 0; index < 64; ++index) {
        std::thread([] {
            thread_local TeardownReads reads;
            reads.armed = true;
            remember();
        }).join();
    }
    std::printf("%s: teardown reads of a dead certificate=%d, of the retired sentinel=%d, of a live one=%d\n",
#ifdef FIXED
        "fixed",
#else
        "original",
#endif
        deadReads.load(), sentinelReads.load(), liveReads.load());
    return deadReads.load() == 0 ? 0 : 1;
}
