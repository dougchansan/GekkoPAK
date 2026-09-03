# GekkoPAK

**Experimental cartridge-side coprocessor architecture for accelerating GameCube static recompilation workloads on New Nintendo 2DS XL / New Nintendo 3DS hardware.**

GekkoPAK explores an enhancement-chip-style cartridge that works alongside a native CTR static-recomp runtime such as GekkoCTR. The cartridge is **not** intended to extend FCRAM directly. Instead, it provides local memory and asynchronous compute engines for workloads whose compute-to-transfer ratio makes cartridge offload worthwhile.

> Status: **Phase 2 emulator/transport prototype.** The host simulator passes, a real ARMv6K guest passes the full GekkoPAK lifecycle inside Azahar, and the NTRCARD bring-up protocol passes through a genuinely mapped `0x1EC64000` register page. The source-patched Azahar core backend compiles successfully; its dedicated core-side NTR E2E runtime gate is being validated separately.

## Current milestones

### Host model

`libgekkopak` models:

- separate TX/RX bandwidth;
- command latency;
- accelerator throughput;
- persistent accelerator-local memory;
- job crossover versus a supplied ARM11 software time.

### Real ARM guest in Azahar

The ARM guest completes:

```text
HELLO -> GET_CAPS -> ALLOC -> UPLOAD -> SUBMIT
      -> POLL(not ready) -> POLL(ready)
      -> COLLECT -> FREE -> COMPLETE
```

Deterministic bring-up result:

```text
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

Those timing values are modeled assumptions, not physical-cartridge measurements.

### NTRCARD register transport

The current guest talks through the legacy gamecard register window rather than a high-level emulator mailbox:

```text
ARM guest
   |
   v
0x1EC64000 guest mapping
0x10164000 physical NTRCARD page
   |
   +-- ROMCNT
   +-- 8-byte command register
   +-- 32-bit FIFO
   |
   v
GekkoPAK device
```

The production ARM guest completes the cold-start sequence in **46 low-level wire transactions**. A stock-Azahar mapped-page development harness reproduces the complete deterministic PASS with ordinary guest CPU loads/stores and a clean shutdown.

See:

- [`docs/AZAHAR_E2E.md`](docs/AZAHAR_E2E.md)
- [`docs/AZAHAR_NTR_INJECTED_PASS.md`](docs/AZAHAR_NTR_INJECTED_PASS.md)
- [`prototype/azahar-ntr/README.md`](prototype/azahar-ntr/README.md)

### Batched NTR v1 target

The F0-F3 protocol is deliberately verbose for bring-up. The planned F4/F5 transport uses the same 512-byte data-phase primitives already demonstrated by DSpico.

Current modeled steady-state control path:

```text
F0-F3: 21 transactions
v1:     3 transactions
```

Under the provisional 6 MiB/s / 25 us command assumptions, the model reduces bus/control time from about **530 us to 238 us** despite moving full 512-byte blocks.

See [`docs/NTR_WIRE_V1.md`](docs/NTR_WIRE_V1.md).

## Goals

- Define a stable `libgekkopak` API before committing to physical hardware.
- Model cartridge transfer bandwidth, command latency, accelerator compute time, and persistent local memory.
- Identify GameCube workloads that benefit from offload rather than guessing from peak chip performance.
- Keep acceleration optional: GekkoCTR must remain functional without GekkoPAK.
- Progress from host simulation -> emulator integration -> DSpico transport -> RP2350 prototype -> FPGA accelerator.

## Non-goals

- GekkoPAK does not make cartridge RAM appear as normal 3DS FCRAM.
- It is not an instruction-by-instruction remote PowerPC interpreter.
- It should not require synchronous round trips for tiny operations.

## Why a coprocessor?

The cartridge interface is narrow compared with system memory. Successful offload therefore follows one rule:

> **Move computation to the cartridge, not memory traffic.**

Good candidate jobs have small commands/inputs, substantial computation, and small results, or operate repeatedly on data retained in GekkoPAK-local RAM.

Candidate accelerators include:

- GameCube DSP/audio workloads
- batched paired-single / matrix / animation kernels
- dynamic texture conversion and swizzling
- decompression and bulk format conversion
- profile-guided, game-specific hot kernels

## Repository layout

```text
include/gekkopak/        Public C++ API
src/                     Host reference implementation
sim/                     Virtual GekkoPAK CLI / experiments
prototype/azahar/        Initial Azahar ARM-guest mailbox prototype
prototype/azahar-ntr/    NTRCARD register/device prototype and source overlay
prototype/dspico/        DSpico hardware transport integration notes
docs/                    Architecture, protocols, emulator results and roadmap
```

## Build

Requires CMake 3.20+ and a C++17 compiler.

```bash
cmake -S . -B build
cmake --build build
./build/gekkopak_sim
```

On Windows with a multi-config generator:

```powershell
cmake -S . -B build
cmake --build build --config Release
.\build\Release\gekkopak_sim.exe
```

## Azahar prototypes

The emulator binary is not vendored.

Initial mailbox E2E:

```bash
export AZAHAR_CORE=/path/to/azahar_libretro.so
./prototype/azahar/run_e2e.sh
```

NTRCARD model and source-patched backend:

```bash
./prototype/azahar-ntr/scripts/run_model.sh
```

The dedicated `Azahar NTRCARD E2E` GitHub Actions workflow builds Azahar 2126.0 from its verified unified-source archive, applies the GekkoPAK core overlay, builds the ARM guest and validation frontend, and runs the NTR guest against the patched core.

## Phase status

- **Phase 0 — virtual hardware model:** complete for the initial timing/local-memory model.
- **Phase 1 — GekkoCTR workload integration:** planned; needs real recomp traces.
- **Phase 2 — Azahar virtual device:** active; ARM guest and mapped NTR transport pass, source-patched core runtime validation in progress.
- **Phase 2.1 — batched NTR transport:** design and transfer-budget regression started.
- **Phase 3 — real 2DS XL ↔ DSpico measurements:** next hardware gate.
- **Phase 4 — RP2350 extended cartridge:** gated on measured transport data.
- **Phase 5 — FPGA kernel:** gated on measured GekkoCTR workload wins.

See [`docs/ROADMAP.md`](docs/ROADMAP.md) and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Relationship to GekkoCTR

GekkoPAK is intended to be consumed by GekkoCTR as an optional backend. The recomp runtime should see a capability-oriented interface rather than DSpico-, FPGA-, or emulator-specific code.

```text
GameCube DOL
    |
 DolRecomp
    |
 GekkoCTR
    |
libgekkopak
    |
    +-- software / null backend
    +-- virtual host backend
    +-- Azahar backend
    +-- DSpico transport
    +-- physical GekkoPAK
```

## License

License selection is intentionally left open during the initial prototype bootstrap. Add a license before public distribution or accepting outside contributions.
