# GekkoPAK DSpico F0-F5 overlay

This directory is the physical-transport bridge for GekkoPAK Protocol v1. It
overlays a pinned revision of the upstream `LNH-team/dspico-firmware` tree
instead of maintaining a hard fork. The pin lives in `deps.lock`; the current
one is `472c9d8e9957ad18df367f14b9cc337b9b887e65`, the revision flashed to real
hardware in `docs/HARDWARE_DSPICO_V1.md`.

`overlay/src/gekkopakNtr.cpp` is a transport adapter, not an implementation of
the protocol. It owns PIO decode, the data phases and the staging buffers; the
state machine is the shared `gekkopak::Device` that the emulator also runs, and
`apply_overlay.py` copies that core into the firmware tree alongside it.

## What it installs

The overlay claims the currently-unused unscrambled-game command slots F0-F5:

| Opcode | GekkoPAK meaning | DSpico data phase |
|---|---|---|
| F0 | WRITE_REG | none |
| F1 | EXEC | none |
| F2 | READ_REG / EVENT | 4 B cart -> console |
| F3 | PAYLOAD_WORD bootstrap | none |
| F4 | WRITE_BLOCK | fixed 512 B console -> cart |
| F5 | READ_BLOCK | fixed 512 B cart -> console |

F6 remains DSpico's existing SD-write command. E3-E5 and E8-EB remain untouched.

The payload ABI does not merely match the emulator model -- it is the same
source file:

- `GKD1` submission descriptor: 64 bytes
- `GKC1` completion record: 64 bytes
- eight descriptors fit one 512-byte F4 data phase
- eight completions fit one 512-byte F5 data phase
- F2 index `0xFE` returns completion queue depth

The default shim reserves 64 KiB of RP2040 SRAM for GekkoPAK-local bring-up memory. Override `GEKKOPAK_LOCAL_BYTES` at compile time for another target/profile. This SRAM pool is only a transport/protocol prototype; later RP2350/PSRAM/FPGA backends should replace it without changing the descriptor/completion ABI.

## Apply to upstream

```bash
git clone https://github.com/LNH-team/dspico-firmware.git
cd dspico-firmware
git checkout 472c9d8e9957ad18df367f14b9cc337b9b887e65
cd ..
python3 GekkoPAK/prototype/dspico-v1/apply_overlay.py dspico-firmware
python3 GekkoPAK/prototype/dspico-v1/verify_overlay.py dspico-firmware
```

## Running the firmware handlers without hardware

`hostshim/` compiles `overlay/src/gekkopakNtr.cpp` -- unmodified, the same file
the RP2040 builds -- against a stand-in for the DSpico headers, so the golden
wire vectors can be replayed through the real handler code on any host:

```bash
cmake -S ../.. -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target gekkopak_conformance
./build/gekkopak_conformance      # dspico-shim is one of the three targets
```

The shim models the cmd0/cmd1 dispatch, the PIO FIFO directive protocol, the
read-payload callback, the DMA response, and the wire byte order the PIO shift
directions imply. It also checks that a handler supplies exactly as many bytes
as its directive declared: on hardware the console clocks out exactly what the
directive promised, so a short handler leaves it reading undriven data and a
long one strands words in the FIFO for the next transaction, and either way the
payload arrives shifted rather than failing cleanly.

It does not model timing, the IRQ, scrambling, or the
dropped-first-transaction-after-a-pause behaviour; those are properties of the
silicon and still need a console.

Then build DSpico normally with the Pico SDK.

## Wire byte order: keep this boundary explicit

DSpico's unscrambled IRQ dispatch reads the opcode from `cmd0 >> 24`, so the canonical command header is described as bytes, for example:

```text
F0 47 4B RR VV VV VV VV
```

Do **not** make the GekkoPAK descriptor ABI depend on how a 3DS-side CARD register writer packs those eight command bytes into CPU words. The host transport should own that serialization. The F4/F5 512-byte payload ABI is little-endian data structures as produced by the ARM guest; DSpico's existing payload receive path performs the bus-word byte reversal before storing into the destination buffer.

The first hardware bring-up should therefore be intentionally small:

1. switch DSpico to unscrambled game mode;
2. issue only F1/F2 `HELLO`/register reads;
3. verify protocol/capability values;
4. issue F4 with one `GKD1` descriptor and 16 inline bytes;
5. read F2 event depth;
6. read F5 and verify `GKC1` + checksum;
7. only then enable batching and performance measurements.

## Timing

The shim currently returns the same provisional compatibility model used by Azahar (6 MiB/s, 25 us command latency, 100 M work-units/s). These are **not physical measurements**. Real 2DS <-> DSpico measurements should replace the profile after the first hardware E2E.

For the current v1 model:

- single job: 3 NTR transactions, 238.396 us modeled transport, 2738 us total, 1.278x vs 3500 us
- batch 8: 3 NTR transactions total = 0.375 transaction/job, about 29.800 us modeled transport/job, 2530 us/job, 1.383x
