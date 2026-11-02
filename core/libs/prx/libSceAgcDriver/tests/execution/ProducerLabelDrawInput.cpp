#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <optional>
#include <initializer_list>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint32_t Width = 192;
constexpr std::uint32_t Height = 128;
constexpr std::uint32_t DistanceVector = 0x00400000u;
constexpr std::uint32_t SetContextReg = 0x69u;
constexpr std::uint32_t SetShReg = 0x76u;
constexpr std::uint32_t SetUconfigReg = 0x79u;
constexpr std::uint32_t DrawIndexOffset2Header = 0xc0033500u;
constexpr std::uint32_t TriangleList = 4u;
constexpr std::uint32_t QuadList = 19u;
constexpr std::uint32_t Index16 = 0u;
constexpr std::uint32_t Index32 = 1u;
constexpr std::uint32_t Compute = 0x20u;
constexpr std::uint32_t Graphics = 0u;
constexpr std::uint32_t GroupCounts = 0x8041u;
constexpr std::uint32_t SpinIterations = 1u << 20u;
constexpr std::uint32_t SpinDispatches = 16;
constexpr auto ProducerStall = std::chrono::milliseconds(200);
constexpr std::uint32_t GateOpen = 7;
constexpr std::uint32_t LabelValue = 1;

constexpr std::size_t Block = 65536;
constexpr std::size_t PoolBytes = Block;
constexpr std::size_t ColorBytes = Width * Height * 4;
constexpr std::size_t VertexOffset = 0x2000;
constexpr std::uint8_t Sentinel = 0xc7;

