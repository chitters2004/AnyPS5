#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_NOTTESTED_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_NOTTESTED_HPP

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

inline std::atomic<unsigned>& NotTestedChecks() {
    static std::atomic<unsigned> checks{0};
    return checks;
}

inline bool TestedRequired() {
    static const bool required = [] {
        const char* value = std::getenv("APS5_REQUIRE_TESTED");
        return value != nullptr && value[0] != '\0' && std::string_view(value) != "0";
    }();
    return required;
}

inline void ReportNotTested() {
    std::cout.flush();
    std::printf("[not-tested] total: %u\n", NotTestedChecks().load(std::memory_order_relaxed));
    std::fflush(stdout);
}

inline const bool NotTestedReportRegistered = std::atexit(ReportNotTested) == 0;

template <typename... Parts>
void NotTested(const Parts&... parts) {
    std::ostringstream line;
    (line << ... << parts);
    const auto text = line.str();
    std::cout << text << '\n';
    NotTestedChecks().fetch_add(1, std::memory_order_relaxed);
    if (!TestedRequired()) return;
    std::cout.flush();
    std::fprintf(stderr, "APS5_REQUIRE_TESTED=1 fails on a check that was not tested: %s\n", text.c_str());
    std::fflush(nullptr);
    std::_Exit(1);
}

#endif
