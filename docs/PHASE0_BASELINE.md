# Phase 0 Baseline

This file records the first executable GekkoPAK model baseline. These are **modeled values, not measurements from a 2DS XL or DSpico**.

## Baseline virtual hardware profile

```text
TX bandwidth:             6.00 MiB/s
RX bandwidth:             6.00 MiB/s
command latency:         25.00 us
accelerator-local RAM:   32.00 MiB
accelerator throughput: 100.00 Mops/s
```

## Build/test sanity check

The initial CMake scaffold was independently configured, built and tested with a C++17 toolchain.

```text
1/1 test passed: gekkopak_simulator
```

## Representative modeled jobs

```text
job                       TX us    compute     RX us   total us   ARM11 us   wins?   speedup
DSP frame mailbox          40.7     2500.0      20.3     2586.0     3500.0     YES     1.35x
paired-single batch       162.8     5000.0      40.7     5228.5     5000.0      NO     0.96x
64 KiB texture roundtrip 10416.7     3000.0   10416.7    23858.3     3000.0      NO     0.13x
resident-data hot kernel   20.3     7500.0      20.3     7565.7     6000.0      NO     0.79x
```

## Immediate takeaway

Even with placeholder assumptions, the model already demonstrates the central design constraint:

- compact mailbox-style jobs can plausibly benefit from acceleration;
- transferring large buffers in both directions can dominate the entire frame budget;
- keeping data resident locally only helps if the accelerator itself executes the kernel faster enough to overcome command/remaining transfer costs.

This is why the project should measure and profile before selecting FPGA resources.

## Next experiments

1. Add queue depth and asynchronous overlap.
2. Add machine-readable trace input/output.
3. Capture real GekkoCTR candidate workloads.
4. Replace the bus assumptions with real 2DS XL <-> DSpico measurements.
5. Re-evaluate kernel choices using critical-path frame time.
