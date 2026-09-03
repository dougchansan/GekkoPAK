# GekkoPAK

**Experimental cartridge-side coprocessor architecture for accelerating GameCube static recompilation workloads on New Nintendo 2DS XL / New Nintendo 3DS hardware.**

GekkoPAK explores an enhancement-chip-style cartridge that works alongside a native CTR static-recomp runtime such as GekkoCTR. The cartridge is **not** intended to extend FCRAM directly. Instead, it provides local memory and asynchronous compute engines for workloads whose compute-to-transfer ratio makes cartridge offload worthwhile.

> Status: Phase 0 — virtual hardware and performance-model prototype.

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
docs/                    Architecture, protocol and roadmap
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

## Phase 0 success criteria

Phase 0 is successful when we can:

1. express accelerator work through a transport-independent API;
2. model TX/RX bandwidth and command latency;
3. allocate persistent accelerator-local memory;
4. estimate offload time versus a supplied ARM11 software time;
5. sweep hardware assumptions and identify the crossover point where offload wins.

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