alignas(256) constexpr std::array<std::uint32_t, 10> VertexCode{
    0xe0382000, 0x80020005, 0xe0382010, 0x80020405, 0xbf8c3f70,
    0xf80000cf, 0x03020100, 0xf80008df, 0x07060504, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 7> PixelCode{
    0x7e0002f2, 0x7e0202f2, 0x7e0402f2, 0x7e0602f2, 0xf800080f, 0x03020100, 0xbf810000,
};

alignas(256) std::array<std::uint32_t, 10> StoreCode{0xf4080200u, 0xfa000000u, 0xbf8cc07fu, 0x7e000202u, 0x7e020203u, 0x7e040204u, 0x7e060205u, 0xe07c0000u, 0x80020000u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 15> SpinCode{0xf4080200u, 0xfa000000u, 0xbe840381u, 0x9304ff04u, 0x0019660du, 0x8004ff04u, 0x3c6ef35fu, 0x80828102u, 0xbf078002u, 0xbf85fff9u, 0xbf8cc07fu, 0x7e000204u, 0xe0700000u, 0x80020000u, 0xbf810000u};

struct Vertex {
    std::array<float, 4> position;
    std::array<float, 4> distances;
};

struct Slot {
    alignas(256) std::array<std::uint32_t, 4> storeDescriptor{};
    alignas(256) std::array<std::uint32_t, 4> spin{};
    alignas(256) std::array<std::uint32_t, 4> spinDescriptor{};
    alignas(256) std::uint32_t label = 0;
    alignas(256) std::uint32_t gate = 0;
};

struct RegisteredShader {
    Shader shader;
    ShaderUserData userData;
};
RegisteredShader VertexShader{};
RegisteredShader PixelShader{};

std::byte* ColorMemory = nullptr;
std::byte* Pool = nullptr;
Slot* Slots = nullptr;

const std::vector<std::pair<std::uint32_t, std::uint32_t>> ContextBaseline{
    {0x2d5, 0x2000},
    {0x1b6, 0}, {0x200, 0}, {0x203, 0x800},
    {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
    {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
    {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
    {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
    {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
    {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
    {0x31b, 0}, {0x31d, 0}, {0x3b8, 0x9000000}, {0x1e0, 0},
    {0xc, 0},
    {0x81, 0x80000000}, {0x90, 0x80000000}, {0x94, 0x80000000},
    {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0}, {0x113, std::bit_cast<std::uint32_t>(1.0f)},
};

enum class Consumer { Indices, Vertices };

struct Variant {
    const char* name;
    std::uint32_t primitive;
    std::uint32_t indexType;
    std::uint32_t indexCount;
    Consumer consumer;
    std::array<std::uint32_t, 4> stale;
    std::array<std::uint32_t, 4> produced;
    std::vector<Vertex> vertices;
    bool reuse = false;
};

Vertex At(float x, float y) {
    return {{x, y, 0.5f, 1.0f}, {-1.0f, -1.0f, -1.0f, 0.0f}};
}

std::vector<Variant> Variants() {
    const std::vector<Vertex> triangle{At(-3.0f, -1.0f), At(1.0f, -1.0f), At(1.0f, 3.0f)};
    const std::vector<Vertex> quad{At(-1.0f, -1.0f), At(1.0f, -1.0f), At(1.0f, 1.0f), At(-1.0f, 1.0f)};
    std::vector<Vertex> collapsed = triangle;
    collapsed[2] = collapsed[1];
    const auto bits = [](float value) { return std::bit_cast<std::uint32_t>(value); };
    return {
        {"32-bit triangle-list indices", TriangleList, Index32, 3, Consumer::Indices, {0, 0, 0, 0}, {0, 1, 2, 0}, triangle},
        {"16-bit triangle-list indices", TriangleList, Index16, 3, Consumer::Indices, {0, 0, 0, 0}, {0x00010000u, 0x00000002u, 0, 0}, triangle},
        {"16-bit quad-list indices expanded on the CPU", QuadList, Index16, 4, Consumer::Indices, {0, 0, 0, 0}, {0x00010000u, 0x00030002u, 0, 0}, quad},
        {"32-bit quad-list indices expanded on the CPU", QuadList, Index32, 4, Consumer::Indices, {0, 0, 0, 3}, {0, 1, 2, 3}, quad},
        {"vertex positions behind intact 32-bit indices", TriangleList, Index32, 3, Consumer::Vertices, {0, 0, 0, 0}, {bits(1.0f), bits(3.0f), bits(0.5f), bits(1.0f)}, collapsed},
        {"32-bit triangle-list indices behind their kept stale copy", TriangleList, Index32, 3, Consumer::Indices, {0, 0, 0, 0}, {0, 1, 2, 0}, triangle, true},
    };
}

std::byte* AllocateWatched(std::size_t bytes) {
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block);
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    void* raw = mmap(nullptr, bytes + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(raw != MAP_FAILED, "producer label draw input: cannot map a watched block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + Block) munmap(reinterpret_cast<void*>(aligned + bytes), begin + Block - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    return static_cast<std::byte*>(block);
}

void Register(RegisteredShader& registered, const void* code, std::uint32_t bytes, std::uint8_t type) {
    registered.shader.file_header = 0x34333231u;
    registered.shader.version = 0x18u;
    registered.shader.user_data = &registered.userData;
    registered.shader.code = code;
    registered.shader.header_size = sizeof(RegisteredShader);
    registered.shader.shader_size = bytes;
    registered.shader.type = type;
    AgcDriverRegisterShader_nid_postfix(&registered.shader);
}

std::uint32_t Low(const volatile void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const volatile void* pointer) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u);
}

std::array<std::uint32_t, 4> Descriptor(const void* pointer, std::uint32_t bytes) {
    return {Low(pointer), High(pointer) & 0xffffu, bytes, 0x31016facu};
}

void SetRegister(std::vector<std::uint32_t>& words, std::uint32_t opcode, std::uint32_t offset, std::uint32_t value) {
    words.push_back(0xc0010000u | (opcode << 8u));
    words.push_back(offset);
    words.push_back(value);
}

void Program(std::vector<std::uint32_t>& words, const void* code, std::initializer_list<std::uint32_t> userData) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(code));
    SetRegister(words, SetShReg, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetRegister(words, SetShReg, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    for (const std::uint32_t offset : {0x207u, 0x208u, 0x209u}) SetRegister(words, SetShReg, offset, 1);
    SetRegister(words, SetShReg, 0x212, 0);
    SetRegister(words, SetShReg, 0x213, static_cast<std::uint32_t>(userData.size()) << 1u);
    std::uint32_t offset = 0x240;
    for (const auto value : userData) SetRegister(words, SetShReg, offset++, value);
}

void Spin(std::vector<std::uint32_t>& words, Slot& slot) {
    for (std::uint32_t index = 0; index < SpinDispatches; ++index) {
        Program(words, SpinCode.data(), {Low(slot.spinDescriptor.data()), High(slot.spinDescriptor.data()), SpinIterations});
        words.insert(words.end(), {0xc0031500u, 1, 1, 1, GroupCounts});
    }
}

void Store(std::vector<std::uint32_t>& words, Slot& slot, const std::array<std::uint32_t, 4>& values) {
    Program(words, StoreCode.data(), {Low(slot.storeDescriptor.data()), High(slot.storeDescriptor.data()), values[0], values[1], values[2], values[3]});
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, GroupCounts});
}

void WriteData(std::vector<std::uint32_t>& words, std::uint32_t* target, std::uint32_t value) {
    words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(target), High(target), value});
}

void WaitEqual(std::vector<std::uint32_t>& words, std::uint32_t* target, std::uint32_t value) {
    words.insert(words.end(), {0xc0053c00u, 0x13u, Low(target), High(target), value, 0xffffffffu, 0x19u});
}

void Draw(std::vector<std::uint32_t>& words, const Variant& variant) {
    const auto colorAddress = reinterpret_cast<std::uintptr_t>(ColorMemory);
    const auto poolAddress = reinterpret_cast<std::uintptr_t>(Pool);
    const auto vertexAddress = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixelAddress = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    for (const auto& [offset, value] : ContextBaseline) SetRegister(words, SetContextReg, offset, value);
    SetRegister(words, SetContextReg, 0x207, DistanceVector);
    SetRegister(words, SetContextReg, 0x318, static_cast<std::uint32_t>(colorAddress >> 8u));
    SetRegister(words, SetContextReg, 0x390, static_cast<std::uint32_t>(colorAddress >> 40u));
    SetRegister(words, SetContextReg, 0x3b0, ((Width - 1u) << 14u) | (Height - 1u));
    SetRegister(words, SetContextReg, 0x10f, std::bit_cast<std::uint32_t>(Width / 2.0f));
    SetRegister(words, SetContextReg, 0x110, std::bit_cast<std::uint32_t>(Width / 2.0f));
    SetRegister(words, SetContextReg, 0x111, std::bit_cast<std::uint32_t>(-(Height / 2.0f)));
    SetRegister(words, SetContextReg, 0x112, std::bit_cast<std::uint32_t>(Height / 2.0f));
    const auto extent = (Height << 16u) | Width;
    SetRegister(words, SetContextReg, 0xd, extent);
    SetRegister(words, SetContextReg, 0x82, extent);
    SetRegister(words, SetContextReg, 0x91, extent);
    SetRegister(words, SetContextReg, 0x95, extent);
    SetRegister(words, SetUconfigReg, 0x242, variant.primitive);
    SetRegister(words, SetShReg, 0x008, static_cast<std::uint32_t>(pixelAddress >> 8u));
    SetRegister(words, SetShReg, 0x009, static_cast<std::uint32_t>(pixelAddress >> 40u));
    SetRegister(words, SetShReg, 0x00b, 0u);
    SetRegister(words, SetShReg, 0x0c8, static_cast<std::uint32_t>(vertexAddress >> 8u));
    SetRegister(words, SetShReg, 0x0c9, static_cast<std::uint32_t>(vertexAddress >> 40u));
    SetRegister(words, SetShReg, 0x08b, 4u << 1u);
    const auto* vertices = Pool + VertexOffset;
    const auto vertexBuffer = reinterpret_cast<std::uintptr_t>(vertices);
    const std::array<std::uint32_t, 4> descriptor{static_cast<std::uint32_t>(vertexBuffer), static_cast<std::uint32_t>((vertexBuffer >> 32u) & 0xffffu) | (static_cast<std::uint32_t>(sizeof(Vertex)) << 16u), static_cast<std::uint32_t>(variant.vertices.size()), 0x01016facu};
    for (std::uint32_t i = 0; i < descriptor.size(); ++i) SetRegister(words, SetShReg, 0x08c + i, descriptor[i]);
    words.insert(words.end(), {0xc0012600u, static_cast<std::uint32_t>(poolAddress), static_cast<std::uint32_t>(poolAddress >> 32u)});
    words.insert(words.end(), {0xc0001300u, variant.indexCount});
    SetRegister(words, SetUconfigReg, 0x243, 0x400u | variant.indexType);
    words.insert(words.end(), {DrawIndexOffset2Header, variant.indexCount, 0u, variant.indexCount, 0u});
}

void Submit(std::uint32_t queue, std::vector<std::uint32_t>& words, const char* what) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    const int result = queue == Graphics ? sceAgcDriverSubmitDcb(&packet) : sceAgcDriverSubmitAcb(queue, &packet);
    Require(result == 0, std::string("producer label draw input: the submission failed for ") + what);
}

std::size_t WrittenPixels(const char* name) {
    std::vector<std::byte> colors(ColorBytes);
    AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ColorMemory)), colors);
    std::size_t written = 0;
    for (std::uint32_t pixel = 0; pixel < Width * Height; ++pixel) {
        const auto value = std::to_integer<std::uint8_t>(colors[pixel * 4u]);
        Require(value == 0u || value == 255u, std::string(name) + ": pixel " + std::to_string(pixel) + " stored " + std::to_string(value));
        written += value == 255u ? 1u : 0u;
    }
    return written;
}

