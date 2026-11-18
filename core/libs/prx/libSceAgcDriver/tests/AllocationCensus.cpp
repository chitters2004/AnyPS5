#include "prx/libSceAgcDriver/Execution/include/AllocationCensus.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

using AgcDriver::AllocationCensus::FormatWindow;
using AgcDriver::AllocationCensus::PreferredImageBase;
using AgcDriver::AllocationCensus::Site;
using AgcDriver::AllocationCensus::Source;
using AgcDriver::AllocationCensus::Table;

int failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void WindowsCountSitesSinceTheLastReport() {
    auto table = std::make_unique<Table<64, 3>>();
    std::array<Site, 3> top{};
    for (int i = 0; i < 5; ++i) table->Note(Source::New, 0x1000, 24);
    for (int i = 0; i < 2; ++i) table->Note(Source::Malloc, 0x1000, 100);
    for (int i = 0; i < 9; ++i) table->Note(Source::New, 0x2000, 8);
    table->Note(Source::Realloc, 0x3000, 4096);
    table->Note(Source::Calloc, 0x4000, 32);
    for (int i = 0; i < 4; ++i) table->NoteDraw();
    auto window = table->Take(top);
    Check(window.draws == 4, "a window counts the draws since the last report");
    Check(window.counts[0] == 14 && window.counts[1] == 2 && window.counts[2] == 1 && window.counts[3] == 1, "a window counts each source");
    Check(window.bytes[0] == 5 * 24 + 9 * 8 && window.bytes[1] == 200 && window.bytes[3] == 4096, "a window sums each source's bytes");
    Check(window.listed == 3, "a window lists at most its top sites");
    Check(top[0].caller == 0x2000 && top[0].source == Source::New && top[0].count == 9 && top[0].bytes == 72, "the busiest site leads");
    Check(top[1].caller == 0x1000 && top[1].source == Source::New && top[1].count == 5, "sites are ordered by count");
    Check(top[2].caller == 0x1000 && top[2].source == Source::Malloc && top[2].count == 2, "one caller keeps a site per source");
    Check(window.untracked == 0, "a table with room tracks every site");
    table->Note(Source::Calloc, 0x4000, 32);
    table->Note(Source::Calloc, 0x4000, 32);
    table->NoteDraw();
    window = table->Take(top);
    Check(window.draws == 1 && window.counts[0] == 0 && window.counts[2] == 2, "the next window starts from the last report");
    Check(window.listed == 1 && top[0].caller == 0x4000 && top[0].count == 2, "a window lists only sites that allocated in it");
}

void FullTablesCountUntrackedSites() {
    auto table = std::make_unique<Table<32, 4>>();
    std::array<Site, 4> top{};
    for (std::uintptr_t caller = 1; caller <= 33; ++caller) table->Note(Source::New, caller * 16, 8);
    const auto window = table->Take(top);
    Check(window.counts[0] == 33, "a full table still counts every allocation");
    Check(window.untracked == 1, "an allocation past a full table counts as untracked");
}

void LinesNameSitesAtThePreferredBase() {
    auto table = std::make_unique<Table<64, 2>>();
    std::array<Site, 2> top{};
    for (int i = 0; i < 6; ++i) table->Note(Source::New, 0x7ff600001234, 40);
    table->Note(Source::Malloc, 0x7ff600005678, 10);
    for (int i = 0; i < 2; ++i) table->NoteDraw();
    const auto window = table->Take(top);
    char line[2048];
    const int n = FormatWindow(line, sizeof(line), 10.0, window, top.data(), 0x7ff600000000, 0x340000000);
    const std::string text(line);
    Check(n > 0 && static_cast<std::size_t>(n) == text.size(), "a census line reports its length");
    Check(text.find("[alloc-census] 10 s: 2 draws, 7 allocations (3.50/draw):") == 0, "a census line leads with draws and allocations per draw");
    Check(text.find(" new 6 (3.00/draw, 240 B)") != std::string::npos && text.find(" malloc 1 (0.50/draw, 10 B)") != std::string::npos, "a census line splits allocations by source");
    Check(text.find(" 0x340001234 new 6 3.000 40;") != std::string::npos, "a site is named at the module's preferred base with its count, per draw and average bytes");
    Check(text.find(" 0x340005678 malloc 1 0.500 10;") != std::string::npos, "every listed site is printed");
    char small[48];
    const int cut = FormatWindow(small, sizeof(small), 10.0, window, top.data(), 0x7ff600000000, 0x340000000);
    Check(cut >= static_cast<int>(sizeof(small)) && std::strlen(small) == sizeof(small) - 1, "a census line stops at its buffer");
}

void PreferredBasesComeFromTheFileHeader() {
    std::array<unsigned char, 512> pe{};
    pe[0] = 'M';
    pe[1] = 'Z';
    pe[0x3c] = 0x80;
    pe[0x80] = 'P';
    pe[0x81] = 'E';
    pe[0x98] = 0x0b;
    pe[0x99] = 0x02;
    const std::uint64_t base = 0x298e10000ull;
    for (std::size_t i = 0; i < 8; ++i) pe[0x98 + 0x18 + i] = static_cast<unsigned char>(base >> (8u * i));
    Check(PreferredImageBase(pe.data(), pe.size()) == base, "a PE32+ header gives its ImageBase");
    pe[0x98] = 0x0b;
    pe[0x99] = 0x01;
    for (std::size_t i = 0; i < 8; ++i) pe[0x98 + 0x18 + i] = 0;
    for (std::size_t i = 0; i < 4; ++i) pe[0x98 + 0x1c + i] = static_cast<unsigned char>(0x00400000u >> (8u * i));
    Check(PreferredImageBase(pe.data(), pe.size()) == 0x00400000u, "a PE32 header gives its ImageBase");
    Check(PreferredImageBase(pe.data(), 0x90) == 0, "a header cut before its optional header gives no base");
    pe[0x80] = 'X';
    Check(PreferredImageBase(pe.data(), pe.size()) == 0, "a file without the PE signature gives no base");
}

}

int main() {
    WindowsCountSitesSinceTheLastReport();
    FullTablesCountUntrackedSites();
    LinesNameSitesAtThePreferredBase();
    PreferredBasesComeFromTheFileHeader();
    if (failures != 0) {
        std::fprintf(stderr, "%d allocation census checks failed\n", failures);
        return 1;
    }
    std::printf("Allocation census tests passed\n");
    return 0;
}
