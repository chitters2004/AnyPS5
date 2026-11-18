#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

std::atomic<std::uint64_t> newCalls{0};

}

#ifndef _WIN32
void* operator new(std::size_t bytes) {
    newCalls.fetch_add(1, std::memory_order_relaxed);
    if (void* block = std::malloc(bytes != 0 ? bytes : 1)) return block;
    throw std::bad_alloc();
}
void operator delete(void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }
#endif

namespace {

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class StderrCapture {
public:
    StderrCapture() : path(std::filesystem::temp_directory_path() / ("aps5_pipeline_profile_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt")) {
        std::fflush(stderr);
#ifdef _WIN32
        saved = _dup(_fileno(stderr));
        Check(saved >= 0, "cannot save stderr");
        Check(_wfreopen(path.wstring().c_str(), L"wb", stderr) != nullptr, "cannot redirect stderr");
#else
        saved = dup(fileno(stderr));
        Check(saved >= 0, "cannot save stderr");
        Check(std::freopen(path.c_str(), "wb", stderr) != nullptr, "cannot redirect stderr");
#endif
    }

    ~StderrCapture() {
        std::fflush(stderr);
#ifdef _WIN32
        _dup2(saved, _fileno(stderr));
        _close(saved);
#else
        dup2(saved, fileno(stderr));
        close(saved);
#endif
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    std::string ReadReport() const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            std::ifstream file(path, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            const auto begin = text.find("[draw] pipelined ");
            if (begin != std::string::npos) {
                const auto end = text.find('\n', begin);
                if (end != std::string::npos) return text.substr(begin, end - begin);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("pipeline profile report was not written");
    }

private:
    std::filesystem::path path;
    int saved = -1;
};

std::size_t Count(std::string_view text, std::string_view value) {
    std::size_t count = 0;
    for (std::size_t offset = 0; (offset = text.find(value, offset)) != std::string_view::npos; offset += value.size()) ++count;
    return count;
}

void TestReport() {
    using AgcDriver::DriverDetail::DrawPipeline;
    auto& pipeline = DrawPipeline::Queue0();
    Check(DrawPipeline::Depth() >= 2, "test requires at least two pipeline slots");
    const auto reportAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(10100);
    const auto clockReads = DrawPipeline::CommitClockReads();
    StderrCapture capture;
    std::binary_semaphore release{0};
    std::exception_ptr failure;
    try {
        pipeline.Enqueue([&] { release.acquire(); }, {}, 0, {}, {{0x1000, 0x1010}, {0x2000, 0x2010}}, true);
        Check(DrawPipeline::HeldReads(0x1000, 4), "queued read range was not held");
        for (unsigned i = 0; i < 3; ++i) pipeline.NoteDecodeDrain();
        std::this_thread::sleep_until(reportAfter);
        pipeline.Enqueue([] {}, {}, 0, {}, {}, true);
    } catch (...) {
        failure = std::current_exception();
    }
    release.release();
    pipeline.Stop();
    if (failure) std::rethrow_exception(failure);
    const auto report = capture.ReadReport();
    std::cout << report << '\n';
    Check(DrawPipeline::CommitClockReads() - clockReads == 4, "a timed commit did not read the clock exactly twice");
    Check(report.find(" 2 read ranges, 1 stores held, 3 decode drains") != std::string::npos, "pipeline read counters changed");
    Check(Count(report, " read ranges") == 1, "pipeline read-range counters were printed more than once");
    Check(Count(report, " stores held") == 1, "pipeline held-store counters were printed more than once");
    Check(Count(report, " decode drains") == 1, "pipeline decode-drain counters were printed more than once");
    Check(report.find("; drains:") != std::string::npos, "pipeline drain counters were lost");
    Check(report.find("packet drains by opcode:") != std::string::npos, "pipeline opcode counters were lost");
}

void TestUntimedCommits() {
    using AgcDriver::DriverDetail::DrawPipeline;
    auto& pipeline = DrawPipeline::Queue0();
    Check(DrawPipeline::Depth() >= 2, "test requires at least two pipeline slots");
    unsigned ran = 0;
    for (unsigned i = 0; i < 3; ++i) pipeline.Enqueue([&] { ++ran; }, {}, 0, {}, {}, true);
    pipeline.Stop();
    Check(ran == 3, "the queued commits did not run");
    Check(DrawPipeline::CommitClockReads() == 0, "the committer read the clock with draw profiling and pipeline statistics off");
}

void TestWatchReuse() {
    using AgcDriver::DriverDetail::DrawPipeline;
    auto& pipeline = DrawPipeline::Queue0();
    Check(DrawPipeline::Depth() >= 2, "test requires at least two pipeline slots");
    std::binary_semaphore release{0};
    std::exception_ptr failure;
    DrawPipeline::Watch watch;
    try {
        pipeline.Enqueue([&] { release.acquire(); }, {{0x1000, 0x1010}, {0x2000, 0x2010}}, 0, {}, {}, true);
        pipeline.Enqueue([] {}, {{0x3000, 0x3010}}, 0, {}, {}, true);
        pipeline.WatchWrites(watch);
        const auto first = watch.writes;
        Check(watch.items >= 1 && std::find(first.begin(), first.end(), DrawPipeline::Range{0x3000, 0x3010}) != first.end(), "a watch missed a queued item's writes");
        const auto before = newCalls.load(std::memory_order_relaxed);
        pipeline.WatchWrites(watch);
        const auto during = newCalls.load(std::memory_order_relaxed) - before;
#ifndef _WIN32
        Check(during == 0, "a watch refilled into its own storage allocated");
#endif
        Check(watch.writes == first, "a refilled watch kept or lost queued writes");
    } catch (...) {
        failure = std::current_exception();
    }
    release.release();
    pipeline.Stop();
    if (failure) std::rethrow_exception(failure);
    Check(pipeline.WrittenSince(watch, {{0x3004, 0x3008}}), "a write that retired after the watch was not reported");
    Check(!pipeline.WrittenSince(watch, {{0x9000, 0x9008}}), "a range no item wrote was reported written");
    pipeline.WatchWrites(watch);
    Check(watch.items == 0 && watch.writes.empty() && watch.retired == pipeline.Retired(), "an idle watch kept the last queue's writes");
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--watch") == 0) {
            TestWatchReuse();
            std::cout << "Draw pipeline watch reuse passed\n";
            return 0;
        }
        if (argc == 2 && std::strcmp(argv[1], "--untimed") == 0) {
            TestUntimedCommits();
            std::cout << "Draw pipeline untimed commits passed\n";
            return 0;
        }
        TestReport();
        std::cout << "Draw pipeline profile passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