std::byte* ProducedTarget(const Variant& variant) {
    return variant.consumer == Consumer::Indices ? Pool : Pool + VertexOffset + 2 * sizeof(Vertex);
}

void Prepare(const Variant& variant, Slot& slot) {
    std::memset(Pool, Sentinel, PoolBytes);
    std::memcpy(Pool + VertexOffset, variant.vertices.data(), variant.vertices.size() * sizeof(Vertex));
    if (variant.consumer == Consumer::Indices) {
        std::memcpy(Pool, variant.stale.data(), sizeof(variant.stale));
    } else {
        const std::array<std::uint32_t, 3> indices{0, 1, 2};
        std::memcpy(Pool, indices.data(), sizeof(indices));
    }
    std::memset(ColorMemory, 0, ColorBytes);
    AgcDriver::GuestMemory::MarkWritten(reinterpret_cast<std::uintptr_t>(Pool), PoolBytes);
    AgcDriver::GuestMemory::MarkWritten(reinterpret_cast<std::uintptr_t>(ColorMemory), ColorBytes);
    slot.storeDescriptor = Descriptor(ProducedTarget(variant), 16);
    slot.spinDescriptor = Descriptor(slot.spin.data(), sizeof(slot.spin));
    slot.label = 0;
    slot.gate = 0;
}

