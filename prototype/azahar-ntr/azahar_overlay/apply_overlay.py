#!/usr/bin/env python3
import argparse
from pathlib import Path
import shutil
import sys


def replace_once(path: Path, old: str, new: str):
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one match, got {count}: {old[:80]!r}")
    path.write_text(text.replace(old, new, 1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("azahar", type=Path)
    args = ap.parse_args()
    root = args.azahar.resolve()
    here = Path(__file__).resolve().parent

    device_dst = root / "src/core/hle/device"
    device_dst.mkdir(parents=True, exist_ok=True)
    for name in ("gekkopak_ntr.h", "gekkopak_ntr.cpp"):
        shutil.copy2(here / "src/core/hle/device" / name, device_dst / name)

    cmake = root / "src/core/CMakeLists.txt"
    replace_once(
        cmake,
        "    hle/ipc.h\n    hle/ipc_helpers.h\n",
        "    hle/ipc.h\n    hle/ipc_helpers.h\n"
        "    hle/device/gekkopak_ntr.cpp\n    hle/device/gekkopak_ntr.h\n",
    )

    kmem = root / "src/core/hle/kernel/memory.cpp"
    replace_once(
        kmem,
        '#include "core/hle/kernel/config_mem.h"\n',
        '#include "core/hle/kernel/config_mem.h"\n#include "core/hle/device/gekkopak_ntr.h"\n',
    )
    replace_once(
        kmem,
        '''    if (area->paddr_base == IO_AREA_PADDR) {\n        LOG_ERROR(Loader, "MMIO mappings are not supported yet. phys_addr=0x{:08X}",\n                  area->paddr_base + offset_into_region);\n        return;\n    }\n''',
        '''    if (area->paddr_base == IO_AREA_PADDR) {\n        const PAddr phys_addr = area->paddr_base + offset_into_region;\n        if (phys_addr >= GekkoPakNtr::PhysicalBase) {\n            const u32 gekkopak_offset = phys_addr - GekkoPakNtr::PhysicalBase;\n            if (gekkopak_offset + mapping.size <= GekkoPakNtr::RegisterPageSize) {\n                GekkoPakNtr::Reset();\n                auto target = MemoryRef{GekkoPakNtr::GetRegisterMemory(), gekkopak_offset};\n                auto vma = address_space\n                               .MapBackingMemory(mapping.address, target, mapping.size, MemoryState::IO)\n                               .Unwrap();\n                address_space.Reprotect(\n                    vma, mapping.read_only ? VMAPermission::Read : VMAPermission::ReadWrite);\n                LOG_INFO(Loader, "Mapped GekkoPAK NTRCARD window at 0x{:08X}", mapping.address);\n                return;\n            }\n        }\n        LOG_ERROR(Loader, "MMIO mappings are not supported yet. phys_addr=0x{:08X}", phys_addr);\n        return;\n    }\n''',
    )

    proc = root / "src/core/hle/kernel/process.cpp"
    replace_once(
        proc,
        '''        {0x1F000000, 0x600000, false}, // entire VRAM\n    };\n''',
        '''        {0x1F000000, 0x600000, false}, // entire VRAM\n        // GekkoPAK development mapping: NTRCARD register page (phys 0x10164000).\n        // Real retail user applications normally reach this hardware through privileged software.\n        {0x1EC64000, 0x1000, false},\n    };\n''',
    )

    core = root / "src/core/core.cpp"
    replace_once(
        core,
        '#include "core/hle/kernel/process.h"\n',
        '#include "core/hle/kernel/process.h"\n#include "core/hle/device/gekkopak_ntr.h"\n',
    )
    replace_once(
        core,
        '''    return status;\n}\n\nbool System::SendSignal''',
        '''    // Service GekkoPAK's register-backed NTRCARD device once per CPU slice.\n    GekkoPakNtr::Tick();\n    return status;\n}\n\nbool System::SendSignal''',
    )

    print("Applied GekkoPAK NTRCARD overlay to", root)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("overlay failed:", e, file=sys.stderr)
        sys.exit(1)
