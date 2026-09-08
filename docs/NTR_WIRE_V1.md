# GekkoPAK NTR wire protocol v1

Status: implemented. F0-F5 run on the host model, on the Azahar core device
and in DSpico RP2040 firmware, all three driving one shared device core, and
are pinned by the golden vectors in `tests/conformance/vectors/`. F4 is
confirmed on real DSpico hardware; F5 readback is not (see
`docs/CONFORMANCE_RESULTS.md`).

Where this document and the implementations disagreed, the implementations
won and the document was corrected. Those corrections are marked below.

## Why v1 is needed

The current emulator bring-up protocol intentionally favors visibility over efficiency. A tiny deterministic DSP transaction requires 50 NTR command/FIFO transfers from cold start, and the repeated SUBMIT -> POLL -> COLLECT portion is still roughly 21 transfers.

That is acceptable for proving the cartridge path, but not for a performance accelerator. Command latency must be amortized over useful work.

DSpico already demonstrates the hardware primitive needed to fix this: in unscrambled game mode it supports 512-byte console-to-cartridge payload writes and 512-byte cartridge-to-console payload reads. Its SD write path uses a two-buffer pipeline so the next 512-byte payload can be transferred while the previous block is being processed.

GekkoPAK v1 should reuse that transfer style rather than carrying bulk data four bytes at a time in command words.

## Design goals

- Keep the high-level GekkoPAK API unchanged.
- Preserve F0-F3 as a diagnostic / bootstrap transport.
- Add 512-byte block ingress and egress commands.
- Keep persistent buffers in cartridge-local memory.
- Allow multiple job descriptors to be submitted in one block.
- Avoid polling when possible; expose a compact completion/event queue.
- Make the same wire format usable by Azahar, DSpico/RP2350 firmware, and FPGA hardware.

## Proposed commands

All commands are issued in unscrambled NTR game mode and retain the `47 4B` (`GK`) discriminator.

### F4 - WRITE_BLOCK

```text
F4 47 4B QQ LL LL FF FF
```

Console-to-cartridge payload transfer.

- `QQ`: destination queue/buffer selector, in the command's index byte.
- `LLLL`: meaningful payload length, 1-512 bytes (bits 15:0 of the value word).
- `FFFF`: reserved. No flag bits are defined or honoured yet.
- Data phase: always a full 512 bytes, regardless of how many are meaningful.

The data phase is always 512 bytes because the NTR block-size field only
encodes 4 bytes or 512 and up. There is no 64- or 128-byte bus transfer, so a
"64-byte payload" is either sixteen 4-byte `F3` transactions or one 512-byte
`F4` with 64 meaningful bytes.

Selectors, as implemented:

- `0`: command/descriptor queue.

Any other selector returns `BadBlock`. Bulk upload staging buffers are not
implemented; inline input inside the descriptor block covers the cases so far.

### F5 - READ_BLOCK

```text
F5 47 4B QQ LL LL OO OO
```

Cartridge-to-console payload transfer.

- `QQ`: source queue/buffer selector.
- `LLLL`: requested valid length, 1-512 bytes (bits 31:16 of the value word).
- `OOOO`: source offset (bits 15:0). Only 0 is accepted today; a non-zero
  offset returns `BadBlock`.
- Data phase: a full 512 bytes, with only `LLLL` bytes considered valid. An
  empty queue returns a zeroed block rather than stale data.

**Corrected.** This document originally gave the two halves the other way
round. Both the Azahar device and the DSpico firmware were written to
`word = (length << 16) | offset`, they agree with each other, and they are what
ran on hardware -- so the specification was wrong, not the code. The layout is
now pinned by `EncodeReadBlockWord()` in `include/gekkopak/protocol.h` and by
the `f4_f5_single` golden vector.

Selectors, as implemented:

- `1`: completion/result queue.

Any other selector returns `BadBlock`. The bulk download buffers this document
originally reserved as selectors 1 and 2 do not exist; when they are added they
take selectors 2 and 3, because 1 is now the completion queue on hardware.

### F1 - KICK

The existing F1 execute opcode remains available for explicit queue kicks and debug commands. In normal v1 operation a descriptor-block write may optionally carry a `SUBMIT_ON_COMMIT` flag so a separate F1 transaction is unnecessary.

### F2 - READ_REG / EVENT

Retain the existing 4-byte response path for very small status values. This avoids reading a full 512-byte block just to learn whether a completion is available.

Index `0xFE` is the completion-queue depth. It is the compact event read: a
client polls it for four bytes and only spends a 512-byte `F5` once something
is actually waiting.

Indices `0xF0`-`0xF3` are a DSpico-local diagnostic window carrying the `F4`
instrumentation counters. They are firmware-specific, not part of the protocol,
and the emulator does not implement them.

## Descriptor block

A 512-byte command block should contain a small header followed by one or more fixed-size descriptors. A 64-byte descriptor gives eight jobs per NTR block and leaves enough room for versioning and future DMA/scatter fields.

Proposed descriptor fields:

