# GekkoPAK conformance architecture

Phase A implementation note: what the GekkoPAK protocol implementation looks
like today, what should become shared code, what must stay transport-specific,
and which compatibility risks the split has to respect.

This is a survey of the tree as it stood at the merge of `phase2-v1-azahar`
into `phase2-v1-dspico-hw`, written before any refactoring, so the reasoning
behind the shared core can be checked against the starting point.

## Where the protocol lives today

There is no single GekkoPAK protocol implementation. There are four, and three
of them are near-verbatim copies of each other.

| # | Implementation | File | Lines | Transport |
|---|---|---|---|---|
| 1 | Standalone host model | `prototype/azahar-ntr/model/gekkopak_ntr_model.{h,cpp}` | 550 | simulated NTR register page |
| 2 | Azahar core device | `prototype/azahar-ntr/azahar_overlay/src/core/hle/device/gekkopak_ntr.cpp` | 554 | real Azahar MMIO page |
| 3 | DSpico firmware overlay | `prototype/dspico-v1/overlay/src/gekkopakNtr.cpp` | 617 | RP2040 PIO / NTR bus |
| 4 | ARM guest client | `prototype/azahar-ntr/guest/gekkopak_guest_ntr.S` | 258 | client, not device |

`src/gekkopak.cpp` is a *fifth* thing and is often mistaken for the protocol
core. It is not: it is the analytic performance model (`Simulator::estimate`,
bandwidth/latency arithmetic, an allocator used only for accounting). It has no
wire format, no opcodes, no handles that cross a bus. It stays where it is.

The DS-side test client `prototype/dspico-v1/ds-test/source/gekkopak_ntr.c` is
a fifth *client*, parallel to the ARM guest.

### How much is duplicated

Implementations 1, 2 and 3 each independently define:

- the `F0`-`F5` opcode constants and the `47 4B` discriminator,
- the ten staging registers `Arg0..Out3`,
- the nine high-level commands `HELLO..COMPLETE`,
- the status enum,
- the 64-byte `GKD1` descriptor and 64-byte `GKC1` completion ABI,
- FNV-1a over uploaded bytes,
- the allocator, the job table and the completion queue,
- the modeled-time arithmetic (`25 us` command latency, `6 MiB/s`, `1e8 ops/s`),
- the `POLL` ready-after-two-polls rule,
- the `SUBMIT`/`POLL`/`COLLECT`/`FREE` bodies, statement for statement.

`ExecuteHighCommand` in (1) and (2) is the same function with `s.` prefixes
added. That is the whole problem: the deterministic behaviour these tests prove
is a property of three separately maintained copies, and nothing checks that
they agree.

### They have already drifted

Three divergences exist right now, and none of them is caught by any test.

1. **Capabilities.** Before the `phase2-v1-azahar` merge, the Azahar device
   reported `0x0000000F` while the host model and DSpico reported `0x0000001F`
   (`kCapBlockTransport` set). The ARM guest gates block transport on that bit,
   so the two branches disagreed about whether the emulated device supported
   `F4`/`F5` at all.

2. **`F5` word layout.** `docs/NTR_WIRE_V1.md` specifies
   `F5 47 4B QQ OO OO LL LL` — offset in the high half of the value word,
   length in the low half. Both the Azahar device and the DSpico firmware
   implement the opposite: `word = (length << 16) | offset`. Two independent
   implementations agree with each other and disagree with the specification,
   so the specification is what is wrong.

3. **Block-error reporting.** The host model's `WriteBlock` returns `bool` and
   leaves the staging registers untouched. The Azahar device sets
   `stage[Result]` to `BadBlock`/`BadDescriptor` and returns nothing. A client
   that checks `RESULT` after a failed `F4` gets a stale value from one and a
   real status from the other. DSpico adds a seventh status, `QueueFull`, that
   neither of the other two defines.

There is also a fourth divergence, at the transport layer rather than the
protocol layer, described under "wire byte order" below.

## What should become shared code

Everything that is a property of the *protocol* rather than of a *bus*:

- protocol version, capability flags, local-memory reporting;
- the wire command shape (`OP 47 4B II VV VV VV VV`) and its decode;
- the ten staging registers and the `F3` payload window;
- command sequencing and the high-level command bodies;
- the allocator and its handles;
- persistent device-local memory;
- job handles, `SUBMIT`/`POLL`/`COLLECT`/`FREE`;
- the `GKD1` descriptor and `GKC1` completion ABI, and batch parsing;
- the completion queue and the `F2` index `0xFE` event-depth read;
- `F4`/`F5` block semantics and their error statuses;
- `GK` discriminator validation;
- the status enum;
- the modeled-time arithmetic, so that "deterministic" means the same numbers
  everywhere.

The shared core must be usable inside RP2040 firmware, which sets hard
constraints on how it can be written:

