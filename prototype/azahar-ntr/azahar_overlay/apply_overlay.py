#!/usr/bin/env python3
"""Apply the GekkoPAK NTRCARD overlay to an Azahar source tree.

Three things get installed:

1. The GekkoPAK device and the shared protocol core, as new files.
2. A device-agnostic MMIO page type in Azahar's memory core.
3. One line binding the two together.

(2) needs explaining, because it is the only part that touches emulator
internals. GekkoPAK's cartridge transport is access-driven: reading the data
FIFO is what advances a transfer and clears DATA_READY, exactly as on hardware.
Modelling that requires the emulator to observe an individual register access.
Azahar cannot -- Citra's MMIORegion was removed and PageType has no MMIO entry,
so a mapped page is simply memory and the device never sees a read.

The patch adds one PageType, one VMAType, a small handler registry, four switch
cases and two mapping entry points. **None of it names GekkoPAK.** The memory
core gains the ability to serve a page from a device; which device, and where,
is decided by whoever registers a window. Before any device registers, an
IO-area mapping is refused exactly as it was before the overlay.

The dynarmic JIT needs no change at all: it already falls back to the memory
callbacks for any page whose pointer is null, which is how MemoryWatchpoint
works today.

(3) is a single `GekkoPakNtr::Install()` in System::Init. It has to be explicit
rather than a static initialiser, because citra_core is a static library and the
linker drops a translation unit nothing references.

See docs/AZAHAR_MMIO.md.
"""

import argparse
from pathlib import Path
import shutil
import sys

# ---------------------------------------------------------------------------
# Long anchors and replacements, kept out of the functions for readability.
# ---------------------------------------------------------------------------

MMIO_REGISTRY = '''namespace {
// A function-local static keeps initialisation order safe no matter when a
// device registers relative to this translation unit.
std::vector<MMIOWindow>& MMIOWindows() {
    static std::vector<MMIOWindow> windows;
    return windows;
}
constexpr std::size_t MAX_MMIO_WINDOWS = 8;
} // namespace

bool RegisterMMIOWindow(PAddr phys_base, u32 size, MMIOHandler handler,
                        std::shared_ptr<BackingMem> backing) {
    auto& windows = MMIOWindows();
    for (auto& window : windows) {
        if (window.phys_base == phys_base) {
            window.size = size;
            window.handler = handler;
            window.backing = std::move(backing);
            return true;
        }
    }
    if (windows.size() >= MAX_MMIO_WINDOWS) {
        return false;
    }
    windows.push_back(MMIOWindow{phys_base, size, 0, handler, std::move(backing)});
    return true;
}

MMIOWindow* FindMMIOWindowByPhys(PAddr phys_base, u32 size) {
    for (auto& window : MMIOWindows()) {
        if (phys_base >= window.phys_base && phys_base + size <= window.phys_base + window.size) {
            return &window;
        }
    }
    return nullptr;
}

static MMIOWindow* FindMMIOWindowByVAddr(VAddr vaddr) {
    for (auto& window : MMIOWindows()) {
        if (window.vaddr_base != 0 && vaddr >= window.vaddr_base &&
            vaddr < window.vaddr_base + window.size) {
            return &window;
        }
    }
    return nullptr;
}

u32 MMIORead32(VAddr vaddr) {
    MMIOWindow* window = FindMMIOWindowByVAddr(vaddr);
    if (window == nullptr || window->handler.read32 == nullptr) {
        LOG_ERROR(HW_Memory, "MMIO read with no registered handler @ 0x{:08X}", vaddr);
        return 0;
    }
    return window->handler.read32(vaddr - window->vaddr_base);
}

void MMIOWrite32(VAddr vaddr, u32 value) {
    MMIOWindow* window = FindMMIOWindowByVAddr(vaddr);
    if (window == nullptr || window->handler.write32 == nullptr) {
        LOG_ERROR(HW_Memory, "MMIO write with no registered handler @ 0x{:08X}", vaddr);
        return;
    }
    window->handler.write32(vaddr - window->vaddr_base, value);
}

'''

