# An MMIO page type for Azahar

GekkoPAK's cartridge transport is access-driven. Reading the data FIFO is what
advances a transfer and clears `DATA_READY` — that is not a convention, it is
what the hardware does, and a client written against anything else will be
wrong on silicon.

Modelling it requires the emulator to observe an *individual* register access.
Azahar could not. This is the patch that lets it.

## What was missing

Azahar 2126.0 has no MMIO mechanism reachable from a normal mapping:

- `PageType` has five entries — `Unmapped`, `Memory`, `RasterizerCachedMemory`,
  `MemoryWatchpoint`, `RasterizerCachedMemoryWatchpoint`. Citra's `MMIORegion`
  and `MapIoRegion` were removed; `grep -r MMIORegion src/` finds nothing.
- `VMAType` has two entries: `Free` and `BackingMemory`.
- `KernelSystem::HandleSpecialMapping` refuses IO-area mappings outright with
  *"MMIO mappings are not supported yet"*.

There *is* an MMIO dispatch in `MemorySystem::Read`/`Write`, but it is reachable
only through the Luma3DS alias (virtual address bit 31 set) and is hardcoded to
`GPU().ReadReg`/`WriteReg`. A page mapped normally takes the pointer fast path
and the device never sees anything.

So before this patch the GekkoPAK window was ordinary RAM, and the transport had
to invent a per-word acknowledgement to work around not being able to see a
read.

## Shape of the solution

The memory core gains a *generic* capability — serve a page from a registered
device instead of from memory — and GekkoPAK registers itself. Azahar's own
sources name GekkoPAK in exactly one place, a single `Install()` call, and the
memory core and VM manager name it nowhere at all.

That split is enforced, not just intended: `verify_overlay.py` fails if a
GekkoPAK reference ever appears in the generic files.

## Why it is tractable

Two facts from the source make this a small change rather than a rewrite:

1. **A page with a null pointer already routes through the callbacks.**
   `MemorySystem::Read`/`Write` take the fast path only `if (page_pointer)`, and
   fall into the `PageType` switch otherwise. `MemoryWatchpoint` relies on
   exactly this: `MapPages` sets `pointers[base] = nullptr` and the switch
   serves the access from a side map.

2. **The dynarmic JIT needs no change at all.** `ARM_Dynarmic::MakeJit` passes
   `config.page_table = &current_page_table->GetPointerArray()`. Dynarmic uses
   that array as an optimisation and calls `MemoryRead32`/`MemoryWrite32` for
   any page whose entry is null. Those callbacks already funnel into
   `MemorySystem`. If this were not true, watchpoints would not work either.

So an MMIO page is just "a page type whose pointer is null and whose switch case
calls a device".

## The patch

Seven files, and none of the generic ones names GekkoPAK.

### Generic infrastructure — no device references

**`src/core/memory.h`**

- One new `PageType::MMIO`.
- `MMIOHandler` — three function pointers: `read32`, `write32`, `on_map`.
  Offsets are relative to the window base, so a device need not know where it
  was mapped.
- `MMIOWindow` — physical base, size, the virtual base filled in at map time,
  the handler, and a backing buffer.
- `RegisterMMIOWindow`, `FindMMIOWindowByPhys`, `MMIORead32`, `MMIOWrite32`.
- `MapMMIORegion` on `MemorySystem`.

**`src/core/memory.cpp`**

- The registry: a `std::vector<MMIOWindow>` behind a function-local static, so
  initialisation order is safe no matter when a device registers.
- `Read<T>` / `Write<T>`: `case PageType::MMIO:` → `MMIORead32` / `MMIOWrite32`.
- `ReadBlock` / `WriteBlock`: a word loop, because those switches end in
  `default: UNREACHABLE()` and a block copy across the window would otherwise
  abort. Nothing does this today; it is there so nothing can.
- `MapMMIORegion`: `MapPages(..., nullptr, PageType::MMIO)`. The null pointer is
  the whole mechanism.

Accesses are asserted 32-bit, matching the existing Luma MMIO path which carries
the same restriction.

**`src/core/hle/kernel/vm_manager.{h,cpp}`**

- One new `VMAType::MMIO`, one case in `UpdatePageTableForVMA`, and
  `MakeMMIO(VMAHandle)` to retype a VMA and refresh its pages.

This is the part that is easy to get wrong. Overwriting the page table behind
the VM manager's back works — until something re-derives it from the VMA. A
reprotect, a memory-state change or a merge all call `UpdatePageTableForVMA`,
which would silently turn the window back into RAM and leave the guest spinning
on a `DATA_READY` that never arrives. Giving the VMA a type keeps the two
consistent by construction.

**`src/core/hle/kernel/memory.cpp`**

The IO-area branch asks the registry whether any device claims the physical
address, and maps it if so. With no device registered this is a no-op and the
mapping is refused exactly as before the overlay.

### The one binding site

**`src/core/core.cpp`** — a single `GekkoPakNtr::Install();` in `System::Init`,
plus its include.

It has to be explicit rather than a static initialiser inside the device's own
translation unit: `citra_core` is a `STATIC` library, and the linker drops a TU
nothing references, taking its initialisers with it.

`Install()` builds an `MMIOHandler` from three captureless lambdas and registers
the window. Everything GekkoPAK-specific — the physical base, the register
semantics, the reset behaviour — lives in the two files the overlay *adds*,
not in the files it *patches*.

`verify_overlay.py` asserts this: it fails if the string `GekkoPak` ever appears
in `memory.h`, `memory.cpp`, `vm_manager.{h,cpp}` or `kernel/memory.cpp`.

## What it buys

- **No emulator-isms in the transport.** Reading the FIFO advances the transfer
  and clears `DATA_READY`, as on silicon. The guest acknowledges nothing.
- **The guest code is the code a real client would run.** It is no longer
  written around an emulator quirk, which matters because this guest is the
  reference for what a GekkoCTR client must emit.
- **Faults are visible.** A wrong ROMCNT block size, or a FIFO touched with no
  transfer open, is reported instead of silently reading RAM.
- **It is fast.** The old model needed one CPU slice per word, 128 per block. A
  transfer now completes as fast as the guest can issue loads and stores.
- **It is the hook every future fidelity feature needs** — per-access latency,
  the dropped-first-transaction-after-a-pause behaviour found on hardware, bus
  contention. None of those can be modelled without seeing individual accesses.

## What it does not buy

Timing. A transfer still completes as fast as the guest can issue instructions;
there are no card clocks and no latency settings. The hook now exists to add
them, but nothing is modelled yet.

## Risk

This is a larger patch surface into emulator internals than the rest of the
overlay, and it is anchored on exact source text in seven files. Azahar is pinned
in `deps.lock` for exactly this reason, and `verify_overlay.py` checks every
marker so a version bump fails in a second rather than forty minutes into a
compile.

The blast radius inside Azahar is small: no existing page type, VMA type or code
path changes behaviour. Every addition is a new enum value and a new switch case
that only the GekkoPAK window can reach.

## Verified

Built locally against pinned Azahar 2126.0 and run with the real ARMv6K guest:

```
Mapped MMIO window at 0x1EC64000 (phys 0x10164000)
GekkoPAK NTR virtual cartridge reset: protocol=0x00010000 caps=0x0000001F
...
NTR E2E PASS
payload checksum: 0xf269b734    modeled offload: 2738 us / 1.278x
steady transfers: 3             clean shutdown
```

No bus faults, and the traced command stream is the same 19 transactions the
staging-window and register-page models produced — the transport changed, the
protocol did not.
