# Azahar NTRCARD GekkoPAK prototype

This is the second emulator prototype. Unlike the original libretro-memory bridge, the ARM guest now speaks through the 3DS NTRCARD register window:

- physical NTRCARD base: `0x10164000`
- ARM11 special-mapping virtual base: `0x1EC64000`
- `ROMCNT`: `+0x04`
- 8-byte command: `+0x08`
- FIFO: `+0x1C`

The guest retains the same high-level GekkoPAK transaction sequence:

`HELLO -> GET_CAPS -> ALLOC -> UPLOAD -> SUBMIT -> POLL -> COLLECT -> FREE -> COMPLETE`

## Wire protocol

The cartridge transport uses six otherwise unused DSpico game-mode opcodes:

- `F0 47 4B RR VV VV VV VV`: write staging register `RR`
- `F1 47 4B OP SS SS SS SS`: execute high-level command `OP`
- `F2 47 4B RR 00 00 00 00`: read staging register `RR` through FIFO
- `F3 47 4B II VV VV VV VV`: upload one 32-bit payload word at index `II`
- `F4 47 4B QQ 00 00 LL LL`: 512-byte console-to-cartridge block
- `F5 47 4B QQ LL LL OO OO`: 512-byte cartridge-to-console block

`47 4B` is the ASCII `GK` discriminator, and the value word is big-endian. F0-F3
remain as the bring-up and diagnostic path; F4/F5 are the steady-state
transport, and the guest uses them once `GET_CAPS` advertises bit 4.

The normative definition is `include/gekkopak/protocol.h`, and
`docs/NTR_WIRE_V1.md` is the prose version.

## What the Azahar overlay changes

Stock Azahar 2126.0 does not implement ARM11 IO-area special mappings, so a stock core reports `0x1EC64004` as unmapped. The overlay:

1. adds a 4 KiB register-backed GekkoPAK NTR device;
2. copies in the shared GekkoPAK device core, so the emulator and the DSpico
   firmware run the same protocol implementation rather than two copies of it;
3. maps physical `0x10164000` at virtual `0x1EC64000` for the development 3DSX/ELF process;
4. services the virtual cartridge at CPU-slice boundaries;
5. leaves the frontend out of command execution entirely.

The device file itself is a transport adapter: it owns the BackingMem, the tick
and the logging. Every protocol decision lives in `gekkopak::Device`.

The frontend may inspect ordinary guest RAM only after execution to validate the deterministic result summary. It never services NTR commands.

## Local model

```bash
./scripts/run_model.sh
```

Expected deterministic baseline:

```text
PASS offload=2530 us speedup=1.383x checksum=0xf269b734 wire_transfers=50
V1 SINGLE PASS transfers=3 modeled=2738 us speedup=1.278x checksum=0xf269b734
V1 BATCH8 PASS transfers=3 modeled/job=2530 us speedup=1.383x
```

The microsecond figures are modeled; the checksum is not. See
`docs/CONFORMANCE_RESULTS.md`.

## Golden vectors

The same protocol is checked against the DSpico firmware handlers and the
device core from the repository root:

```bash
cmake -S ../.. -B build && cmake --build build --target gekkopak_conformance
./build/gekkopak_conformance
```

## Block-transport fidelity caveat

F4/F5 payloads cross into the Azahar device through a staging window inside the
register page (`+0x100` and `+0x300`), not through the ROMCNT data FIFO. The
protocol semantics above it are exact; the 512-byte bus transfer underneath it
is not modelled at all. So this prototype can prove the descriptor ABI, the
queue behaviour and the responses, and cannot say anything about block-size
fields, latency settings or DMA.

## Patched Azahar run

After building an Azahar libretro core with `azahar_overlay/apply_overlay.py` applied:

```bash
export AZAHAR_CORE=/path/to/azahar_libretro.so
./scripts/run_patched_core.sh
```

A passing run ends with `NTR E2E PASS`.

## Real-hardware caveat

The direct ARM11 IO mapping is an **Azahar development facility**, not a claim that an ordinary retail 3DS user process can directly map NTRCARD registers. The physical implementation will need the appropriate CFW/kernel/ARM9/Process9-side broker or equivalent privileged transport. The guest-facing GekkoPAK API should remain above that transport boundary.