PAGETYPE_ANCHOR = "    RasterizerCachedMemoryWatchpoint,\n};"

PAGETYPE_PATCHED = '''    RasterizerCachedMemoryWatchpoint,
    /// Page is a memory-mapped IO window. It has no backing pointer, so every
    /// access is dispatched to a registered device instead of being served from
    /// memory.
    MMIO,
};

/// Handlers for a device-served MMIO window. Offsets are relative to the window
/// base, so a device need not know where it was mapped.
struct MMIOHandler {
    u32 (*read32)(u32 offset) = nullptr;
    void (*write32)(u32 offset, u32 value) = nullptr;
    /// Called when the window is mapped into a process, so the device can return
    /// itself to a powered-on state.
    void (*on_map)() = nullptr;
};

struct MMIOWindow {
    PAddr phys_base = 0;
    u32 size = 0;
    /// Filled in when the window is mapped; zero until then. This is the key
    /// address dispatch looks up.
    VAddr vaddr_base = 0;
    MMIOHandler handler{};
    /// Exists only so the VM manager can carve a VMA: there is no physical
    /// memory behind an IO address. Never served to the guest.
    std::shared_ptr<BackingMem> backing;
};

/// Registers an MMIO window. Re-registering the same physical base replaces it.
/// Returns false only if the table is full.
bool RegisterMMIOWindow(PAddr phys_base, u32 size, MMIOHandler handler,
                        std::shared_ptr<BackingMem> backing);
/// Finds the registered window covering a physical range, or nullptr.
MMIOWindow* FindMMIOWindowByPhys(PAddr phys_base, u32 size);
/// Dispatches a 32-bit access to whichever window contains the address.
u32 MMIORead32(VAddr vaddr);
void MMIOWrite32(VAddr vaddr, u32 value);'''

READ_ANCHOR = '''    case PageType::MemoryWatchpoint: {
        auto it = page_table->watchpoint_pages_map.find(vaddr >> CITRA_PAGE_BITS);
        ASSERT_MSG(it != page_table->watchpoint_pages_map.end(),
                   "Missing memory for watchpoint page");

        ReadType value;'''

READ_PATCHED = '''    case PageType::MMIO: {
        // Strictly 32-bit, like every other MMIO access on this bus.
        ASSERT_MSG(read_size == sizeof(u32), "non-32-bit MMIO read @ {:08X}", vaddr);
        return static_cast<ReadType>(MMIORead32(vaddr));
    }
''' + READ_ANCHOR

WRITE_ANCHOR = '''    case PageType::MemoryWatchpoint: {
        auto it = page_table->watchpoint_pages_map.find(vaddr >> CITRA_PAGE_BITS);
        ASSERT_MSG(it != page_table->watchpoint_pages_map.end(),
                   "Missing memory for watchpoint page");

        std::memcpy(it->second.memory.GetPtr() + (vaddr & CITRA_PAGE_MASK), &data, sizeof(T));'''

WRITE_PATCHED = '''    case PageType::MMIO: {
        ASSERT_MSG(sizeof(T) == sizeof(u32), "non-32-bit MMIO write @ {:08X}", vaddr);
        MMIOWrite32(vaddr, static_cast<u32>(data));
        return;
    }
''' + WRITE_ANCHOR

READ_BLOCK_ANCHOR = '''                std::memcpy(dest_buffer, GetPointerForRasterizerCache(current_vaddr), copy_amount);
                break;
            }
            default:
                UNREACHABLE();'''

