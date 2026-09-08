#!/usr/bin/env python3
"""Apply the GekkoPAK NTRCARD overlay to an Azahar source tree.

Two things get installed:

1. The GekkoPAK device and the shared protocol core, as new files.
2. An MMIO page type in Azahar's memory core.

(2) needs explaining, because it is the only part that touches emulator
internals. GekkoPAK's cartridge transport is access-driven: reading the data
FIFO is what advances a transfer and clears DATA_READY, exactly as on hardware.
Modelling that requires the emulator to observe an individual register access.
Azahar cannot -- Citra's MMIORegion was removed and PageType has no MMIO entry,
so a mapped page is simply memory and the device never sees a read.

The patch adds one PageType, one VMAType, four switch cases and two small
mapping entry points. The dynarmic JIT needs no change at all: it already falls
back to the memory callbacks for any page whose pointer is null, which is how
MemoryWatchpoint works today.

See docs/AZAHAR_MMIO.md.
"""

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


def install_files(root: Path, here: Path):
    device_dst = root / "src/core/hle/device"
    device_dst.mkdir(parents=True, exist_ok=True)
    for name in ("gekkopak_ntr.h", "gekkopak_ntr.cpp"):
        shutil.copy2(here / "src/core/hle/device" / name, device_dst / name)

    # The shared GekkoPAK protocol/device core travels with the adapter. It is
    # the same source the host model and the DSpico firmware compile, which is
    # what makes the three implementations conformant by construction rather
    # than by inspection.
    repo = here.parents[2]
    core_dst = root / "src/gekkopak"
    core_dst.mkdir(parents=True, exist_ok=True)
    for name in ("protocol.h", "device.h", "ntr_register_transport.h"):
        shutil.copy2(repo / "include/gekkopak" / name, core_dst / name)
    shutil.copy2(repo / "src/device.cpp", device_dst / "gekkopak_device.cpp")

    replace_once(
        root / "src/core/CMakeLists.txt",
        "    hle/ipc.h\n    hle/ipc_helpers.h\n",
        "    hle/ipc.h\n    hle/ipc_helpers.h\n"
        "    hle/device/gekkopak_ntr.cpp\n    hle/device/gekkopak_ntr.h\n"
        "    hle/device/gekkopak_device.cpp\n",
    )