struct Access {
    bool flush;
    std::thread::id thread;
    std::uint64_t address;
    std::size_t bytes;
    bool producedBefore;
    bool producedAfter;
};

bool Imported = false;
bool InPlace = false;
std::optional<std::thread::id> ConsumerThread;

std::mutex AccessMutex;
std::vector<Access> Accesses;
std::uint64_t WatchBegin = 0;
std::uint64_t WatchEnd = 0;
std::array<std::uint32_t, 4> WatchProduced{};
void (*PreviousFlush)(std::uint64_t, std::size_t) = nullptr;

bool Watched(std::uint64_t address, std::size_t bytes) {
    return address < WatchEnd && WatchBegin < address + bytes;
}

bool Produced() {
    std::array<std::uint32_t, 4> held{};
    std::memcpy(held.data(), reinterpret_cast<const void*>(WatchBegin), sizeof(held));
    return held == WatchProduced;
}

void TracingFlush(std::uint64_t address, std::size_t bytes) {
    const bool watched = Watched(address, bytes);
    const bool before = watched && Produced();
    PreviousFlush(address, bytes);
    if (!watched) return;
    std::lock_guard lock(AccessMutex);
    Accesses.push_back({true, std::this_thread::get_id(), address, bytes, before, Produced()});
}

void TracingGuard(std::uint64_t address, std::size_t bytes) {
    if (!Watched(address, bytes)) return;
    const bool produced = Produced();
    std::lock_guard lock(AccessMutex);
    Accesses.push_back({false, std::this_thread::get_id(), address, bytes, produced, produced});
}

void Trace(const Variant& variant) {
    std::lock_guard lock(AccessMutex);
    Accesses.clear();
    WatchBegin = reinterpret_cast<std::uintptr_t>(ProducedTarget(variant));
    WatchEnd = WatchBegin + sizeof(variant.produced);
    WatchProduced = variant.produced;
    PreviousFlush = AgcDriver::GuestMemory::CurrentFlushHook();
    Require(PreviousFlush != nullptr, std::string(variant.name) + ": no flush hook is installed to trace");
    AgcDriver::GuestMemory::SetFlushHook(&TracingFlush);
    AgcDriver::GuestMemory::SetReadGuard(&TracingGuard);
}

