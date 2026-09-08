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

Six files, and the largest hunk is four lines.

### `src/core/memory.h`

- One new `PageType::MMIO`.
- One declaration: `void MapMMIORegion(PageTable&, VAddr base, u32 size)`.

### `src/core/memory.cpp`

- Include the GekkoPAK device header.
- `Read<T>`: `case PageType::MMIO:` → `GekkoPakNtr::Read32(vaddr & CITRA_PAGE_MASK)`.
- `Write<T>`: `case PageType::MMIO:` → `GekkoPakNtr::Write32(...)`.
- `ReadBlock`/`WriteBlock`: a word loop, because those switches end in
  `default: UNREACHABLE()` and a block copy across the window would otherwise
  abort. Nothing in GekkoPAK does this; it is there so nothing can.
- `MapMMIORegion`: `MapPages(..., nullptr, PageType::MMIO)`. The null pointer is
  the whole mechanism.

Accesses are asserted to be 32-bit, matching the existing Luma MMIO path which
carries the same restriction (`ASSERT(sizeof(data) == sizeof(u32))`).

### `src/core/hle/kernel/vm_manager.{h,cpp}`

- One new `VMAType::MMIO`.
- One case in `UpdatePageTableForVMA`.
- `MakeMMIO(VMAHandle)`: retype a VMA and refresh its pages.

This is the part that is easy to get wrong. Overwriting the page table behind
the VM manager's back works — until something re-derives it from the VMA. A
reprotect, a memory-state change or a VMA merge all call
`UpdatePageTableForVMA`, which would silently turn the window back into RAM and
leave the guest spinning on a `DATA_READY` that never arrives. Giving the VMA a
type keeps the two consistent by construction.

### `src/core/hle/kernel/memory.cpp`

The IO-area branch maps the GekkoPAK window instead of refusing it. The VMA is
carved as backing memory first so the VM manager's bookkeeping is complete, then
converted with `MakeMMIO`.

The backing buffer comes from `GekkoPakNtr::GetRegisterMemory()` rather than
`memory.GetPhysicalRef(phys_addr)` — there is no physical memory behind an IO
address, and asking for it trips an assertion. That buffer is never served to
the guest; it doubles as the device's own register storage.

### `src/core/core.cpp`

Nothing. The previous overlay hooked `System::RunLoop` to service the device
once per CPU slice; an access-driven device needs no servicing, so that hook is
gone and `core.cpp` is untouched.

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
overlay, and it is anchored on exact source text in six files. Azahar is pinned
in `deps.lock` for exactly this reason, and `verify_overlay.py` checks every
marker so a version bump fails in a second rather than forty minutes into a
compile.

The blast radius inside Azahar is small: no existing page type, VMA type or code
path changes behaviour. Every addition is a new enum value and a new switch case
that only the GekkoPAK window can reach.

## Verified

Built locally against pinned Azahar 2126.0 and run with the real ARMv6K guest:

```
Mapped GekkoPAK NTRCARD window at 0x1EC64000 as MMIO
...
NTR E2E PASS
payload checksum: 0xf269b734    modeled offload: 2738 us / 1.278x
steady transfers: 3             clean shutdown
```

No bus faults, and the traced command stream is the same 19 transactions the
staging-window and register-page models produced — the transport changed, the
protocol did not.
