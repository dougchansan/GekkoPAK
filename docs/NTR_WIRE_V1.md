# GekkoPAK NTR wire protocol v1

Status: design target after the F0-F3 bring-up transport is proven end-to-end.

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

- `QQ`: destination queue/buffer selector.
- `LLLL`: valid payload length, 0-512 bytes.
- `FFFF`: flags / sequence bits.
- Data phase: up to 512 bytes written by the console to the cartridge.

Initial selectors:

- `0`: command/descriptor queue.
- `1`: bulk upload staging buffer A.
- `2`: bulk upload staging buffer B.

### F5 - READ_BLOCK

```text
F5 47 4B QQ OO OO LL LL
```

Cartridge-to-console payload transfer.

- `QQ`: source queue/buffer selector.
- `OOOO`: source offset or result slot.
- `LLLL`: requested valid length, 0-512 bytes.
- Data phase: a 512-byte cartridge response, with only `LLLL` bytes considered valid.

Initial selectors:

- `0`: completion/result queue.
- `1`: bulk download buffer A.
- `2`: bulk download buffer B.

### F1 - KICK

The existing F1 execute opcode remains available for explicit queue kicks and debug commands. In normal v1 operation a descriptor-block write may optionally carry a `SUBMIT_ON_COMMIT` flag so a separate F1 transaction is unnecessary.

### F2 - READ_REG / EVENT

Retain the existing 4-byte response path for very small status values. This avoids reading a full 512-byte block just to learn whether a completion is available.

## Descriptor block

A 512-byte command block should contain a small header followed by one or more fixed-size descriptors. A 64-byte descriptor gives eight jobs per NTR block and leaves enough room for versioning and future DMA/scatter fields.

Proposed descriptor fields:

```text
u32 magic              // 'GKJB'
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

The exact ABI remains provisional until GekkoCTR workload traces tell us which fields are actually useful.

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

## Emulator implementation plan

1. Finish the current F0-F3 Azahar NTR E2E test.
2. Add F4/F5 data-phase semantics to the standalone NTR model.
3. Add the same commands to the Azahar register-backed cartridge device.
4. Extend the ARM guest with 512-byte write/read transactions.
5. Compare transfer count and modeled critical-path time against the F0-F3 baseline.
6. Port the exact F4/F5 handlers to DSpico/RP2350 firmware.

## Hardware note

This protocol is intentionally shaped around operations already demonstrated by DSpico firmware: 512-byte payload receive (`ntrc_beginRead` / read-payload completion) and 512-byte DMA-to-bus responses. The final implementation should preserve double buffering so cartridge compute or local-memory work can overlap the next NTR transfer.