std::vector<Access> Untrace() {
    AgcDriver::GuestMemory::SetReadGuard(nullptr);
    AgcDriver::GuestMemory::SetFlushHook(PreviousFlush);
    std::lock_guard lock(AccessMutex);
    return std::exchange(Accesses, {});
}

bool CopiedWriters() {
    return !Imported;
}

void RequireAccesses(const Variant& variant, const std::vector<Access>& accesses) {
    const std::string name = variant.name;
    const auto begin = reinterpret_cast<std::uintptr_t>(ProducedTarget(variant));
    const auto end = variant.consumer == Consumer::Indices ? begin + static_cast<std::uint64_t>(variant.indexCount) * (variant.indexType == Index16 ? 2u : 4u) : begin + sizeof(variant.produced);
    const auto inside = [&](const Access& access) { return access.address < end && begin < access.address + access.bytes; };
    std::vector<std::size_t> reads;
    for (std::size_t i = 0; i < accesses.size(); ++i) {
        if (!accesses[i].flush && inside(accesses[i]) && ConsumerThread && accesses[i].thread == *ConsumerThread) reads.push_back(i);
    }
    for (const auto i : reads) Require(accesses[i].producedAfter, name + ": a read of the input at 0x" + std::to_string(accesses[i].address) + "+" + std::to_string(accesses[i].bytes) + " saw the stale bytes");
    if (variant.consumer == Consumer::Vertices) {
        if (reads.empty()) {
            Require(Imported, name + ": with copied writes the vertices must be read on the CPU, and no read of them was seen");
            std::printf("%s: no CPU read of the vertices; the GPU reads them through their import\n", variant.name);
        } else {
            std::printf("%s: %zu CPU reads of the vertices, all of the produced bytes\n", variant.name, reads.size());
        }
        return;
    }
    Require(!reads.empty(), name + ": the draw did not read its indices from guest memory");
    const auto consumer = accesses[reads.front()].thread;
    std::vector<std::size_t> covering;
    for (std::size_t i = 0; i < accesses.size(); ++i) {
        const auto& access = accesses[i];
        if (!access.flush || access.thread != consumer || !inside(access)) continue;
        Require(access.address <= begin && access.address + access.bytes >= end, name + ": the consumer flushed only part of its index range (0x" + std::to_string(access.address) + "+" + std::to_string(access.bytes) + ")");
        covering.push_back(i);
    }
    Require(covering.size() == 1, name + ": the consumer flushed its index range " + std::to_string(covering.size()) + " times, expected once");
    const auto& flush = accesses[covering.front()];
    Require(covering.front() < reads.front(), name + ": the consumer read its indices before flushing them");
    Require(flush.producedAfter, name + ": the consumer's flush ended before the producer's store landed");
    if (Imported) {
        Require(!flush.producedBefore, name + ": the consumer's flush began after the producer's store had landed, so the case does not test that the flush orders the copy");
        std::printf("%s: one flush, begun with the producer outstanding, landed the store before %zu reads\n", variant.name, reads.size());
    } else {
        Require(flush.producedBefore, name + ": with copied writes the label should hold the consumer until the write-back landed");
        std::printf("%s: one flush, after the write-back the label waited for, before %zu reads\n", variant.name, reads.size());
    }
}

void DrawStale(const Variant& variant, const char* what) {
    std::vector<std::uint32_t> draw;
    Draw(draw, variant);
    Submit(Graphics, draw, what);
    AgcDriverWaitIdle_nid_postfix();
    Require(WrittenPixels(variant.name) == 0, std::string(variant.name) + ": " + what + " covered pixels, so a draw that copied early would not be told apart");
}

