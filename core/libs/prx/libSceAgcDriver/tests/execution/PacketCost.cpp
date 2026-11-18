#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/SubmissionCapture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr auto CompletionDeadline = std::chrono::seconds(120);

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t Low(const void* p) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p)); }
std::uint32_t High(const void* p) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p) >> 32u); }

struct Mix {
    const char* name;
    std::uint32_t opcode;
    std::uint32_t firstRegister;
    std::uint32_t span;
    std::uint32_t values;
};

constexpr std::array<Mix, 4> Mixes{{
    {"SET_CONTEXT_REG x1", 0x69, 0x100, 0x40, 1},
    {"SET_CONTEXT_REG x4", 0x69, 0x100, 0x40, 4},
    {"SET_SH_REG x1", 0x76, 0x0c, 0x20, 1},
    {"SET_UCONFIG_REG x1", 0x79, 0x380, 0x10, 1},
}};

std::vector<std::uint32_t> RegisterStream(const Mix& mix, std::size_t packets) {
    std::vector<std::uint32_t> words;
    words.reserve(packets * (2 + mix.values));
    for (std::size_t index = 0; index < packets; ++index) {
        words.push_back(0xc0000000u | (mix.values << 16u) | (mix.opcode << 8u));
        words.push_back(mix.firstRegister + static_cast<std::uint32_t>(index * mix.values) % mix.span);
        for (std::uint32_t value = 0; value < mix.values; ++value) words.push_back(static_cast<std::uint32_t>(index * 2654435761u) + value);
    }
    return words;
}

void Release(std::vector<std::uint32_t>& words, const void* label, std::uint32_t value) {
    words.insert(words.end(), {0xc0064900u, 0x514u, 2u << 29u, Low(label), High(label), value, 0u, 0u});
}

void Await(const std::uint32_t* label, std::uint32_t value) {
    const auto deadline = std::chrono::steady_clock::now() + CompletionDeadline;
    while (*static_cast<const volatile std::uint32_t*>(label) != value) {
        Require(std::chrono::steady_clock::now() < deadline, "the submission's release never landed");
        std::this_thread::yield();
    }
}

double TableNanoseconds(const std::vector<std::uint32_t>& words, std::size_t packets) {
    AgcDriver::QueueState queue;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t cursor = 0; cursor < words.size();) {
        const auto count = AgcDriver::Pm4::PacketWords(words[cursor]);
        AgcDriver::Pm4::Execute(std::span(words).subspan(cursor, count), queue);
        cursor += count;
    }
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / static_cast<double>(packets);
}


bool RegisterSet(std::uint32_t header) {
    if ((header >> 30u) != 3u || AgcDriver::Pm4::Predicated(header)) return false;
    const auto opcode = (header >> 8u) & 0xffu;
    return opcode == 0x69 || opcode == 0x76 || opcode == 0x79;
}

void PutNop(std::vector<std::uint32_t>& words, std::size_t count) {
    if (count < 2) {
        words.insert(words.end(), count, 0x80000000u);
        return;
    }
    words.push_back(0xc0001000u | (static_cast<std::uint32_t>(count - 2) << 16u));
    words.insert(words.end(), count - 1, 0u);
}

struct Skeleton {
    std::vector<std::vector<std::uint32_t>> registers;
    std::vector<std::vector<std::uint32_t>> baseline;
    std::size_t submissions = 0;
    std::size_t packets = 0;
    std::size_t registerPackets = 0;
    std::size_t predicatedRegisters = 0;
    std::size_t runs = 0;
    std::size_t longest = 0;
    std::map<std::size_t, std::size_t> lengths;
};

std::size_t LengthBucket(std::size_t length) {
    if (length <= 2) return length;
    if (length <= 4) return 4;
    if (length <= 8) return 8;
    if (length <= 16) return 16;
    return 17;
}

Skeleton BuildSkeleton(const std::filesystem::path& directory, std::size_t limit) {
    Skeleton skeleton;
    for (const auto& submission : AgcDriver::SubmissionCapture::Read(directory)) {
        if (submission.commands.empty()) continue;
        if (limit != 0 && skeleton.submissions == limit) break;
        ++skeleton.submissions;
        std::vector<std::uint32_t> registers;
        std::vector<std::uint32_t> baseline;
        std::size_t run = 0;
        const auto endRun = [&] {
            if (run == 0) return;
            ++skeleton.runs;
            skeleton.longest = std::max(skeleton.longest, run);
            ++skeleton.lengths[LengthBucket(run)];
            run = 0;
        };
        const auto& commands = submission.commands;
        for (std::size_t cursor = 0; cursor < commands.size();) {
            const auto header = commands[cursor];
            if (AgcDriver::Pm4::FillerPacket(header)) {
                registers.push_back(header);
                baseline.push_back(header);
                ++cursor;
                continue;
            }
            const auto count = std::min<std::size_t>(std::max<std::size_t>(AgcDriver::Pm4::PacketWords(header), 1), commands.size() - cursor);
            ++skeleton.packets;
            if (RegisterSet(header) && count == AgcDriver::Pm4::PacketWords(header)) {
                registers.insert(registers.end(), commands.begin() + static_cast<std::ptrdiff_t>(cursor), commands.begin() + static_cast<std::ptrdiff_t>(cursor + count));
                PutNop(baseline, count);
                ++skeleton.registerPackets;
                ++run;
            } else {
                const auto opcode = (header >> 8u) & 0xffu;
                if ((header >> 30u) == 3u && (opcode == 0x69 || opcode == 0x76 || opcode == 0x79)) ++skeleton.predicatedRegisters;
                PutNop(registers, count);
                PutNop(baseline, count);
                endRun();
            }
            cursor += count;
        }
        endRun();
        skeleton.registers.push_back(std::move(registers));
        skeleton.baseline.push_back(std::move(baseline));
    }
    return skeleton;
}

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2u;
    return values.size() % 2u == 1u ? values[middle] : (values[middle - 1u] + values[middle]) / 2.0;
}

}

