# GekkoPAK Roadmap

The roadmap is evidence-driven: each phase should answer a question that determines whether the following hardware investment is justified.

## Phase 0 — Virtual hardware model

**Question:** What compute/transfer ratios make cartridge offload worthwhile?

Deliverables:

- [x] transport-independent public API scaffold
- [x] command/bandwidth/compute timing model
- [x] modeled accelerator-local memory
- [x] representative-job CLI
- [x] payload-size crossover sweep
- [ ] automated tests for timing and allocator behavior
- [ ] CSV/JSON benchmark output
- [ ] queue-depth and asynchronous overlap model
- [ ] kernel-specific throughput profiles

Exit criteria:

- repeatable simulator results;
- measured/profiler inputs can replace assumptions;
- clear break-even curves for candidate workloads.

## Phase 1 — GekkoCTR integration

**Question:** Which real static-recomp workloads are worth offloading?

Deliverables:

- [ ] add `libgekkopak` optional backend to GekkoCTR
- [ ] null/software fallback backend
- [ ] instrumentation around candidate hot paths
- [ ] per-frame offload trace export
- [ ] first real-game workload captures

Primary candidates:

1. DSP/audio frame work
2. paired-single/vector/matrix batches
3. texture conversion/swizzle
4. decompression
5. game-specific hot kernels

Exit criteria:

- at least one real workload with a convincing modeled critical-path win;
- exact input/output sizes and call frequency known.

## Phase 2 — Azahar virtual GekkoPAK

**Question:** Does the architecture still help when integrated with a 3DS emulator/runtime boundary?

Deliverables:

- [x] virtual GekkoPAK device/backend in an Azahar development fork
- [x] functional mode
- [x] timed mode (modeled; see the caveat in `CONFORMANCE_RESULTS.md`)
- [ ] selectable hardware profiles
- [x] persistent-buffer implementation
- [x] asynchronous submit/poll/collect behavior

Exit criteria:

- GekkoCTR can run unchanged against software and virtual-device backends;
- virtual hardware timing can be varied without changing guest code.

## Phase 2.5 — Shared device core and conformance

**Question:** Do the emulator and the firmware actually implement the same protocol?

They did not. Three copies of the state machine had drifted apart in four ways
that no test could see, including a guest that would have byte-swapped every
command value word on real silicon.

Deliverables:

- [x] one shared protocol/device core, freestanding enough for an RP2040
- [x] transport adapters for host, Azahar and DSpico
- [x] transport-independent golden wire vectors
- [x] a host-buildable DSpico shim, so firmware handlers run without hardware
- [x] cross-target byte-identity check
- [x] CI gates with every external revision pinned
- [x] a real ROMCNT/FIFO data phase in the emulator, replacing the staging window
- [ ] the same vectors replayed on physical hardware

Exit criteria: met, except that the vectors do not yet run on a console. See
`docs/CONFORMANCE_RESULTS.md`.

## Phase 3 — Real 2DS XL cartridge transport

**Question:** What are the actual cartridge path latency, bandwidth and ARM11 overhead?

Initial hardware:

- existing DSpico-class cartridge
- native CTR test application
- no FPGA required

Bring-up so far (`docs/HARDWARE_DSPICO_V1.md`):

- [x] cartridge boots and is detected
- [x] bus, ROMCTRL and command byte order confirmed on silicon
- [x] F0-F3 control path and the `0xf269b734` checksum
- [x] F4 512-byte block write
- [ ] F5 512-byte block readback — the one open fault
- [ ] any timing measurement at all; every number so far is modeled

Measurements:

- [ ] PING latency distribution
- [ ] write throughput: 16 B -> 256 KiB blocks
- [ ] read throughput: 16 B -> 256 KiB blocks
- [ ] bidirectional request/response throughput
- [ ] host CPU utilization/cycles
- [ ] transfer alignment effects
- [ ] maximum stable command rate
- [ ] overlap with useful ARM11 work

Exit criteria:

- simulator defaults replaced by measured real-hardware profiles.

## Phase 4 — RP2350 extended-cartridge prototype

**Question:** Can a purpose-built smart cartridge provide a reliable accelerator transport and local-memory subsystem?

Mechanical concept:

- cartridge connector tongue plus larger Action-Replay-style body;
- USB-C 5 V external power for development;
- debug/status access without opening the enclosure.

Deliverables:

- [ ] schematic
- [ ] voltage/power architecture
- [ ] cartridge PHY/PIO firmware
- [ ] command FIFO
- [ ] asynchronous job scheduler
- [ ] local PSRAM/SDRAM interface
- [ ] USB firmware/debug bridge
- [ ] physical latency/bandwidth characterization

Battery power is deferred until power measurements are available.

Exit criteria:

- stable hardware implementation of Protocol v0.1;
- persistent local memory works from a real 2DS XL;
- PC-assisted USB accelerator mode can be used for development if useful.

## Phase 5 — FPGA acceleration

**Question:** Can dedicated hardware materially shorten real frame critical paths?

Deliverables depend on Phase 1 profiling. Likely sequence:

- [ ] one high-confidence kernel in RTL
- [ ] Verilator functional/cycle simulation
- [ ] FPGA prototype connected behind MCU/transport
- [ ] local-memory DMA
- [ ] kernel profiling counters
- [ ] asynchronous execution
- [ ] real GekkoCTR A/B benchmark

Do not implement a broad PowerPC interpreter in FPGA. Prefer coarse static-recomp kernels.

Exit criteria:

- demonstrated wall-clock/frame-time gain on a real New 2DS XL.

## Phase 6 — Portable hardware revision

Only begin after Phase 5 proves useful acceleration.

Possible work:

- [ ] integrated PCB
- [ ] lower-power FPGA/accelerator selection
- [ ] custom extended cartridge enclosure
- [ ] USB-C power/data
- [ ] optional protected slim LiPo + charger
- [ ] thermal characterization
- [ ] battery runtime characterization
- [ ] sleep/wake and safe power-state handling

## Decision gates

### Gate A — after Phase 1

If no candidate workload has enough compute intensity to overcome realistic cartridge transfer costs, stop custom hardware work and keep GekkoPAK as a research result.

### Gate B — after Phase 3

If measured cartridge latency/bandwidth is materially worse than modeled, rerun all workload analysis before designing a PCB.

### Gate C — after Phase 5

Only miniaturize and battery-power the device after real hardware shows meaningful frame-time improvement.
