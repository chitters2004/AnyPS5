#include "prx/libSceAgcDriver/Graphics/include/EffectRepeatScreen.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::EffectReadPrint;
using AgcDriver::Graphics::EffectRepeat;
using AgcDriver::Graphics::EffectRepeatScreen;
using Reads = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

int recorderA = 0;
int recorderB = 0;
int listA = 0;
int listB = 0;

void CheckPrintFollowsEveryInput() {
    const Reads reads{{0x1000, 0x1100}, {0x2000, 0x2040}};
    const auto print = EffectReadPrint(reads, &listA, 1);
    Require(print == EffectReadPrint(Reads{{0x1000, 0x1100}, {0x2000, 0x2040}}, &listA, 1), "equal reads do not print alike");
    Require(print != EffectReadPrint(Reads{{0x1000, 0x1100}, {0x2000, 0x2080}}, &listA, 1), "a changed range end prints alike");
    Require(print != EffectReadPrint(Reads{{0x1000, 0x1100}, {0x2040, 0x2040}}, &listA, 1), "a changed range begin prints alike");
    Require(print != EffectReadPrint(Reads{{0x2000, 0x2040}, {0x1000, 0x1100}}, &listA, 1), "reordered ranges print alike");
    Require(print != EffectReadPrint(Reads{{0x1000, 0x1100}}, &listA, 1), "a dropped range prints alike");
    Require(print != EffectReadPrint(reads, &listB, 1), "another read list prints alike");
    Require(print != EffectReadPrint(reads, nullptr, 1), "no read list prints like a list");
    Require(print != EffectReadPrint(reads, &listA, 2), "another read kind prints alike");
    Require(EffectReadPrint(Reads{}, nullptr, 0) != EffectReadPrint(Reads{{0, 0}}, nullptr, 0), "an empty range prints like no range");
}

void CheckRepeatsWithinOneBatch() {
    EffectRepeatScreen screen;
    Require(screen.Note(&recorderA, 1, 7) == EffectRepeat::First, "the first mark is not first");
    Require(screen.Note(&recorderA, 1, 7) == EffectRepeat::Unchanged, "the same reads in the same batch are not unchanged");
    Require(screen.Note(&recorderA, 1, 8) == EffectRepeat::Changed, "other reads in the same batch are not changed");
    Require(screen.Note(&recorderA, 1, 8) == EffectRepeat::Unchanged, "a repeat compares with the latest mark, not the first");
    Require(screen.Note(&recorderA, 2, 8) == EffectRepeat::First, "the same reads in the next batch repeat across batches");
    Require(screen.Note(&recorderB, 2, 8) == EffectRepeat::First, "the same batch serial of another recorder repeats");
    Require(screen.Note(&recorderB, 2, 8) == EffectRepeat::Unchanged, "a repeat on the other recorder is not unchanged");
}

void CheckFirstMarkAtZero() {
    EffectRepeatScreen screen;
    Require(screen.Note(nullptr, 0, 0) == EffectRepeat::First, "a first mark that matches the empty state repeats");
    Require(screen.Note(nullptr, 0, 0) == EffectRepeat::Unchanged, "a repeat after a zero mark is not unchanged");
}

}

int main() {
    try {
        CheckPrintFollowsEveryInput();
        CheckRepeatsWithinOneBatch();
        CheckFirstMarkAtZero();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "effect repeat screen tests passed\n";
    return 0;
}
