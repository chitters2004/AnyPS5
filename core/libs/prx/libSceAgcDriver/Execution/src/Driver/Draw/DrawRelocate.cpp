#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "ControlFlow/GraphBuilder.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::size_t MaxAliases = 16384;
constexpr std::size_t MaxContentWords = 64;

std::uint64_t AlignedWord(std::uint32_t word) {
    return word & ~std::uint32_t {3};
}

}

bool Driver::drawRelocationEnabled() {
    return !Graphics::RuntimeSwitches::Set(Graphics::RuntimeSwitch::NoDrawRelocation);
}

std::uint64_t Driver::relocatedContentKey(const RelocatableUserWord& word, std::uint32_t value, bool& readable) {
    std::uint64_t hash = 0x9e3779b97f4a7c15ull;
    const auto mix = [&](std::uint64_t mixed) {
        hash ^= mixed;
        hash *= 0x100000001b3ull;
    };
    if (word.value) return hash;
    mix(value & 3u);
    if (value == 0) {
        readable = false;
        return hash;
    }
    std::array<std::uint32_t, MaxContentWords> content;
    for (const auto& [offset, words] : word.reads) {
        mix(static_cast<std::uint64_t>(offset));
        mix(words);
        for (std::size_t done = 0; done < words; done += MaxContentWords) {
            const auto count = std::min<std::size_t>(words - done, MaxContentWords);
            const auto address = (static_cast<std::uint64_t>(word.high) << 32u) + AlignedWord(value) + static_cast<std::uint64_t>(offset) + done * sizeof(std::uint32_t);
            if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(content).first(count))) != GuestMemory::Compare::Equal) {
                readable = false;
                return hash;
            }
            for (std::size_t i = 0; i < count; ++i) mix(content[i]);
        }
    }
    return hash;
}

void Driver::tagUserValueWords(DispatchVariant& variant, const DrawProgram& program) {
    if (variant.compiled == nullptr) return;
    const auto& compiled = *variant.compiled;
    const auto& forwarded = compiled.shaderDataUserWords;
    const auto users = std::min<std::size_t>(program.userData.size(), 64);
    const auto bound = [&](std::uint32_t value) {
        std::size_t count = 0;
        for (const auto& binding : compiled.bindings) count += static_cast<std::size_t>(std::count(binding.guestDescriptor.begin(), binding.guestDescriptor.end(), value));
        return count;
    };
    const auto elsewhere = [&](std::uint32_t index, std::uint32_t value) {
        for (std::size_t slot = 0; (slot + 1) * sizeof(std::uint32_t) <= compiled.pushConstants.size(); ++slot) {
            std::uint32_t word = 0;
            std::memcpy(&word, compiled.pushConstants.data() + slot * sizeof(std::uint32_t), sizeof(word));
            if (word == value && (slot >= forwarded.size() || forwarded[slot] != index)) return true;
        }
        if (std::any_of(compiled.specialization.begin(), compiled.specialization.end(), [&](const auto& constant) { return constant.value == value; })) return true;
        return std::find(variant.words.begin(), variant.words.end(), value) != variant.words.end();
    };
    const auto readThrough = [&](const std::uint32_t* descriptor) {
        const auto base = static_cast<std::uint64_t>(descriptor[0]) | static_cast<std::uint64_t>(descriptor[1] & 0xffffu) << 32u;
        const auto stride = (descriptor[1] >> 16u) & 0x3fffu;
        const auto bytes = std::max<std::uint64_t>(stride != 0 ? static_cast<std::uint64_t>(stride) * descriptor[2] : descriptor[2], 16u);
        return std::any_of(variant.runs.begin(), variant.runs.end(), [&](const auto& run) { return run.first < base + bytes && run.second > base; });
    };
    const auto taken = [&](std::uint32_t index) { return ((variant.relocatableWords | variant.valueWords | variant.calleeWords) >> index & 1u) != 0; };
    if (ShaderRecompiler::GcnGuestShaders() && program.binary.stage == ShaderRecompiler::ShaderStage::Vertex) {
        for (const auto instruction : program.binary.code) {
            if ((instruction & 0xff80ff00u) != 0xbe802100u) continue;
            const auto target = instruction & 0xffu;
            if (target < program.firstUserSgpr) continue;
            const auto index = program.hiddenUserWords + target - program.firstUserSgpr;
            if (index + 1 >= users || taken(index) || taken(index + 1)) continue;
            const auto low = program.userData[index];
            const auto address = static_cast<std::uint64_t>(low) | static_cast<std::uint64_t>(program.userData[index + 1]) << 32u;
            if (address == 0 || address % 4u != 0 || bound(low) != 0 || elsewhere(index, low)) continue;
            std::array<std::uint32_t, 1024> code;
            if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(code))) != GuestMemory::Compare::Equal) continue;
            std::optional<std::uint32_t> length;
            try {
                length = ShaderRecompiler::UserDataCalleeLength(code, (instruction >> 16u) & 0x7fu);
            } catch (const std::exception&) {
                continue;
            }
            if (!length || *length == 0) continue;
            variant.calleeWords |= std::uint64_t {1} << index;
            variant.calleeReads.emplace_back(index, *length);
        }
    }
    for (std::uint32_t index = program.hiddenUserWords; index + 2 < users; ++index) {
        const auto base = program.userData[index];
        if (base == 0 || taken(index) || bound(base) != 1 || elsewhere(index, base)) continue;
        std::uint32_t binding = 0, word = 0;
        for (std::size_t b = 0; b < compiled.bindings.size(); ++b) {
            const auto& descriptor = compiled.bindings[b].guestDescriptor;
            const auto found = std::find(descriptor.begin(), descriptor.end(), base);
            if (found == descriptor.end()) continue;
            binding = static_cast<std::uint32_t>(b);
            word = static_cast<std::uint32_t>(found - descriptor.begin());
        }
        const auto& target = compiled.bindings[binding];
        const auto element = word / 4u;
        const bool readOnly = target.role == ShaderRecompiler::DescriptorRole::GuestBuffers && target.guestDescriptor.size() == static_cast<std::size_t>(target.count) * 4u && word % 4u == 0 && element < target.bufferWritten.size() && !target.bufferWritten[element] && (element >= target.bufferAtomic.size() || !target.bufferAtomic[element]);
        if (!readOnly || target.guestDescriptor[word + 1] != program.userData[index + 1] || target.guestDescriptor[word + 2] != program.userData[index + 2] || readThrough(target.guestDescriptor.data() + word)) continue;
        variant.valueWords |= std::uint64_t {1} << index;
        variant.valueSlots.push_back({index, binding, word});
        const auto records = program.userData[index + 2];
        if (records != 0 && !taken(index + 2) && bound(records) == 1 && !elsewhere(index + 2, records)) {
            variant.valueWords |= std::uint64_t {1} << (index + 2);
            variant.valueSlots.push_back({index + 2, binding, word + 2});
        }
    }
}