def patch_mmio_page_type(root: Path):
    """Add PageType::MMIO and a mapping entry point to the memory core."""
    mem_h = root / "src/core/memory.h"
    replace_once(
        mem_h,
        "    RasterizerCachedMemoryWatchpoint,\n};",
        "    RasterizerCachedMemoryWatchpoint,\n"
        "    /// Page is a memory-mapped IO window. It has no backing pointer, so every\n"
        "    /// access is dispatched to a device instead of being served from memory.\n"
        "    /// Added for GekkoPAK's NTRCARD register window.\n"
        "    MMIO,\n};",
    )
    replace_once(
        mem_h,
        "    void MapMemoryRegion(PageTable& page_table, VAddr base, u32 size, MemoryRef target);",
        "    void MapMemoryRegion(PageTable& page_table, VAddr base, u32 size, MemoryRef target);\n"
        "    /// Maps an MMIO window. The pages get no backing pointer, which is what\n"
        "    /// routes every access through Read/Write for both the interpreter and\n"
        "    /// the dynarmic JIT.\n"
        "    void MapMMIORegion(PageTable& page_table, VAddr base, u32 size);",
    )

    mem_cpp = root / "src/core/memory.cpp"
    replace_once(
        mem_cpp,
        '#include "core/hle/kernel/process.h"\n',
        '#include "core/hle/device/gekkopak_ntr.h"\n#include "core/hle/kernel/process.h"\n',
    )

    # Read<T>. The ReadType declaration distinguishes this from the write path.
    replace_once(
        mem_cpp,
        """    case PageType::MemoryWatchpoint: {
        auto it = page_table->watchpoint_pages_map.find(vaddr >> CITRA_PAGE_BITS);
        ASSERT_MSG(it != page_table->watchpoint_pages_map.end(),
                   "Missing memory for watchpoint page");

        ReadType value;""",
        """    case PageType::MMIO: {
        // Strictly 32-bit, like every other MMIO access on this bus.
        ASSERT_MSG(read_size == sizeof(u32), "non-32-bit MMIO read @ {:08X}", vaddr);
        return static_cast<ReadType>(GekkoPakNtr::Read32(vaddr & CITRA_PAGE_MASK));
    }
    case PageType::MemoryWatchpoint: {
        auto it = page_table->watchpoint_pages_map.find(vaddr >> CITRA_PAGE_BITS);
        ASSERT_MSG(it != page_table->watchpoint_pages_map.end(),
                   "Missing memory for watchpoint page");

        ReadType value;""",
    )

    # Write<T>.
    replace_once(
        mem_cpp,
        """    case PageType::MemoryWatchpoint: {
        auto it = page_table->watchpoint_pages_map.find(vaddr >> CITRA_PAGE_BITS);
        ASSERT_MSG(it != page_table->watchpoint_pages_map.end(),
                   "Missing memory for watchpoint page");

        std::memcpy(it->second.memory.GetPtr() + (vaddr & CITRA_PAGE_MASK), &data, sizeof(T));""",
        """    case PageType::MMIO: {
        ASSERT_MSG(sizeof(T) == sizeof(u32), "non-32-bit MMIO write @ {:08X}", vaddr);
        GekkoPakNtr::Write32(vaddr & CITRA_PAGE_MASK, static_cast<u32>(data));
        return;
    }
    case PageType::MemoryWatchpoint: {
        auto it = page_table->watchpoint_pages_map.find(vaddr >> CITRA_PAGE_BITS);
        ASSERT_MSG(it != page_table->watchpoint_pages_map.end(),
                   "Missing memory for watchpoint page");

        std::memcpy(it->second.memory.GetPtr() + (vaddr & CITRA_PAGE_MASK), &data, sizeof(T));""",
    )

    # A block copy across an MMIO page becomes a word loop. Nothing in GekkoPAK
    # does this, but the switches have no default beyond UNREACHABLE().
    replace_once(
        mem_cpp,
        """                std::memcpy(dest_buffer, GetPointerForRasterizerCache(current_vaddr), copy_amount);
                break;
            }
            default:
                UNREACHABLE();""",
        """                std::memcpy(dest_buffer, GetPointerForRasterizerCache(current_vaddr), copy_amount);
                break;
            }
            case PageType::MMIO: {
                u8* out = static_cast<u8*>(dest_buffer);
                for (std::size_t i = 0; i + sizeof(u32) <= copy_amount; i += sizeof(u32)) {
                    const u32 word =
                        GekkoPakNtr::Read32(static_cast<u32>(current_vaddr + i) & CITRA_PAGE_MASK);
                    std::memcpy(out + i, &word, sizeof(word));
                }
                break;
            }
            default:
                UNREACHABLE();""",
    )
    replace_once(
        mem_cpp,
        """                std::memcpy(GetPointerForRasterizerCache(current_vaddr), src_buffer, copy_amount);
                break;
            }
            default:
                UNREACHABLE();""",
        """                std::memcpy(GetPointerForRasterizerCache(current_vaddr), src_buffer, copy_amount);
                break;
            }
            case PageType::MMIO: {
                const u8* in = static_cast<const u8*>(src_buffer);
                for (std::size_t i = 0; i + sizeof(u32) <= copy_amount; i += sizeof(u32)) {
                    u32 word = 0;
                    std::memcpy(&word, in + i, sizeof(word));
                    GekkoPakNtr::Write32(static_cast<u32>(current_vaddr + i) & CITRA_PAGE_MASK,
                                         word);
                }
                break;
            }
            default:
                UNREACHABLE();""",
    )

    replace_once(
        mem_cpp,
        "void MemorySystem::UnmapRegion(PageTable& page_table, VAddr base, u32 size) {",
        """void MemorySystem::MapMMIORegion(PageTable& page_table, VAddr base, u32 size) {
    ASSERT_MSG((size & CITRA_PAGE_MASK) == 0, "non-page aligned size: {:08X}", size);
    ASSERT_MSG((base & CITRA_PAGE_MASK) == 0, "non-page aligned base: {:08X}", base);
    // A null backing pointer is what makes both the interpreter and the
    // dynarmic JIT route every access through Read/Write instead of a memcpy.
    MapPages(page_table, base / CITRA_PAGE_SIZE, size / CITRA_PAGE_SIZE, nullptr, PageType::MMIO);
}

void MemorySystem::UnmapRegion(PageTable& page_table, VAddr base, u32 size) {""",
    )


