#include "prx/libSceAgcDriver/Execution/include/CriticalPath.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace CP = AgcDriver::CriticalPath;

void check(bool condition, const std::string& message) {
    if (condition) return;
    std::fprintf(stderr, "critical path test failed: %s\n", message.c_str());
    std::exit(1);
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void testRoles() {
    check(CP::RoleOf("draw committer") == CP::Role::Committer, "the committer role is wrong");
    check(CP::RoleOf("draw finisher") == CP::Role::Finisher, "the finisher role is wrong");
    check(CP::RoleOf("queue worker 0x0") == CP::Role::Worker, "a queue worker role is wrong");
    check(CP::RoleOf("presenter") == CP::Role::Presenter, "the presenter role is wrong");
    check(CP::RoleOf("guest") == CP::Role::Other, "an unknown thread is not other");
}

void testFormat() {
    CP::FrameRow row;
    row.present = 7;
    row.beginNs = 100;
    row.endNs = 33100;
    row.ringDepth = 64;
    CP::ThreadRow thread;
    thread.name = "draw \"committer\"";
    thread.role = CP::Role::Committer;
    thread.cpuNs = 30000;
    thread.values[static_cast<std::size_t>(CP::Measure::CommitBusy)] = 29000;
    thread.values[static_cast<std::size_t>(CP::Measure::Commits)] = 3;
    thread.waitedBehind[static_cast<std::size_t>(CP::Lock::Gpu)][static_cast<std::size_t>(CP::Role::Worker)] = 500;
    row.threads.push_back(thread);
    const auto text = CP::FormatRow(row);
    check(text == "{\"present\":7,\"begin_ns\":100,\"end_ns\":33100,\"ring_depth\":64,\"threads\":[{\"name\":\"draw \\\"committer\\\"\",\"role\":\"committer\",\"cpu_ns\":30000,\"commit_busy_ns\":29000,\"commits\":3,\"gpu_wait_behind_ns\":{\"worker\":500}}]}", "the row format is wrong: " + text);
}

std::vector<std::string> lines(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::vector<std::string> result;
    for (std::string line; std::getline(file, line);) result.push_back(line);
    return result;
}

void testWindow(const std::filesystem::path& directory) {
    check(!CP::Active(), "the recorder is active before its window opens");
    CP::BindThread("draw committer");
    CP::Add(CP::Measure::CommitBusy, 1);
    CP::OpenWindow(10);
    check(CP::Active(), "the recorder is not active inside its window");
    CP::Add(CP::Measure::CommitBusy, 2500);
    CP::Add(CP::Measure::Commits, 2);
    CP::Max(CP::Measure::OccupancyMax, 5);
    CP::Max(CP::Measure::OccupancyMax, 3);
    CP::Acquired(CP::Lock::Gpu);
    CP::Released(CP::Lock::Gpu);
    std::thread other([] {
        CP::Waited(CP::Lock::Gpu, CP::Owner(CP::Lock::Gpu), 700);
        { const CP::Span collecting(CP::Measure::Collect); }
        CP::Add(CP::Measure::Collects, 1);
    });
    other.join();
    CP::Present(11);
    CP::Add(CP::Measure::RoomWait, 900);
    CP::CloseWindow(12, directory, "critical.jsonl");
    check(!CP::Active(), "the recorder is active after its window closes");
    CP::Add(CP::Measure::RoomWait, 1);
    const auto rows = lines(directory / "critical.jsonl");
    check(rows.size() == 3, "the file has " + std::to_string(rows.size()) + " lines, not a header and two frames");
    check(contains(rows[0], "\"format\":\"aps5-critical/1\"") && contains(rows[0], "\"rows\":2"), "the header is wrong: " + rows[0]);
    check(contains(rows[1], "\"present\":11") && contains(rows[1], "\"name\":\"draw committer\",\"role\":\"committer\",\"cpu_ns\":"), "the first frame misses the committer: " + rows[1]);
    check(contains(rows[1], "\"commit_busy_ns\":2500,\"commits\":2,\"occupancy_max\":5") && contains(rows[1], "\"gpu_acquires\":1"), "the first frame misses the committer's measures: " + rows[1]);
    check(contains(rows[1], "\"name\":\"\",\"role\":\"other\"") && contains(rows[1], "\"gpu_wait_ns\":700") && contains(rows[1], "\"gpu_wait_behind_ns\":{\"committer\":700}") && contains(rows[1], "\"collects\":1"), "the first frame misses the unbound thread's lock wait and collect: " + rows[1]);
    check(contains(rows[2], "\"present\":12") && contains(rows[2], "\"room_wait_ns\":900") && !contains(rows[2], "commit_busy"), "the second frame is wrong: " + rows[2]);
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_TRACE_CRITICAL", "1");
    const auto process = static_cast<unsigned long>(_getpid());
#else
    setenv("APS5_TRACE_CRITICAL", "1", 1);
    const auto process = static_cast<unsigned long>(getpid());
#endif
    const auto directory = std::filesystem::temp_directory_path() / ("aps5-critical-path-test-" + std::to_string(process));
    std::filesystem::remove_all(directory);
    testRoles();
    testFormat();
    testWindow(directory);
    std::filesystem::remove_all(directory);
    std::puts("critical path tests passed");
    return 0;
}
