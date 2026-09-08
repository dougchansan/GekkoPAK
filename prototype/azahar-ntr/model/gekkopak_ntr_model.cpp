#include "gekkopak_ntr_model.h"

#include <cstring>

namespace gekkopak::ntr {

Device::Device(std::uint32_t local_bytes) : pool_(local_bytes, 0) {
    gekkopak::Device::Config config;
    config.pool = pool_.data();
    config.pool_bytes = local_bytes;
    config.reported_local_bytes = local_bytes;
    core_.Reset(config);
    Store32(kRegRomCnt, kCardResetHigh);
}

std::uint32_t Device::Load32(std::size_t off) const {
    std::uint32_t v = 0;
    std::memcpy(&v, regs_.data() + off, sizeof(v));
    return v;
}

void Device::Store32(std::size_t off, std::uint32_t value) {
    std::memcpy(regs_.data() + off, &value, sizeof(value));
}

void Device::Tick() {
    const std::uint32_t romcnt = Load32(kRegRomCnt);
    if ((romcnt & kCardActivate) == 0)
        return;

    std::uint8_t bytes[protocol::kCommandBytes]{};
    std::memcpy(bytes, regs_.data() + kRegCommand, sizeof(bytes));
    Store32(kRegRomCnt, romcnt & ~(kCardActivate | kCardDataReady));

    const protocol::WireCommand cmd = protocol::DecodeCommand(bytes);
    core_.count_transfer();
    if (!cmd.valid)
        return;

    switch (cmd.opcode) {
    case protocol::kWireWriteReg:
        core_.WriteReg(cmd.index, cmd.word);
        break;
    case protocol::kWireExec:
        core_.Exec(cmd.index, cmd.word);
        break;
    case protocol::kWireReadReg:
        Store32(kRegFifo, core_.ReadReg(cmd.index));
        Store32(kRegRomCnt, Load32(kRegRomCnt) | kCardDataReady);
        break;
    case protocol::kWireWritePayloadWord:
        core_.WritePayloadWord(cmd.index, cmd.word);
        break;
    case protocol::kWireWriteBlock:
        core_.WriteBlock(cmd.index, cmd.word, regs_.data() + kRegBlockTx, kBlockBytes);
        break;
    case protocol::kWireReadBlock:
        core_.ReadBlock(cmd.index, cmd.word, regs_.data() + kRegBlockRx, kBlockBytes);
        break;
    default:
        break;
    }
}

bool Device::WriteBlock(BlockSelector selector, const std::uint8_t* data, std::size_t len,
                        std::uint16_t /*wire_flags*/) {
    core_.count_transfer();
    if (data == nullptr || len == 0 || len > kBlockBytes)
        return false;
    const std::uint32_t word =
        protocol::EncodeWriteBlockWord(static_cast<std::uint16_t>(len));
    return core_.WriteBlock(static_cast<std::uint8_t>(selector), word, data, len) == protocol::kOk;
}

std::uint32_t Device::ReadEvent() {
    core_.count_transfer();
    return core_.ReadReg(protocol::kEventCompletionDepth);
}

std::size_t Device::ReadBlock(BlockSelector selector, std::uint16_t offset, std::uint8_t* out,
                              std::size_t len) {
    core_.count_transfer();
    if (out == nullptr || len == 0 || len > kBlockBytes)
        return 0;
    const std::uint32_t word =
        protocol::EncodeReadBlockWord(offset, static_cast<std::uint16_t>(len));
    return core_.ReadBlock(static_cast<std::uint8_t>(selector), word, out, len);
}

} // namespace gekkopak::ntr