```text
u32 magic              // 'GKD1' (0x31444B47), little-endian
u16 version
u16 opcode
u32 sequence
u32 flags
u32 input_handle
u32 input_offset
u32 input_length
u32 output_handle
u32 output_offset
u32 output_length
u32 kernel_id
u32 work_units
u32 arg0
u32 arg1
u32 arg2
u32 arg3
```

**Corrected.** The magic is `GKD1`, not the `GKJB` this document first
proposed; completions use `GKC1`. Both are 64 bytes, both are little-endian and
byte-transparent on the wire, and both are pinned by `static_assert` in
`include/gekkopak/protocol.h`.

`version` must be 1 and `opcode` must be 1 (submit); anything else is
`BadDescriptor`. `flags` bit 0 is `INLINE_INPUT`: the descriptor is followed
immediately in the block by `input_length` bytes of input data. `arg0` carries
the software reference time in microseconds, which is what the reported speedup
is measured against.

A descriptor whose `magic` is zero terminates the block, so a partially filled
512-byte block does not need a count field.

The exact ABI remains provisional until GekkoCTR workload traces tell us which
fields are actually useful.

## Persistent-memory flow

Large game data should normally cross the cartridge bus once:

```text
ALLOC local buffer
WRITE_BLOCK upload(s)
    ... buffer remains resident ...
WRITE_BLOCK job descriptor
KICK / implicit submit
READ_REG completion status
READ_BLOCK result only when result is larger than a few words
```

A repeated frame job should therefore not repeat ALLOC or asset uploads.

## Expected steady-state reduction

The current F0-F3 DSP sequence uses approximately 21 NTR transfers for SUBMIT + two POLLs + COLLECT.

A v1 steady-state path can target:

```text
1 x WRITE_BLOCK  descriptor + small dynamic input
0-1 x KICK       optional; may be implicit
1 x READ_REG     completion/event
0-1 x READ_BLOCK only for larger results
```

That reduces the normal critical path from about 21 command transactions to roughly 2-4 while simultaneously increasing useful bytes per transfer.

## Implementation status

1. F0-F3 Azahar NTR E2E test -- done.
2. F4/F5 data-phase semantics in the standalone NTR model -- done.
3. The same commands in the Azahar register-backed cartridge device -- done.
4. The ARM guest using 512-byte write/read transactions -- done.
5. Transfer count and modeled critical path against the F0-F3 baseline -- done:
   21 transfers to 3, and 530.1 us of modeled bus time to 238.4 us.
6. The same F4/F5 handlers in DSpico/RP2350 firmware -- done, and they are the
   same source file, not a port. All three transports drive one shared device
   core and are checked against each other by
   `tests/conformance/vectors/`.

What is not done is hardware validation of F5; see
`docs/CONFORMANCE_RESULTS.md`.

## Wire byte order

Settled against the DSpico PIO configuration rather than convention:

- `sm_config_set_in_shift(&c, false, true, 32)` shifts left, so the first byte
  received lands in bits 31:24. **The 8-byte command header is big-endian:**
  `OP 47 4B II VV VV VV VV`, value word MSB first.
- `sm_config_set_out_shift(&c, true, true, 32)` shifts right, so cartridge to
  console words go out LSB first and memory order equals wire order.
- `ntrCardIrq.S` applies `rev` to each received payload word before storing.

Net result: **the command header is big-endian; payloads are byte-transparent
in both directions.** `DecodeCommand()` and `EncodeCommand()` in
`include/gekkopak/protocol.h` are the only place this is expressed.

The 16-byte reference pattern

```text
44 33 22 11  88 77 66 55  DD CC BB AA  0D F0 AD 0B
```

hashes under FNV-1a to `0xf269b734` when laid into the block in written order,
and to `0x899bd1de` if word-swapped. Every conformance path asserts
`0xf269b734`, so a byte-order regression fails loudly rather than silently.

## Sequencing

`F1` carries a 32-bit sequence word. It is **advisory**: the device stores
nothing, rejects nothing, and enforces no ordering. Replaying a sequence
number, or sending one that goes backwards, is accepted on every target. The
`sequence_is_advisory` golden vector records this so it is a known contract
rather than an assumption.

If replay rejection or ordering enforcement is wanted, it is a wire-spec change
and needs a capability bit, because existing clients do not maintain a
monotonic counter across a reset.

## Handles

Allocation handles are slot indices, `slot + 1`, over a fixed table of 16. A
freed handle can therefore be reissued by the next `ALLOC`, and a stale handle
can alias a live allocation. Job handles are monotonic within a session and are
not reused.

This is deliberate -- it is what fits an RP2040 -- but it means a client must
not hold a handle across a `FREE`. Generation-tagged handles would fix it at
the cost of 8 bits of handle space.

## Hardware note

This protocol is intentionally shaped around operations already demonstrated by DSpico firmware: 512-byte payload receive (`ntrc_beginRead` / read-payload completion) and 512-byte DMA-to-bus responses. The final implementation should preserve double buffering so cartridge compute or local-memory work can overlap the next NTR transfer.
