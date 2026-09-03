# Azahar end-to-end GekkoPAK prototype

## Status

**PASS on Azahar 2126.0 libretro, New 3DS mode.**

This prototype executes a real ARMv6K guest inside Azahar and bridges a GekkoPAK mailbox through the memory descriptors exposed by `RETRO_ENVIRONMENT_SET_MEMORY_MAPS`. The frontend is acting as the virtual cartridge/coprocessor; the ARM guest owns the protocol state machine.

The emulator binary is intentionally **not** stored in this repository. Point `AZAHAR_CORE` at a locally obtained `azahar_libretro.so`.

## Transaction path

```text
ARM guest in Azahar
      |
      | emulated RW process memory at 0x0FFFC000
      v
GekkoPAK mailbox
      |
      | libretro memory-map bridge
      v
virtual GekkoPAK device
      |
      +-- 32 MiB modeled local RAM
      +-- async job queue
      +-- provisional 6 MiB/s TX/RX model
      +-- provisional 25 us command latency
      +-- provisional 100 Mops/s compute model
```

The guest executes:

```text
HELLO -> GET_CAPS -> ALLOC -> UPLOAD -> SUBMIT
      -> POLL(not ready) -> POLL(ready)
      -> COLLECT -> FREE -> COMPLETE
```

## Verified baseline

The first deterministic run produced:

```text
status          : PASS
protocol        : 1.0
capabilities    : 0x0000000f
local RAM       : 32 MiB
allocation      : handle 1
job             : handle 1
modeled offload : 2530 us
software base   : 3500 us
speedup         : 1.383x
payload checksum: 0xf269b734
```

The uploaded 16-byte payload originates in guest ARM memory. The frontend copies it into accelerator-local memory and computes FNV-1a `0xf269b734`; the regression validator independently computes the same checksum. This proves guest-to-device data transfer rather than merely replaying a host-side command list.

The timing numbers remain simulated hardware assumptions. They are **not** measurements of a physical DSpico, RP2350, cartridge bus, or FPGA.

## Run

Requirements:

- Linux x86_64
- `gcc`
- `clang` with `arm-none-eabi` target support
- `ld.lld`
- Python 3
- Azahar 2126.0-compatible Linux x86_64 libretro core

```bash
export AZAHAR_CORE=/path/to/azahar_libretro.so
./prototype/azahar/run_e2e.sh
```

Artifacts land under `prototype/azahar/build/` by default.

## Why this matters

We now have a stable guest-facing ABI that can be exercised inside a real 3DS emulator before physical hardware exists. The next backend can replace the libretro memory-map shim with an Azahar virtual NTRCARD/cartridge device while keeping the ARM-side command sequence unchanged. The same contract can then be implemented by DSpico/RP2350 firmware and eventually the FPGA hardware.
