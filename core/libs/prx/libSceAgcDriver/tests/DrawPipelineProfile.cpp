#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

    std::string Text() const {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    std::string ReadReport(std::string_view prefix = "[draw] pipelined ") const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            std::ifstream file(path, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            const auto begin = text.find(prefix);
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

double Number(std::string_view text, std::string_view before, std::size_t skip = 0) {
    auto at = text.find(before);
    for (; skip != 0 && at != std::string_view::npos; --skip) at = text.find(before, at + before.size());
    if (at == std::string_view::npos) throw std::runtime_error("pipeline wait report lost: " + std::string(before));
    return std::stod(std::string(text.substr(at + before.size(), 32)));
}

void TestReport() {
    using AgcDriver::DriverDetail::DrawPipeline;
    auto& pipeline = DrawPipeline::Queue0();
    Check(DrawPipeline::Depth() >= 2, "test requires at least two pipeline slots");
    const auto reportAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(10100);
    StderrCapture capture;
    std::binary_semaphore release{0};
    std::exception_ptr failure;
    try {
        using AgcDriver::DriverDetail::DrawFinisher;
        pipeline.Enqueue([] {}, {}, 0, {}, {}, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto& finisher = DrawFinisher::Get();
        finisher.Enqueue([] { std::this_thread::sleep_for(std::chrono::milliseconds(200)); });
        finisher.Drain(DrawFinisher::WaitSite::Relocation);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        finisher.Enqueue([] {});
        finisher.Drain(DrawFinisher::WaitSite::Pipeline);
        pipeline.Enqueue([] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }, {}, 0, {}, {}, false);
        for (std::size_t i = 0; i < DrawPipeline::Depth(); ++i) pipeline.Enqueue([] {}, {}, 0, {}, {}, false);
        pipeline.Enqueue([] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }, {}, 0, {}, {}, false);
        pipeline.Drain(DrawPipeline::DrainReason::Flush);
        finisher.Enqueue([&] {
            pipeline.Enqueue([] { std::this_thread::sleep_for(std::chrono::milliseconds(250)); }, {}, 0, {}, {}, false);
            pipeline.Drain(DrawPipeline::DrainReason::Flush);
        });
        finisher.Drain(DrawFinisher::WaitSite::Lookup);
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
    Check(report.find(" 2 read ranges, 1 stores held, 3 decode drains") != std::string::npos, "pipeline read counters changed");
    Check(Count(report, " read ranges") == 1, "pipeline read-range counters were printed more than once");
    Check(Count(report, " stores held") == 1, "pipeline held-store counters were printed more than once");
    Check(Count(report, " decode drains") == 1, "pipeline decode-drain counters were printed more than once");
    Check(report.find("; drains:") != std::string::npos, "pipeline drain counters were lost");
    Check(report.find("packet drains by opcode:") != std::string::npos, "pipeline opcode counters were lost");
    const auto text = capture.Text();
    const auto idle = text.find("[committer-idle] (10 s): ");
    Check(idle != std::string::npos && text.find(" while the finisher held work (", idle) != std::string::npos && text.find(" while it was idle (", idle) != std::string::npos, "the committer idle counters were not reported");
    const auto waits = capture.ReadReport("[pipeline-waits] ");
    std::cout << waits << '\n';
    Check(Number(waits, "committer waited ") >= 250.0, "committer wait for work was not timed");
    Check(Number(waits, "committer waited ") <= 10500.0, "committer wait for work exceeds the report window");
    Check(Number(waits, "asleep ") + Number(waits, "spin ") >= 250.0, "committer spin and sleep do not add up to its wait");
    Check(Number(waits, " ms in ") >= 1.0, "committer sleeps were not counted");
    Check(Number(waits, "finisher waited ") >= 50.0, "finisher wait for jobs was not timed");
    Check(Number(waits, "by site: reloc ") == 1.0, "relocation drain was not counted at its site");
    const auto reloc = waits.substr(waits.find("by site: reloc 1 (") + 18);
    Check(std::stod(reloc) >= 150.0, "relocation drain wait was not timed");
    Check(Number(waits, ", lookup ") == 1.0, "a drain was counted at the wrong site");
    Check(Number(waits, ", pipeline ") == 1.0, "a drain was counted at the wrong site");
    Check(Number(waits, "worker waited ") >= 100.0, "worker wait for committer room was not timed");
    Check(Number(waits, "for committer room in ", 1) == 1.0, "worker waits for committer room were not counted");
    Check(Number(waits, "for committer room in ") == 0.0, "a worker wait for committer room was counted on the finisher");
    Check(Number(waits, "on pipeline drains in ") == 1.0, "finisher pipeline drains were not counted on the finisher");
    Check(Number(waits, "on pipeline drains in ", 1) == 1.0, "worker pipeline drains were not counted on the worker");
    Check(Number(waits, "room in 0 waits, ") >= 200.0, "finisher pipeline drain was not timed on the finisher");
    Check(Number(waits, "room in 1 waits, ") >= 50.0 && Number(waits, "room in 1 waits, ") < 200.0, "worker pipeline drain was not timed on the worker");
    Check(Number(waits, ", other ") == 0.0, "a drain was counted at the wrong site");
    Check(waits.find("ring preparers, ") != std::string::npos ? Number(waits, "ring preparers, ") >= 1.0 : Number(waits, " ms in ", 1) >= 1.0, "finisher sleeps were not counted");
    Check(Count(waits, "[pipeline-waits]") == 1, "pipeline wait counters were printed more than once");
}

}

int main() {
    try {
        TestReport();
        std::cout << "Draw pipeline profile passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