void Driver::tagVariantRuns(DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions, const ShaderRecompiler::SrtUserDataUse& use, bool eligible, std::uint32_t hiddenWords, std::span<const std::shared_ptr<const ShaderRecompiler::SrtGuestWords>> supplied) {
    variant.userWordsWalked = use.asBase | use.asValue;
    std::uint64_t relocatable = eligible ? use.asBase & ~use.asValue : 0;
    if (hiddenWords != 0) relocatable &= hiddenWords >= 64 ? 0 : ~((std::uint64_t {1} << hiddenWords) - 1);
    std::vector<std::pair<std::uint64_t, std::int32_t>> reads;
    const auto covered = [&](std::uint64_t address) {
        return std::any_of(regions.begin(), regions.end(), [&](const ShaderRecompiler::MemoryRegion& region) { return address >= region.guestAddress && address - region.guestAddress + sizeof(std::uint32_t) <= region.bytes.size(); });
    };
    if (relocatable != 0) {
        reads = use.reads;
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        const auto drop = [&](std::int32_t origin) {
            if (origin >= 0) relocatable &= ~(std::uint64_t {1} << origin);
        };
        for (std::size_t i = 0; i < reads.size(); ++i) {
            if (i + 1 < reads.size() && reads[i].first == reads[i + 1].first) {
                drop(reads[i].second);
                drop(reads[i + 1].second);
            }
            if (reads[i].second >= 0 && ((reads[i].first & 3u) != 0 || !covered(reads[i].first))) drop(reads[i].second);
        }
    }
    const auto origin = [&](std::uint64_t address) -> std::int8_t {
        const auto found = std::lower_bound(reads.begin(), reads.end(), std::make_pair(address, std::numeric_limits<std::int32_t>::min()));
        if (found == reads.end() || found->first != address || found->second < 0) return -1;
        return (relocatable >> found->second & 1u) != 0 ? static_cast<std::int8_t>(found->second) : std::int8_t {-1};
    };
    variant.relocatableWords = relocatable;
    for (std::size_t index = 0; index < regions.size(); ++index) {
        const auto& region = regions[index];
        const auto count = region.bytes.size() / sizeof(std::uint32_t);
        const auto offset = variant.words.size();
        variant.words.resize(offset + count);
        std::memcpy(variant.words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
        if (relocatable == 0) {
            AppendSharedWords(variant.shared, variant.runs.size(), offset, region.bytes, supplied);
            variant.runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
            continue;
        }
        std::size_t start = 0;
        auto current = count != 0 ? origin(region.guestAddress) : std::int8_t {-1};
        for (std::size_t word = 1; word <= count; ++word) {
            const auto next = word < count ? origin(region.guestAddress + word * sizeof(std::uint32_t)) : std::int8_t {-2};
            if (next == current) continue;
            AppendSharedWords(variant.shared, variant.runs.size(), offset + start, region.bytes.subspan(start * sizeof(std::uint32_t), (word - start) * sizeof(std::uint32_t)), supplied);
            variant.runs.emplace_back(region.guestAddress + start * sizeof(std::uint32_t), region.guestAddress + word * sizeof(std::uint32_t));
            variant.runBases.push_back(current);
            start = word;
            current = next;
        }
    }
}

namespace {

template <typename TMap>
std::size_t dropEveryOther(TMap& map) {
    std::size_t dropped = 0;
    bool drop = false;
    for (auto it = map.begin(); it != map.end();) {
        drop = !drop;
        if (!drop) {
            ++it;
            continue;
        }
        it = map.erase(it);
        ++dropped;
    }
    return dropped;
}

}

namespace {

class RelocationEdit {
public:
    explicit RelocationEdit(const DrawRelocation& base) : view(&base) {}
    const DrawRelocation& Shape() const { return *view; }
    DrawRelocation& Edit() {
        if (copy == nullptr) {
            copy = std::make_shared<DrawRelocation>(*view);
            view = copy.get();
        }
        return *copy;
    }
    std::shared_ptr<DrawRelocation> Take() { return std::move(copy); }

private:
    const DrawRelocation* view;
    std::shared_ptr<DrawRelocation> copy;
};

const DrawRelocation& EmptyRelocation() {
    static const DrawRelocation empty;
    return empty;
}

std::ptrdiff_t HeldWord(const DrawRelocation& shape, std::uint32_t reg) {
    const auto held = std::find_if(shape.words.begin(), shape.words.end(), [&](const RelocatableUserWord& word) { return word.reg == reg; });
    return held == shape.words.end() ? -1 : held - shape.words.begin();
}

bool Banned(const DrawRelocation& shape, std::uint32_t reg) {
    return std::find(shape.banned.begin(), shape.banned.end(), reg) != shape.banned.end();
}

}

std::shared_ptr<const DrawRelocation> Driver::MergeDrawRelocation(const std::shared_ptr<const DrawRelocation>& shape, const DrawEntry& entry) {
    const auto& programs = entry.decode->programs;
    RelocationEdit edit(shape != nullptr ? *shape : EmptyRelocation());
    const auto ban = [&](std::uint32_t reg) {
        if (const auto held = HeldWord(edit.Shape(), reg); held >= 0) {
            auto& words = edit.Edit().words;
            words.erase(words.begin() + held);
        }
        if (!Banned(edit.Shape(), reg)) edit.Edit().banned.push_back(reg);
    };
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (entry.stages[i].empty()) continue;
        const auto& variant = *entry.stages[i].front();
        const auto& program = programs[i];
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size() && index < 64; ++index) {
            const auto reg = program.userDataBase + index - program.hiddenUserWords;
            const bool relocatable = (variant.relocatableWords >> index & 1u) != 0;
            const bool value = !relocatable && (variant.valueWords >> index & 1u) != 0;
            if (!relocatable && !value && (variant.calleeWords >> index & 1u) == 0) {
                ban(reg);
                continue;
            }
            if (Banned(edit.Shape(), reg)) continue;
            RelocatableUserWord word{reg, {}, value};
            for (const auto& [callee, length] : variant.calleeReads) {
                if (callee != index) continue;
                word.reads.emplace_back(0, length);
                word.high = program.userData[index + 1];
            }
            const auto base = AlignedWord(program.userData[index]);
            for (std::size_t run = 0; run < variant.runs.size() && run < variant.runBases.size(); ++run) {
                if (variant.runBases[run] != static_cast<std::int8_t>(index)) continue;
                word.reads.emplace_back(static_cast<std::int64_t>(variant.runs[run].first - base), static_cast<std::uint32_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t)));
            }
            std::sort(word.reads.begin(), word.reads.end());
            word.reads.erase(std::unique(word.reads.begin(), word.reads.end()), word.reads.end());
            const auto held = HeldWord(edit.Shape(), reg);
            if (held < 0) {
                edit.Edit().words.push_back(std::move(word));
                continue;
            }
            const auto& kept = edit.Shape().words[static_cast<std::size_t>(held)];
            if (kept.value != word.value || kept.high != word.high) {
                ban(reg);
                continue;
            }
            if (std::includes(kept.reads.begin(), kept.reads.end(), word.reads.begin(), word.reads.end())) continue;
            std::vector<std::pair<std::int64_t, std::uint32_t>> reads;
            std::set_union(kept.reads.begin(), kept.reads.end(), word.reads.begin(), word.reads.end(), std::back_inserter(reads));
            edit.Edit().words[static_cast<std::size_t>(held)].reads = std::move(reads);
        }
    }
    auto changed = edit.Take();
    if (changed == nullptr) return shape != nullptr ? shape : std::make_shared<const DrawRelocation>();
    std::sort(changed->words.begin(), changed->words.end(), [](const RelocatableUserWord& a, const RelocatableUserWord& b) { return a.reg < b.reg; });
    std::sort(changed->banned.begin(), changed->banned.end());
    return changed;
}