READ_BLOCK_PATCHED = '''                std::memcpy(dest_buffer, GetPointerForRasterizerCache(current_vaddr), copy_amount);
                break;
            }
            case PageType::MMIO: {
                u8* out = static_cast<u8*>(dest_buffer);
                for (std::size_t i = 0; i + sizeof(u32) <= copy_amount; i += sizeof(u32)) {
                    const u32 word = MMIORead32(static_cast<VAddr>(current_vaddr + i));
                    std::memcpy(out + i, &word, sizeof(word));
                }
                break;
            }
            default:
                UNREACHABLE();'''

WRITE_BLOCK_ANCHOR = '''                std::memcpy(GetPointerForRasterizerCache(current_vaddr), src_buffer, copy_amount);
                break;
            }
            default:
                UNREACHABLE();'''

WRITE_BLOCK_PATCHED = '''                std::memcpy(GetPointerForRasterizerCache(current_vaddr), src_buffer, copy_amount);
                break;
            }
            case PageType::MMIO: {
                const u8* in = static_cast<const u8*>(src_buffer);
                for (std::size_t i = 0; i + sizeof(u32) <= copy_amount; i += sizeof(u32)) {
                    u32 word = 0;
                    std::memcpy(&word, in + i, sizeof(word));
                    MMIOWrite32(static_cast<VAddr>(current_vaddr + i), word);
                }
                break;
            }
            default:
                UNREACHABLE();'''

MAP_MMIO_REGION = '''void MemorySystem::MapMMIORegion(PageTable& page_table, VAddr base, u32 size) {
    ASSERT_MSG((size & CITRA_PAGE_MASK) == 0, "non-page aligned size: {:08X}", size);
    ASSERT_MSG((base & CITRA_PAGE_MASK) == 0, "non-page aligned base: {:08X}", base);
    // A null backing pointer is what makes both the interpreter and the
    // dynarmic JIT route every access through Read/Write instead of a memcpy.
    MapPages(page_table, base / CITRA_PAGE_SIZE, size / CITRA_PAGE_SIZE, nullptr, PageType::MMIO);
}

'''

KERNEL_IO_ANCHOR = '''    if (area->paddr_base == IO_AREA_PADDR) {
        LOG_ERROR(Loader, "MMIO mappings are not supported yet. phys_addr=0x{:08X}",
                  area->paddr_base + offset_into_region);
        return;
    }
'''

