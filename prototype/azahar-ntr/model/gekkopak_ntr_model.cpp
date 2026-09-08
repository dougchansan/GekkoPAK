#include "gekkopak_ntr_model.h"

#include "gekkopak/ntr_register_transport.h"

#include <cstring>

namespace gekkopak::ntr {

Device::Device(std::uint32_t local_bytes) : pool_(local_bytes, 0) {
    gekkopak::Device::Config config;
    config.pool = pool_.data();
    config.pool_bytes = local_bytes;
    config.reported_local_bytes = local_bytes;
    core_.Reset(config);
    transport_.Reset(regs_.data());
}

std::uint32_t Device::Load32(std::size_t off) const {
    return ntr_transport::Load32(regs_.data(), off);
}

void Device::Store32(std::size_t off, std::uint32_t value) {
    ntr_transport::Store32(regs_.data(), off, value);
}

void Device::Tick() {
    transport_.Tick(core_, regs_.data());
}

bool Device::WriteBlock(BlockSelector selector, const std::uint8_t* data, std::size_t len,
                        std::uint16_t /*wire_flags*/) {
    core_.count_transfer();
    if (data == nullptr || len == 0 || len > kBlockBytes)
        return false;
    const std::uint32_t word = protocol::EncodeWriteBlockWord(static_cast<std::uint16_t>(len));
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