void Driver::learnDrawRelocation(std::uint64_t key, const DrawRelocationSource& source) {
    if (source.identity == 0) return;
    const auto inserted = drawCache.find(key);
    if (inserted == drawCache.end() || inserted->second->decode == nullptr) return;
    const auto& entry = *inserted->second;
    if (entry.stages.size() != entry.decode->programs.size()) return;
    const auto known = drawRelocations.find(source.identity);
    const auto current = known != drawRelocations.end() ? known->second : nullptr;
    auto merged = MergeDrawRelocation(current, entry);
    if (merged == current) {
        ++drawRelocationCounters.learnedUnchanged;
    } else {
        ++drawRelocationCounters.learnedChanged;
        drawRelocations.insert_or_assign(source.identity, std::move(merged));
    }
    if (source.alias == 0) return;
    if (drawAliases.size() >= MaxAliases) drawRelocationCounters.evicted += dropEveryOther(drawAliases);
    drawAliases[source.alias] = key;
}

void Driver::relocateDrawEntry(const QueueState& queue, std::uint64_t alias, std::shared_ptr<DrawEntry>& entry, std::shared_ptr<const DrawDecode>& decode, std::vector<DrawUserPatch>& patches) {
    auto& counters = drawRelocationCounters;
    ++counters.lookups;
    const auto found = drawAliases.find(alias);
    if (found == drawAliases.end()) return;
    const auto cached = drawCache.find(found->second);
    if (cached == drawCache.end()) {
        drawAliases.erase(found);
        return;
    }
    if (cached->second->decode == nullptr) return;
    const auto donor = cached->second;
    const auto& programs = donor->decode->programs;
    if (donor->stages.size() != programs.size()) return;
    struct Move {
        std::size_t program;
        std::uint32_t index, from, to;
    };
    std::vector<Move> moves;
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size(); ++index) {
            const auto current = readUserData(queue.shader, program.userDataBase + index - program.hiddenUserWords);
            if (current != program.userData[index]) moves.push_back({i, index, program.userData[index], current});
        }
    }
    if (moves.empty()) return;
    ++counters.donors;
    auto relocated = std::make_shared<DrawEntry>();
    relocated->decode = donor->decode;
    relocated->donor = donor->donor != nullptr ? donor->donor : donor;
    relocated->stages.resize(donor->stages.size());
    for (std::size_t i = 0; i < donor->stages.size(); ++i) {
        const bool moved = std::any_of(moves.begin(), moves.end(), [&](const Move& move) { return move.program == i; });
        if (!moved) {
            relocated->stages[i] = donor->stages[i];
            continue;
        }
        for (const auto& variant : donor->stages[i]) {
            if (variant == nullptr || variant->compiled == nullptr) continue;
            const bool allowed = std::all_of(moves.begin(), moves.end(), [&](const Move& move) { return move.program != i || (move.index < 64 && move.from != 0 && move.to != 0 && ((((variant->relocatableWords | variant->calleeWords) >> move.index & 1u) != 0 && ((move.from ^ move.to) & 3u) == 0) || (variant->valueWords >> move.index & 1u) != 0)); });
            if (!allowed) {
                ++counters.refused;
                continue;
            }
            auto copy = std::make_shared<DispatchVariant>();
            copy->runs = variant->runs;
            copy->runBases = variant->runBases;
            copy->relocatableWords = variant->relocatableWords;
            copy->userWordsWalked = variant->userWordsWalked;
            copy->valueWords = variant->valueWords;
            copy->valueSlots = variant->valueSlots;
            copy->calleeWords = variant->calleeWords;
            copy->calleeReads = variant->calleeReads;
            copy->donor = variant->donor != nullptr ? variant->donor : variant;
            copy->words = variant->words;
            copy->forgetSerial = variant->forgetSerial;
            copy->compiled = variant->compiled;
            copy->meshIndexDescriptor = variant->meshIndexDescriptor;
            copy->shader = variant->shader;
            copy->dataPositions = variant->dataPositions;
            copy->dataSlots = variant->dataSlots;
            copy->flatBinding = variant->flatBinding;
            copy->pushOffset = variant->pushOffset;
            copy->vertexInfo = variant->vertexInfo;
            for (const auto& move : moves) {
                if (move.program != i) continue;
                const auto delta = AlignedWord(move.to) - AlignedWord(move.from);
                for (std::size_t run = 0; run < copy->runs.size() && run < copy->runBases.size(); ++run) {
                    if (copy->runBases[run] != static_cast<std::int8_t>(move.index)) continue;
                    copy->runs[run].first += delta;
                    copy->runs[run].second += delta;
                }
                const auto& forwarded = variant->compiled->shaderDataUserWords;
                for (std::uint32_t slot = 0; slot < forwarded.size(); ++slot) {
                    if (forwarded[slot] == move.index) copy->shaderDataPatches.emplace_back(slot, move.to);
                }
                for (const auto& [index, binding, word] : variant->valueSlots) {
                    if (index == move.index) copy->descriptorPatches.push_back({binding, word, move.to});
                }
            }
            relocated->stages[i].push_back(std::move(copy));
        }
    }
    patches.clear();
    for (const auto& move : moves) patches.push_back({move.program, move.index, move.to});
    entry = std::move(relocated);
    decode = donor->decode;
}