KERNEL_IO_PATCHED = '''    if (area->paddr_base == IO_AREA_PADDR) {
        const PAddr phys_addr = area->paddr_base + offset_into_region;
        // Ask the MMIO registry whether any device claims this window. With no
        // device registered this is a no-op and the mapping is refused exactly
        // as it was before.
        if (auto* window = Memory::FindMMIOWindowByPhys(phys_addr, mapping.size)) {
            if (window->handler.on_map != nullptr) {
                window->handler.on_map();
            }
            window->vaddr_base = mapping.address;
            // The VMA is carved as ordinary backing memory first so the VM
            // manager's bookkeeping is complete, then converted to MMIO. The
            // backing store is never read: every access is dispatched instead.
            auto vma = address_space
                           .MapBackingMemory(mapping.address, MemoryRef{window->backing},
                                             mapping.size, MemoryState::IO)
                           .Unwrap();
            address_space.Reprotect(
                vma, mapping.read_only ? VMAPermission::Read : VMAPermission::ReadWrite);
            address_space.MakeMMIO(vma);
            LOG_INFO(Loader, "Mapped MMIO window at 0x{:08X} (phys 0x{:08X})", mapping.address,
                     window->phys_base);
            return;
        }
        LOG_ERROR(Loader, "MMIO mappings are not supported yet. phys_addr=0x{:08X}", phys_addr);
        return;
    }
'''


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
    """Add a device-agnostic MMIO page type and handler registry.

    Nothing in this function's output mentions GekkoPAK.
    """
    mem_h = root / "src/core/memory.h"
    replace_once(mem_h, PAGETYPE_ANCHOR, PAGETYPE_PATCHED)
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
    read_fn = ("template <typename T>\n"
               "T MemorySystem::Read(const std::shared_ptr<PageTable>& page_table, "
               "const VAddr vaddr) {")
    replace_once(mem_cpp, read_fn, MMIO_REGISTRY + read_fn)
    replace_once(mem_cpp, READ_ANCHOR, READ_PATCHED)
    replace_once(mem_cpp, WRITE_ANCHOR, WRITE_PATCHED)
    replace_once(mem_cpp, READ_BLOCK_ANCHOR, READ_BLOCK_PATCHED)
    replace_once(mem_cpp, WRITE_BLOCK_ANCHOR, WRITE_BLOCK_PATCHED)
    replace_once(
        mem_cpp,
        "void MemorySystem::UnmapRegion(PageTable& page_table, VAddr base, u32 size) {",
        MAP_MMIO_REGION + "void MemorySystem::UnmapRegion(PageTable& page_table, VAddr base, u32 size) {",
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
        "    case VMAType::BackingMemory:\n"
        "        memory.MapMemoryRegion(*page_table, vma.base, vma.size, vma.backing_memory);\n"
        "        break;\n    }",
        "    case VMAType::BackingMemory:\n"
        "        memory.MapMemoryRegion(*page_table, vma.base, vma.size, vma.backing_memory);\n"
        "        break;\n"
        "    case VMAType::MMIO:\n"
        "        memory.MapMMIORegion(*page_table, vma.base, vma.size);\n"
        "        break;\n    }",
    )
    replace_once(
        vm_cpp,
        "void VMManager::UpdatePageTableForVMA(const VirtualMemoryArea& vma) {",
        "void VMManager::MakeMMIO(VMAHandle vma) {\n"
        "    VirtualMemoryArea& area = vma_map[vma->first];\n"
        "    area.type = VMAType::MMIO;\n"
        "    UpdatePageTableForVMA(area);\n"
        "}\n\n"
        "void VMManager::UpdatePageTableForVMA(const VirtualMemoryArea& vma) {",
    )


def patch_kernel_mapping(root: Path):
    """Map a claimed IO window instead of refusing it.

    Device-agnostic: the branch asks the registry whether anything claims this
    physical address.
    """
    replace_once(root / "src/core/hle/kernel/memory.cpp", KERNEL_IO_ANCHOR, KERNEL_IO_PATCHED)

    replace_once(
        root / "src/core/hle/kernel/process.cpp",
        "        {0x1F000000, 0x600000, false}, // entire VRAM\n    };\n",
        "        {0x1F000000, 0x600000, false}, // entire VRAM\n"
        "        // GekkoPAK development mapping: NTRCARD register page (phys 0x10164000).\n"
        "        // Real retail user applications normally reach this hardware through privileged software.\n"
        "        {0x1EC64000, 0x1000, false},\n    };\n",
    )


def patch_core_init(root: Path):
    """The one place Azahar's own sources name GekkoPAK.

    citra_core is a static library, so a translation unit nothing references is
    dropped by the linker along with any static initialiser inside it. The
    device therefore has to be installed explicitly.
    """
    core = root / "src/core/core.cpp"
    replace_once(
        core,
        '#include "core/hle/kernel/process.h"\n',
        '#include "core/hle/device/gekkopak_ntr.h"\n#include "core/hle/kernel/process.h"\n',
    )
    replace_once(
        core,
        "    memory = std::make_unique<Memory::MemorySystem>(*this);\n",
        "    memory = std::make_unique<Memory::MemorySystem>(*this);\n\n"
        "    // Claim the NTRCARD window. Until this runs the memory core has no MMIO\n"
        "    // windows registered and an IO-area mapping is refused exactly as before.\n"
        "    GekkoPakNtr::Install();\n",
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
    patch_core_init(root)

    print("Applied GekkoPAK NTRCARD overlay to", root)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("overlay failed:", e, file=sys.stderr)
        sys.exit(1)