int main(int argc, char** argv) {
    void* block = nullptr;
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        std::size_t packets = 20000;
        std::size_t rounds = 5;
        std::size_t limit = 0;
        std::size_t repeat = 1;
        std::filesystem::path capture;
        for (int index = 1; index + 1 < argc; index += 2) {
            const std::string option = argv[index];
            if (option == "--packets") packets = std::strtoull(argv[index + 1], nullptr, 10);
            else if (option == "--rounds") rounds = std::strtoull(argv[index + 1], nullptr, 10);
            else if (option == "--capture") capture = argv[index + 1];
            else if (option == "--limit") limit = std::strtoull(argv[index + 1], nullptr, 10);
            else if (option == "--repeat") repeat = std::strtoull(argv[index + 1], nullptr, 10);
            else throw std::runtime_error("unknown option " + option + "; usage: [--packets N] [--rounds R] [--capture <APS5_DUMP_SUBMITS directory> [--limit submissions] [--repeat passes]]");
        }
        Require(packets != 0 && rounds != 0 && repeat != 0, "--packets, --rounds and --repeat must be positive");
#ifdef _WIN32
        block = VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        block = std::aligned_alloc(65536, 65536);
#endif
        Require(block != nullptr, "cannot allocate the label page");
        std::memset(block, 0, 65536);
        GuestAllocations::Mutation().Add(block, 65536, true, true, true);
        auto* label = static_cast<std::uint32_t*>(block);
        std::uint32_t serial = 0;
        const auto submitTimed = [&](std::vector<std::uint32_t> words) {
            ++serial;
            Release(words, label, serial);
            Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
            const auto start = std::chrono::steady_clock::now();
            AgcDriver::Submit(&packet, 0);
            const auto submitted = std::chrono::steady_clock::now();
            Await(label, serial);
            const auto landed = std::chrono::steady_clock::now();
            AgcDriverWaitIdle_nid_postfix();
            return std::pair{std::chrono::duration<double, std::nano>(landed - start).count(), std::chrono::duration<double, std::nano>(submitted - start).count()};
        };
        submitTimed({});
        std::vector<double> empty;
        std::vector<double> emptySubmit;
        for (std::size_t round = 0; round < rounds; ++round) {
            const auto [spent, submit] = submitTimed({});
            empty.push_back(spent);
            emptySubmit.push_back(submit);
        }
        const auto emptyNs = Median(empty);
        const auto emptySubmitNs = Median(emptySubmit);
        std::printf("[packet-cost] empty submission: %.1f us, Submit returns after %.1f us (median of %zu)\n", emptyNs / 1e3, emptySubmitNs / 1e3, rounds);
        if (!capture.empty()) {
            const auto skeleton = BuildSkeleton(capture, limit);
            Require(skeleton.registerPackets != 0, "the capture has no unpredicated register packets");
            std::printf("[packet-cost] capture %s: %zu submissions, %zu packets, %zu register packets (%zu predicated left out), %zu runs, %.2f packets per run, longest %zu\n", capture.string().c_str(), skeleton.submissions, skeleton.packets, skeleton.registerPackets, skeleton.predicatedRegisters, skeleton.runs, static_cast<double>(skeleton.registerPackets) / static_cast<double>(skeleton.runs), skeleton.longest);
            std::printf("[packet-cost] run lengths:");
            for (const auto& [bucket, count] : skeleton.lengths) std::printf(" %s%zu: %zu", bucket == 17 ? ">" : (bucket > 2 ? "<=" : ""), bucket == 17 ? std::size_t{16} : bucket, count);
            std::printf("\n");
            constexpr std::size_t ChunkWords = std::size_t{1} << 22u;
            const auto join = [&](const std::vector<std::vector<std::uint32_t>>& streams) {
                std::vector<std::vector<std::uint32_t>> chunks(1);
                for (std::size_t pass = 0; pass < repeat; ++pass) {
                    for (const auto& stream : streams) {
                        if (!chunks.back().empty() && chunks.back().size() + stream.size() + 2 > ChunkWords) chunks.emplace_back();
                        chunks.back().insert(chunks.back().end(), stream.begin(), stream.end());
                        PutNop(chunks.back(), 2);
                    }
                }
                return chunks;
            };
            const auto registerChunks = join(skeleton.registers);
            const auto baselineChunks = join(skeleton.baseline);
            const auto replay = [&](const std::vector<std::vector<std::uint32_t>>& chunks) {
                ++serial;
                auto last = chunks.back();
                Release(last, label, serial);
                const auto start = std::chrono::steady_clock::now();
                for (std::size_t index = 0; index + 1 < chunks.size(); ++index) {
                    Packet packet{const_cast<std::uint32_t*>(chunks[index].data()), static_cast<std::uint32_t>(chunks[index].size()), 0, {}};
                    AgcDriver::Submit(&packet, 0);
                }
                Packet packet{last.data(), static_cast<std::uint32_t>(last.size()), 0, {}};
                AgcDriver::Submit(&packet, 0);
                Await(label, serial);
                const auto landed = std::chrono::steady_clock::now();
                AgcDriverWaitIdle_nid_postfix();
                return std::chrono::duration<double, std::nano>(landed - start).count();
            };
            namespace Switches = AgcDriver::Graphics::RuntimeSwitches;
            using AgcDriver::Graphics::RuntimeSwitch;
            const bool initial = Switches::Set(RuntimeSwitch::RegisterRuns);
            replay(baselineChunks);
            replay(registerChunks);
            std::vector<double> off;
            std::vector<double> on;
            std::vector<double> base;
            std::uint64_t coveredOn = 0;
            std::uint64_t coveredOff = 0;
            for (std::size_t round = 0; round < rounds; ++round) {
                const bool onFirst = round % 2 == 1;
                double baseline = 0;
                double times[2] = {0, 0};
                for (int step = 0; step < 3; ++step) {
                    if (step == 1) {
                        baseline = replay(baselineChunks);
                        continue;
                    }
                    const bool runs = (step == 0) == onFirst;
                    Switches::Assign(RuntimeSwitch::RegisterRuns, runs);
                    const auto before = AgcDriverRegisterRunPackets_nid_postfix();
                    times[runs ? 1 : 0] = replay(registerChunks);
                    (runs ? coveredOn : coveredOff) += AgcDriverRegisterRunPackets_nid_postfix() - before;
                }
                base.push_back(baseline);
                const auto replayed = static_cast<double>(skeleton.registerPackets * repeat);
                off.push_back((times[0] - baseline) / replayed);
                on.push_back((times[1] - baseline) / replayed);
            }
            Switches::Assign(RuntimeSwitch::RegisterRuns, initial);
            const auto offNs = Median(off);
            const auto onNs = Median(on);
            std::vector<double> saved;
            for (std::size_t round = 0; round < rounds; ++round) saved.push_back(off[round] - on[round]);
            std::sort(saved.begin(), saved.end());
            std::printf("[packet-cost] capture replay x%zu in %zu submissions, median of %zu rounds: NOP skeleton %.1f us; per register packet above it: runs off %.1f ns, runs on %.1f ns, saved %.1f ns (paired rounds: min %.1f, quartiles %.1f..%.1f, max %.1f)\n", repeat, registerChunks.size(), rounds, Median(base) / 1e3, offNs, onNs, Median(saved), saved.front(), saved[(saved.size() - 1) / 4], saved[(3 * (saved.size() - 1)) / 4], saved.back());
            std::printf("[packet-cost] runs covered %.1f%% of register packets with the switch on, %.1f%% with it off\n", 100.0 * static_cast<double>(coveredOn) / static_cast<double>(skeleton.registerPackets * rounds * repeat), 100.0 * static_cast<double>(coveredOff) / static_cast<double>(skeleton.registerPackets * rounds * repeat));
            Require(coveredOff == 0, "runs fired with the switch off");
#ifdef _WIN32
            VirtualFree(block, 0, MEM_RELEASE);
#else
            std::free(block);
#endif
            return 0;
        }
        for (const auto& mix : Mixes) {
            const auto words = RegisterStream(mix, packets);
            std::vector<double> table;
            std::vector<double> worker;
            std::vector<double> submitted;
            for (std::size_t round = 0; round < rounds; ++round) {
                table.push_back(TableNanoseconds(words, packets));
                const auto [spent, submit] = submitTimed(words);
                worker.push_back((spent - emptyNs) / static_cast<double>(packets));
                submitted.push_back((submit - emptySubmitNs) / static_cast<double>(packets));
            }
            const auto tableNs = Median(table);
            const auto workerNs = Median(worker);
            const auto submitNs = Median(submitted);
            std::printf("[packet-cost] %-20s %zu packets: Pm4::Execute %.1f ns/packet, worker loop %.1f ns/packet (%.1fx), loop overhead %.1f ns/packet, of which Submit %.1f ns/packet\n", mix.name, packets, tableNs, workerNs, tableNs > 0 ? workerNs / tableNs : 0.0, workerNs - tableNs, submitNs);
            Require(workerNs > 0, std::string(mix.name) + ": the worker loop measured no time");
        }
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
}