void Driver::applyShaderDataPatches(DispatchVariant& variant) {
    if (variant.shaderDataPatches.empty() && variant.descriptorPatches.empty()) return;
    auto rebased = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
    for (const auto& [slot, value] : variant.shaderDataPatches) patchForwardedWord(*rebased, slot, value);
    for (const auto& [binding, word, value] : variant.descriptorPatches) {
        if (binding < rebased->bindings.size() && word < rebased->bindings[binding].guestDescriptor.size()) rebased->bindings[binding].guestDescriptor[word] = value;
    }
    variant.compiled = std::move(rebased);
    variant.shaderDataPatches.clear();
    variant.descriptorPatches.clear();
}

void Driver::patchForwardedWord(ShaderRecompiler::RecompileResult& result, std::uint32_t slot, std::uint32_t value) {
    const auto at = static_cast<std::size_t>(slot) * sizeof(std::uint32_t);
    if (at + sizeof(std::uint32_t) <= result.pushConstants.size()) std::memcpy(result.pushConstants.data() + at, &value, sizeof(value));
    for (auto& binding : result.bindings) {
        if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData && slot < binding.guestDescriptor.size()) binding.guestDescriptor[slot] = value;
    }
}

void Driver::reportDrawRelocation() {
    auto& counters = drawRelocationCounters;
    AgcDriver::ProfilePrint_nid_no_patch("[draw-relocate] %llu alias lookups: %llu donors with moved user words, %llu refusals, %llu hits through a relocated entry; %zu shader sets learned (%llu inserts changed a learned set, %llu left it unchanged), %zu aliases held (%llu dropped at the limit)\n", static_cast<unsigned long long>(counters.lookups), static_cast<unsigned long long>(counters.donors), static_cast<unsigned long long>(counters.refused), static_cast<unsigned long long>(counters.hits), drawRelocations.size(), static_cast<unsigned long long>(counters.learnedChanged), static_cast<unsigned long long>(counters.learnedUnchanged), drawAliases.size(), static_cast<unsigned long long>(counters.evicted));
    counters = {};
}

}
