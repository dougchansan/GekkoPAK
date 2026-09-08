#ifndef GEKKOPAK_CONFORMANCE_TARGETS_H
#define GEKKOPAK_CONFORMANCE_TARGETS_H

#include <vector>

#include "vector_runner.h"

namespace gekkopak {
namespace conformance {

// Every host-buildable GekkoPAK transport, in the order the cross-check
// compares them. The first is the reference.
const std::vector<Target>& AllTargets();

// Device-local pool every target is configured with, so HELLO and GET_CAPS
// answer identically. Real deployments differ here -- 32 MiB in the emulator,
// 64 KiB on the RP2040 -- and that is a capability difference, not a protocol
// one.
constexpr std::uint32_t kConformancePoolBytes = 64u * 1024u;

} // namespace conformance
} // namespace gekkopak

#endif // GEKKOPAK_CONFORMANCE_TARGETS_H
