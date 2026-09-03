# GekkoPAK Architecture

## 1. Concept

GekkoPAK is an optional cartridge-side accelerator for GameCube static recompilation on New Nintendo 2DS XL / New Nintendo 3DS-class hardware.

It follows the enhancement-chip model used by historical cartridges: the host console remains responsible for the game and platform runtime, while sufficiently coarse compute jobs can be submitted to a coprocessor with its own local memory.

GekkoPAK is **not** system RAM expansion. Accelerator RAM is private to GekkoPAK and is accessed through explicit upload/download/compute operations.

## 2. Design rule

The cartridge interface is expected to be much slower than the host's internal memory system. The architecture therefore optimizes for **compute intensity**, not remote memory bandwidth.

An offload is attractive when:

```text
T_command + T_TX + T_compute + T_RX < T_ARM11_software
```

with:

```text
T_TX = bytes_TX / cartridge_TX_rate
T_RX = bytes_RX / cartridge_RX_rate
```

Persistent GekkoPAK buffers can reduce `bytes_TX` and `bytes_RX` on later jobs.

## 3. Logical stack

```text
GameCube executable
        |
   static recomp
        |
     GekkoCTR
        |
   libgekkopak
        |
  transport interface
        |
+-------+------------+----------------+
|                    |                |
Host simulator    Azahar          real CTR
                                      |
                                  cart transport
                                      |
                            DSpico / RP2350 PHY
                                      |
                               accelerator core
                                      |
                              FPGA + local RAM
```

`libgekkopak` owns capabilities, buffers, jobs, synchronization primitives, errors, and profiling. It must not expose FPGA registers or DSpico-specific packets to the recomp runtime.

## 4. Backends

### 4.1 Null/software backend

Purpose:

- portability;
- correctness reference;
- fallback when no cartridge is installed;
- A/B comparisons against accelerated paths.

### 4.2 Host virtual backend

The Phase 0 simulator is intentionally deterministic. It models:

- command latency;
- host-to-device bandwidth;
- device-to-host bandwidth;
- nominal accelerator operations per second;
- accelerator-local RAM capacity.

Later revisions should add queue depth, overlap, DMA setup cost, kernel-specific throughput and contention.

### 4.3 Azahar virtual device

The emulator backend should expose the same GekkoPAK protocol/capabilities as hardware while deliberately applying modeled timing constraints. Host CPU speed must not accidentally make the virtual accelerator appear infinitely fast.

Two useful Azahar modes are planned:

1. **functional mode** — execute kernels immediately for correctness testing;
2. **timed mode** — delay completion according to a selected virtual hardware profile.

### 4.4 DSpico transport prototype

The first real-console milestone should use an existing DSpico-class cartridge only as a transport/command endpoint. The goal is to measure the actual path from native CTR software through the cartridge interface.

Required measurements:

- minimum command round-trip latency;
- sustained host->cart bandwidth by block size;
- sustained cart->host bandwidth by block size;
- bidirectional command pattern performance;
- CPU cost of transfers on the 2DS XL;
- ability to overlap transfer/device work with useful ARM11 work.

The measured numbers replace simulator assumptions.

### 4.5 RP2350 prototype

A later extended-cartridge prototype can use RP2350 as:

- cartridge protocol engine;
- DMA/FIFO manager;
- command scheduler;
- firmware/debug endpoint;
- bridge to local RAM and/or FPGA.

The MCU is not assumed to be the final high-performance accelerator. It gives us a programmable hardware transport target before FPGA complexity is introduced.

### 4.6 FPGA accelerator

The FPGA should execute **coarse kernels**, not individual emulated PowerPC instructions.

Initial candidates:

- DSP/audio frame processing;
- paired-single/vector/matrix batches;
- texture format conversion/swizzle;
- decompression;
- game-specific profiled kernels.

The final kernel set should be driven by GekkoCTR profiling on real games.

## 5. Local memory

Local memory is identified through opaque handles.

```text
host alloc -> handle 17
host upload large table once

frame N:
  submit kernel(handle 17, small parameters)

frame N+1:
  submit kernel(handle 17, small parameters)
```

This is preferable to repeatedly transmitting a large working set.

The API must eventually support:

- allocation/free;
- upload/download ranges;
- alignment and capability queries;
- immutable/read-mostly buffers;
- device-owned state;
- asynchronous DMA.

## 6. Asynchronous execution

Synchronous RPC is the wrong default.

Desired scheduling:

```text
ARM11/GekkoCTR                    GekkoPAK

submit DSP ---------------------> queue/run DSP
continue game logic               ...
submit math --------------------> queue/run math
prepare PICA200 work              DSP completes
poll/collect DSP <--------------- result
continue                          math completes
collect math <------------------- result
```

The protocol therefore uses sequence/job IDs and explicit completion state.

## 7. Hardware form factor and power

Physical cartridge dimensions should not constrain early prototypes. The intended prototype is an **extended cartridge body** with only the connector tongue constrained by the handheld slot.

Preferred early power architecture:

```text
2DS XL cartridge slot -> signaling / limited board power
USB-C 5 V ------------> accelerator power rail
```

USB-C power keeps FPGA and local-memory experiments independent of cartridge-slot power limits. USB data may additionally be used for firmware, tracing, logging and PC-assisted development.

A slim LiPo is a possible later portability feature, but it should come only after real power and thermal measurements. A battery revision requires charging, protection, state reporting and enclosure safety work that does not help validate the accelerator concept.

## 8. Hardware candidates

The project intentionally avoids selecting a final FPGA or RAM part during Phase 0. Hardware requirements will be derived from measured workloads.

We need to learn:

- required multiply/DSP resources;
- BRAM needs;
- external memory bandwidth;
- kernel clock targets;
- peak and average power;
- acceptable PCB area;
- cartridge-interface voltage translation requirements.

## 9. Performance accounting

Every accelerated job should report at least:

```text
job type
TX bytes
RX bytes
queue delay
command/setup time
transfer time
compute time
total device latency
host software baseline
net time saved
```

Frame-level profiling should distinguish **critical-path time saved** from compute that merely moved to another device without shortening the frame.

## 10. Guiding principle

The hardware exists to serve the static recomp runtime, not the other way around.

We should first identify hot, batchable workloads in GekkoCTR; then build only the accelerator resources that materially reduce frame time on the 2DS XL.
