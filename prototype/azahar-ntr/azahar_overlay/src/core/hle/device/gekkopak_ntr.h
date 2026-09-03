#pragma once

#include <memory>
#include "common/common_types.h"

class BackingMem;

namespace GekkoPakNtr {

constexpr u32 PhysicalBase = 0x10164000u;
constexpr u32 VirtualBase = 0x1EC64000u;
constexpr u32 RegisterPageSize = 0x1000u;

std::shared_ptr<BackingMem> GetRegisterMemory();
void Reset();
void Tick();

} // namespace GekkoPakNtr