- no dynamic allocation — fixed-capacity tables and a caller-supplied pool;
- no exceptions, no RTTI, no iostream;
- no `std::vector` / `std::unordered_map` / `std::deque`;
- **no floating point**, because the RP2040 has no FPU and because a shared
  core whose results depend on `double` rounding is not a conformance
  reference. The existing `double` arithmetic is re-expressed in integer
  nanoseconds, and the tests pin the results it has to keep producing.

## What must stay transport-specific

The adapters own the bus and nothing else:

| Adapter | Owns |
|---|---|
| Host model | the simulated register page, `ROMCNT` activate/data-ready bits, the `Tick()` pump |
| Azahar | `BackingMem` mapping into the guest address space, the MMIO hook, the per-slice `Tick()`, logging |
| DSpico | PIO command decode, `ntrc_beginRead`/`ntrc_beginWrite`/`ntrc_dmaToBus`, the 512-byte staging-buffer flip, IRQ-path budget |

Each adapter converts its native representation into
`(opcode, index, word)` + a data-phase pointer, and converts the core's
response back. That is the entire adapter contract.

### Keeping the IRQ path light

The DSpico split the firmware already uses is preserved: the `F4` completion
callback copies the staging buffer and marks it pending; descriptor parsing and
job execution happen off the cartridge IRQ path. The shared core therefore has
to expose block submission as a plain function over a buffer the caller already
holds, not as something that pulls bytes from a bus itself.

## Wire byte order: the fourth divergence

The 8-byte command header is `OP 47 4B II VV VV VV VV`. The first four bytes
are unambiguous. The value word is not.

`docs/HARDWARE_DSPICO_V1.md` settles the hardware answer from the PIO
configuration: `sm_config_set_in_shift(&c, false, true, 32)` shifts left, so
the first byte received lands in bits 31:24 and **the value word is
big-endian on the wire**. The DS-side test client matches this — it assembles a
big-endian `u64` and byte-swaps it into the register pair.

The Azahar path does not. The ARM guest stores the value word to `REG_CMD1`
with an ordinary little-endian `str`, and both the Azahar device and the host
model `memcpy` four bytes out of the register page as a native little-endian
`u32`. The three agree with each other, so every existing test passes — but a
guest written against the emulator would deliver a byte-swapped value word to
real DSpico hardware.

This is a genuine defect and not merely a representation choice, because the
guest is the reference for what a real GekkoCTR client must emit. The fix is to
move Azahar to the hardware order: byte-swap in the guest, decode big-endian in
the adapter. The host model covers the same decode path, so the change is
verifiable locally before CI ever builds Azahar.

`0xf269b734`, the 16-byte reference checksum, is unaffected — it is computed
over payload bytes, which are byte-transparent in both directions, and the
hardware run already confirmed it end to end on silicon.

## Compatibility risks

**The Azahar E2E cannot be run locally.** It needs a 45-minute Linux build of a
patched Azahar. Any change to the overlay, the guest or the frontend is
verified locally only through the host model, which shares the core but not the
transport. Mitigation: keep the three in lockstep by construction (one core,
thin adapters), keep guest changes minimal and mechanical, and treat the CI job
as the gate.

**The deterministic numbers are load-bearing.** `0xf269b734`, `2530`, `2738`,
`1278`, `1383` and the transfer counts `50`/`21`/`3` appear in tests, in
`docs/`, and in the hardware bring-up record. Re-expressing the timing
arithmetic in integers must reproduce them exactly; the conformance vectors pin
them so a regression fails loudly.

**Local-memory size legitimately differs.** The emulator reports 32 MiB; the
RP2040 has 64 KiB (`GEKKOPAK_LOCAL_BYTES`). That is a real device capability
difference, not a protocol divergence. The core takes the pool from the
adapter, the emulator keeps 32 MiB, and the conformance runner configures every
target with the same 64 KiB profile so responses are byte-identical.

**The overlays are copied, not linked.** Both `apply_overlay.py` scripts
`shutil.copy2` their sources into a foreign tree. The shared core has to be
copied alongside them and added to the same CMake lists, and the overlay
verifiers have to check it arrived.

**`F4`/`F5` in Azahar are not a real data phase.** The merged Azahar branch
carries block payloads through a staging window inside the register page
(`0x100`/`0x300`) rather than through the `ROMCNT` FIFO. The protocol semantics
above it are real; the 512-byte bus transfer underneath it is not. This is
recorded as an emulator fidelity gap rather than fixed here — see
`docs/CONFORMANCE_RESULTS.md`.

## Resulting shape

```
        GekkoCTR / ARM guest / DS test client
                        |
                     NTRCARD
                        |
        +---------------+---------------+
        |               |               |
   host adapter   Azahar adapter   DSpico adapter
        |               |               |
        +---------------+---------------+
                        |
              shared GekkoPAK core
        include/gekkopak/protocol.h
        include/gekkopak/device.h
        src/device.cpp
```

The core depends on `<cstdint>` and `<cstring>` and nothing else. It does not
know that Azahar, the Pico SDK, PIO, libretro or the CTR SDK exist.