def patch_vm_manager(root: Path):
    """Teach the VM manager about MMIO regions.

    Overwriting the page table behind the VM manager's back would work until
    something re-derived it from the VMA -- a reprotect, a state change, a
    merge -- and silently turned the window back into RAM. A VMA type keeps the
    two consistent instead.
    """
    vm_h = root / "src/core/hle/kernel/vm_manager.h"
    replace_once(
        vm_h,
        "    /// VMA is backed by a raw, unmanaged pointer.\n    BackingMemory,\n};",
        "    /// VMA is backed by a raw, unmanaged pointer.\n    BackingMemory,\n"
        "    /// VMA is a memory-mapped IO window served by a device rather than memory.\n"
        "    MMIO,\n};",
    )
    replace_once(
        vm_h,
        "    VMAHandle Reprotect(VMAHandle vma, VMAPermission new_perms);",
        "    VMAHandle Reprotect(VMAHandle vma, VMAPermission new_perms);\n\n"
        "    /// Converts an already-mapped VMA into an MMIO window, so its pages are\n"
        "    /// dispatched to a device instead of being served from memory.\n"
        "    void MakeMMIO(VMAHandle vma);",
    )

    vm_cpp = root / "src/core/hle/kernel/vm_manager.cpp"
    replace_once(
        vm_cpp,
        """    case VMAType::BackingMemory:
        memory.MapMemoryRegion(*page_table, vma.base, vma.size, vma.backing_memory);
        break;
    }""",
        """    case VMAType::BackingMemory:
        memory.MapMemoryRegion(*page_table, vma.base, vma.size, vma.backing_memory);
        break;
    case VMAType::MMIO:
        memory.MapMMIORegion(*page_table, vma.base, vma.size);
        break;
    }""",
    )
    replace_once(
        vm_cpp,
        "void VMManager::UpdatePageTableForVMA(const VirtualMemoryArea& vma) {",
        """void VMManager::MakeMMIO(VMAHandle vma) {
    VirtualMemoryArea& area = vma_map[vma->first];
    area.type = VMAType::MMIO;
    UpdatePageTableForVMA(area);
}

void VMManager::UpdatePageTableForVMA(const VirtualMemoryArea& vma) {""",
    )


def patch_kernel_mapping(root: Path):
    """Map the NTRCARD window as MMIO instead of refusing it."""
    kmem = root / "src/core/hle/kernel/memory.cpp"
    replace_once(
        kmem,
        '#include "core/hle/kernel/config_mem.h"\n',
        '#include "core/hle/kernel/config_mem.h"\n#include "core/hle/device/gekkopak_ntr.h"\n',
    )
    replace_once(
        kmem,
        """    if (area->paddr_base == IO_AREA_PADDR) {
        LOG_ERROR(Loader, "MMIO mappings are not supported yet. phys_addr=0x{:08X}",
                  area->paddr_base + offset_into_region);
        return;
    }
""",
        """    if (area->paddr_base == IO_AREA_PADDR) {
        const PAddr phys_addr = area->paddr_base + offset_into_region;
        if (phys_addr >= GekkoPakNtr::PhysicalBase &&
            phys_addr - GekkoPakNtr::PhysicalBase + mapping.size <=
                GekkoPakNtr::RegisterPageSize) {
            GekkoPakNtr::Reset();
            // The VMA is carved as ordinary backing memory first so the VM
            // manager's bookkeeping is complete, then converted to MMIO. The
            // backing store is never read: every access is dispatched to the
            // device instead.
            auto vma = address_space
                           .MapBackingMemory(mapping.address,
                                             MemoryRef{GekkoPakNtr::GetRegisterMemory()},
                                             mapping.size, MemoryState::IO)
                           .Unwrap();
            address_space.Reprotect(
                vma, mapping.read_only ? VMAPermission::Read : VMAPermission::ReadWrite);
            address_space.MakeMMIO(vma);
            LOG_INFO(Loader, "Mapped GekkoPAK NTRCARD window at 0x{:08X} as MMIO",
                     mapping.address);
            return;
        }
        LOG_ERROR(Loader, "MMIO mappings are not supported yet. phys_addr=0x{:08X}", phys_addr);
        return;
    }
""",
    )

    proc = root / "src/core/hle/kernel/process.cpp"
    replace_once(
        proc,
        "        {0x1F000000, 0x600000, false}, // entire VRAM\n    };\n",
        "        {0x1F000000, 0x600000, false}, // entire VRAM\n"
        "        // GekkoPAK development mapping: NTRCARD register page (phys 0x10164000).\n"
        "        // Real retail user applications normally reach this hardware through privileged software.\n"
        "        {0x1EC64000, 0x1000, false},\n    };\n",
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("azahar", type=Path)
    args = ap.parse_args()
    root = args.azahar.resolve()
    here = Path(__file__).resolve().parent

    install_files(root, here)
    patch_mmio_page_type(root)
    patch_vm_manager(root)
    patch_kernel_mapping(root)

    print("Applied GekkoPAK NTRCARD overlay to", root)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("overlay failed:", e, file=sys.stderr)
        sys.exit(1)