void RequireStaleDegenerate(const Variant& variant, Slot& slot) {
    Prepare(variant, slot);
    Trace(variant);
    DrawStale(variant, "the draw over the stale input");
    const auto first = Untrace();
    if (const auto read = std::find_if(first.begin(), first.end(), [](const Access& access) { return !access.flush; }); read != first.end()) {
        Require(!ConsumerThread || *ConsumerThread == read->thread, std::string(variant.name) + ": the stale draw read its input on another thread than the earlier draws");
        ConsumerThread = read->thread;
    }
    if (!variant.reuse) return;
    Trace(variant);
    DrawStale(variant, "the second draw over the stale input");
    const auto accesses = Untrace();
    const bool reused = std::none_of(accesses.begin(), accesses.end(), [](const Access& access) { return !access.flush; });
    if (!CopiedWriters() && !reused) {
        std::printf("%s: imported memory is compared, not write-watched, on this device, so no copy is kept to reuse\n", variant.name);
        return;
    }
    Require(reused, std::string(variant.name) + ": the second draw over the unchanged stale input read it again instead of reusing its kept copy, so the case below would not test reuse");
    std::printf("%s: the second stale draw reused its kept copy\n", variant.name);
}

void Run(const Variant& variant, Slot& slot) {
    RequireStaleDegenerate(variant, slot);
    if (variant.reuse) {
        std::memset(ColorMemory, 0, ColorBytes);
        AgcDriver::GuestMemory::MarkWritten(reinterpret_cast<std::uintptr_t>(ColorMemory), ColorBytes);
        slot.label = 0;
        slot.gate = 0;
    } else {
        Prepare(variant, slot);
    }
    std::vector<std::uint32_t> producer;
    WaitEqual(producer, &slot.gate, GateOpen);
    Spin(producer, slot);
    Store(producer, slot, variant.produced);
    WriteData(producer, &slot.label, LabelValue);
    std::vector<std::uint32_t> consumer;
    WaitEqual(consumer, &slot.label, LabelValue);
    Draw(consumer, variant);
    Trace(variant);
    Submit(Compute, producer, "the gated producer and its label");
    Submit(Graphics, consumer, "the label wait and the draw");
    std::this_thread::sleep_for(ProducerStall);
    Require(slot.label == 0, std::string(variant.name) + ": the label landed while its producer was still gated");
    reinterpret_cast<volatile std::uint32_t&>(slot.gate) = GateOpen;
    AgcDriverWaitIdle_nid_postfix();
    const auto accesses = Untrace();
    Require(slot.label == 1, std::string(variant.name) + ": the producer's label never landed");
    Require(std::memcmp(ProducedTarget(variant), variant.produced.data(), sizeof(variant.produced)) == 0, std::string(variant.name) + ": the producer's store did not reach guest memory");
    const auto written = WrittenPixels(variant.name);
    Require(written == Width * Height, std::string(variant.name) + ": the draw behind the label covered " + std::to_string(written) + " of " + std::to_string(Width * Height) + " pixels: it read its input before the producer the label follows wrote it");
    RequireAccesses(variant, accesses);
}

}

int main(int argc, char** argv) {
#ifdef _WIN32
    _putenv_s("APS5_NO_DRAW_INPUTS_IN_PLACE", "1");
#else
    setenv("APS5_NO_DRAW_INPUTS_IN_PLACE", "1", 1);
#endif
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            Imported = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device).hostImportAlignment != 0;
            InPlace = device->DrawInputsInPlace();
            Require(!InPlace, "producer label draw input: draw inputs are read in place although APS5_NO_DRAW_INPUTS_IN_PLACE is set");
            std::printf("memory mode: %s, draw inputs in place: %s\n", Imported ? "imported" : "copied", InPlace ? "yes" : "no");
        }
        ColorMemory = AllocateWatched(2 * Block);
        Pool = AllocateWatched(PoolBytes);
        auto* slotBlock = AllocateWatched(Block);
        GuestAllocations::Mutation().Add(Pool, PoolBytes, true, true, true);
        GuestAllocations::Mutation().Add(slotBlock, Block, true, true, true);
        Slots = new (slotBlock) Slot[1];
        Register(VertexShader, VertexCode.data(), sizeof(VertexCode), 2);
        Register(PixelShader, PixelCode.data(), sizeof(PixelCode), 1);
        const auto variants = Variants();
        for (std::size_t index = 0; index < variants.size(); ++index) {
            if (argc > 1 && std::to_string(index) != argv[1]) continue;
            Run(variants[index], Slots[0]);
            std::cout << variants[index].name << ": passed\n";
        }
        std::cout << "producer label draw input tests passed\n";
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}
