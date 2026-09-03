# DSpico GekkoPAK transport prototype

This directory is the hardware-side bridge between the Azahar NTRCARD prototype and a real DSpico/RP2040/RP2350 cartridge implementation.

No DSpico firmware is vendored here. The intent is to keep a small, reviewable patch surface that can be applied to the upstream `LNH-team/dspico-firmware` tree or to a GekkoPAK-specific firmware fork.

## Why the mapping is clean

DSpico's unscrambled game-mode IRQ dispatcher has unused command-table entries at `F0` through `F5` in both the command-word-0 and command-word-1 tables. Existing DSpico extensions occupy E3-E5 (SD read), E8-EB (USB), and F6 (512-byte SD write), so the current GekkoPAK range does not collide with them.

The emulator bring-up protocol therefore maps directly:

| Opcode | GekkoPAK use | DSpico data phase |
|---|---|---|
| F0 | write staging register | no payload; value is command word 1 |
| F1 | execute high-level command | no payload; sequence is command word 1 |
| F2 | read staging register | 4-byte DSpico -> DS response |
| F3 | write one payload word | no payload; value is command word 1 |
| F4 | v1 WRITE_BLOCK | 512-byte DS -> DSpico payload |
| F5 | v1 READ_BLOCK | 512-byte DSpico -> DS payload |

All GekkoPAK commands include bytes `47 4B` (`GK`) after the opcode so another F0-F5 command family can be rejected rather than interpreted as accelerator traffic.

## Upstream dispatcher changes

In `src/ntrCardIrq.S`, replace the F0-F5 dummy/unknown entries in `gGameNoScrambleCmd0HandlerTable` and `gGameNoScrambleCmd1HandlerTable` with GekkoPAK handlers.

Command-word-0 table:

```asm
.word ntrc_gekkopakWriteRegCmd0      // F0
.word ntrc_gekkopakExecCmd0          // F1
.word ntrc_gekkopakReadRegCmd0       // F2
.word ntrc_gekkopakPayloadWordCmd0   // F3
.word ntrc_gekkopakWriteBlockCmd0    // F4
.word ntrc_gekkopakReadBlockCmd0     // F5
```

Command-word-1 table:

```asm
.word ntrc_gekkopakWriteRegCmd1      // F0
.word ntrc_gekkopakExecCmd1          // F1
.word ntrc_gameNoScrambleCmd1Dummy4 // F2
.word ntrc_gekkopakPayloadWordCmd1   // F3
.word ntrc_gekkopakWriteBlockCmd1    // F4
.word ntrc_gekkopakReadBlockCmd1     // F5
```

Exact dummy choice for F2/F5 should be verified against the final data-phase implementation.

## F0/F1/F3 bring-up handlers

These commands need the complete 8-byte command, so command-word-0 should simply advance the NTR state without a payload. Command-word-1 then validates the `GK` discriminator and acts on `romEmu->cmd0` plus `word`/`romEmu->cmd1`.

The byte layout used by the ARM guest is:

```text
F0 47 4B RR VV VV VV VV   write staging register RR = value
F1 47 4B OP SS SS SS SS   execute high-level command OP, sequence SS
F2 47 4B RR 00 00 00 00   read staging register RR
F3 47 4B II VV VV VV VV   write payload word II = value
```

On the RP2040, `romEmu->cmd0` contains the first four bytes and `cmd1` the second four bytes. DSpico's assembly dispatcher already selects the handler from the high byte of `cmd0`.

## F2 4-byte response

DSpico already uses this pattern for status/read-ID commands:

```c
ntrc_beginWrite(pio, 4);
ntrc_writeWord(pio, result);
ntrc_finishGameNoScrambleCmd0(romEmu);
```

F2 can use the same primitive to expose a staging-register value through the normal NTR data FIFO.

## F4 block ingress

DSpico already has the required 512-byte console-to-cartridge mechanism. Its USB and SD write handlers use:

```c
ntrc_beginRead(pio, 512);
ntrc_finishGameNoScrambleCmd1WithReadPayload(
    romEmu, (u32*)buffer, 512, payloadCompleteCallback);
```

GekkoPAK should use two aligned 512-byte staging buffers and alternate between them. A completed F4 payload is either:

- copied into persistent accelerator-local memory; or
- interpreted as a descriptor block and queued for execution.

The callback must do as little work as possible on the cartridge IRQ-critical path. Expensive parsing/compute should be handed to the other MCU core or accelerator queue.

## F5 block egress

DSpico's SD/USB read paths demonstrate the inverse primitive:

```c
ntrc_beginWrite(pio, 512);
ntrc_dmaToBus(buffer, 512);
```

F5 should expose a completed-result block or bulk output buffer in the same way. For the first v1 implementation, make the data phase a fixed 512 bytes so the command-word-0 handler can start the transfer without waiting for additional length fields.

## MCU division of labor

Recommended RP2350 prototype split:

```text
core 0 / PIO IRQ
    NTR command decode
    FIFO/data-phase setup
    copy/flip 512-byte staging buffers
    enqueue lightweight work descriptors

core 1
    command queue processing
    local-memory allocator
    software accelerator reference kernels
    FPGA/DMA submission later
```

Do not execute large DSP/vector kernels in the PIO IRQ path.

## Device state shared with Azahar

The firmware should initially preserve the exact deterministic semantics already exercised by the Azahar model:

- protocol `0x00010000`
- capabilities `0x0000000f`
- 32 MiB modeled local-memory target (actual RP prototype may expose less and report it honestly)
- persistent allocation handles
- job handles
- SUBMIT/POLL/COLLECT/FREE
- same 16-byte test payload checksum `0xf269b734`

This gives us a byte-for-byte conformance test between emulator and hardware.

## Bring-up sequence

1. Add only F0-F3 handlers to a DSpico firmware fork.
2. Run the real-console transport test and reproduce the Azahar deterministic transaction.
3. Measure command round-trip latency and throughput rather than retaining emulator assumptions.
4. Add F4/F5 512-byte transfers.
5. Rerun the same job over block transport.
6. Replace modeled transport values in `libgekkopak` with measured hardware profiles.

See `docs/NTR_WIRE_V1.md` for the block protocol and `prototype/azahar-ntr/` for the emulator reference implementation.
