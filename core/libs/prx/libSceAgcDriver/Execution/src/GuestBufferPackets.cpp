#include "prx/libSceAgcDriver/Execution/include/GuestBufferProfile.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"

namespace AgcDriver::GuestBufferProfile {
namespace {

constexpr std::uint64_t StoreSignalBytes = 64;

}

void NotePacket(std::uint32_t opcode, std::span<const std::uint32_t> packet) {
    if (!Recording()) return;
    if (Pm4::DrawOpcode(opcode)) {
        NotePacketEffect(PacketEffect::Draw);
    } else if (opcode == 0x15 || opcode == 0x16) {
        NotePacketEffect(PacketEffect::Dispatch);
    } else if ((opcode == 0x3c || opcode == 0x93) && packet[0] != RenderingWaitPacketHeader && packet.size() >= 4) {
        const auto awaitedBytes = Pm4::WaitAwaitedBytes(packet);
        NotePacketEffect(PacketEffect::Wait, (packet[2] & (awaitedBytes == 8 ? ~7u : ~3u)) | (static_cast<std::uint64_t>(packet[3]) << 32u));
    } else if (opcode == 0x37 || opcode == 0x49) {
        const auto label = Pm4::DecodeLabelWrite(packet);
        NotePacketEffect(label ? PacketEffect::Label : PacketEffect::Other, label ? label->address : 0);
    } else {
        NotePacketEffect(PacketEffect::Other);
        if (opcode == 0x40 || opcode == 0x50 || opcode == 0x83) {
            for (const auto& store : Pm4::DecodeStores(packet)) {
                if (store.bytes == 0 || store.bytes > StoreSignalBytes) continue;
                for (auto address = store.address & ~std::uint64_t{7}; address < store.address + store.bytes; address += 8) NoteStoreSignal(address);
            }
        }
    }
}

}
