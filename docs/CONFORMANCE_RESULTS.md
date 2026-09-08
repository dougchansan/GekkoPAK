# GekkoPAK conformance results

What is proven, by what, and what still needs physical hardware.

The distinction this document exists to keep straight is between three very
different kinds of claim:

1. **protocol correctness** — the device answers the right bytes;
2. **modeled timing** — an arithmetic assumption about a bus nobody measured;
3. **physical measurement** — a number read off a New 2DS XL.

Only the first is fully established. The second is clearly labelled everywhere
it appears. The third is largely still missing, and nothing in this repository
should be read as a hardware performance result.

## What runs against what

```
         golden vectors (tests/conformance/vectors/*.vec)
                            |
     +----------------+-----+------------+
     |                |                  |
 host-core       azahar-model       dspico-shim
 (Device API)   (register page)   (firmware handlers)
     |                |                  |
     +----------------+------------------+
                      |
            shared gekkopak::Device
```

| Target | What it is | Where it runs |
|---|---|---|
| `host-core` | `gekkopak::Device` through its own API | host, CTest |
| `azahar-model` | the register-page transport, which is the Azahar device model | host, CTest |
| `dspico-shim` | `prototype/dspico-v1/overlay/src/gekkopakNtr.cpp`, the RP2040 source, on a host shim | host, CTest |
| Azahar E2E | a real ARMv6K guest driving a mapped NTRCARD page in a patched Azahar | CI, Linux |
| DSpico firmware | the same overlay cross-compiled for RP2040 | CI, compile-proof only |
| DSpico hardware | a New 2DS XL driving a real DSpico | manual, see `HARDWARE_DSPICO_V1.md` |

## Host conformance

14 vectors, 172 wire transactions, three targets.

| Result | Status |
|---|---|
| `host-core` against the golden responses | PASS |
| `azahar-model` against the golden responses | PASS |
| `dspico-shim` against the golden responses | PASS |
| all three byte-identical to each other | PASS |

Coverage: the full `HELLO`..`COMPLETE` lifecycle; the `GK` discriminator; bad
opcodes; bad handles on every command that takes one; allocation limits;
`COLLECT` before ready; out-of-range register indices; `F4`/`F5` single and
batch-8; block rejections; per-descriptor failure inside an accepted block; and
a 256-byte round trip whose ramp covers every byte value.

Deterministic values pinned rather than derived:

| Value | Meaning |
|---|---|
| `0xf269b734` | FNV-1a of the 16-byte reference pattern, confirmed on silicon |
| `0x90a458c5` | FNV-1a of the 256-byte round-trip ramp |
| 2530 us / 1.383x | modeled batch-8 job, F0-F3 legacy job |
| 2738 us / 1.278x | modeled single-descriptor block job |
| 238396 ns | modeled transport for one F4 + F2 + F5 round trip |
| 29800 ns | that transport divided across a batch of eight |
| 50 / 21 / 3 | transfers for cold start, F0-F3 steady state, block steady state |

## What the conformance run found

Four divergences existed before this work and none of them was caught by any
test, because each implementation was checked only against itself.

| # | Divergence | Resolution |
|---|---|---|
| 1 | Azahar advertised capabilities `0x0F`, the model and DSpico `0x1F` | one shared constant, `0x1F` |
| 2 | `NTR_WIRE_V1.md` specified the F5 length/offset halves the opposite way round from both implementations | the document was wrong; corrected |
| 3 | A refused `F4` left `RESULT` untouched on DSpico while the emulator reported `BadBlock` | DSpico now routes rejections through the core's validation |
| 4 | The ARM guest emitted the command value word little-endian; DSpico hardware reads it big-endian | `rev` before the store; big-endian everywhere |

Number 4 is the one that mattered most. A guest developed entirely against the
emulator would have byte-swapped every value word on real silicon, and the
emulator would have kept agreeing with itself indefinitely.

Two further behaviours were found to be undefined rather than wrong, and are
now recorded as contract in `NTR_WIRE_V1.md` and pinned by vectors: the `F1`
sequence word is advisory and enforces nothing, and allocation handles are slot
indices that a later `ALLOC` can reissue.

## Azahar

The real ARMv6K guest test is unchanged in kind: it still runs a genuine ARM
guest performing mapped loads and stores against the NTRCARD register page in a
patched Azahar, and it has not been replaced by a host-only fake.

What changed is that the device behind that page is now the shared core, and
that the run captures the guest's command stream (`GEKKOPAK_TRACE=1`) so
`tools/check_azahar_trace.py` can check what the guest actually sent rather than
only that it reached `PASS`.

**Fidelity gap: `F4`/`F5` in Azahar are not a real data phase.** The 512-byte
payload is carried through a staging window inside the register page
(`0x100`/`0x300`) rather than through the ROMCNT FIFO. The protocol semantics
above it are exact; the bus transfer underneath it is not modelled at all. So
Azahar can prove the descriptor ABI, the queue behaviour and the responses, and
cannot prove anything about block-size fields, latency settings or DMA. That is
precisely the part hardware has to answer.

## DSpico

| Check | Status |
|---|---|
| Overlay applies to pristine upstream at the pinned commit | PASS |
| Overlay verifier finds the adapter and the shared core | PASS |
| Firmware handlers answer the golden vectors on the host shim | PASS |
| Cross-compiles for RP2040 against the pinned Pico SDK | CI gate |
| F0-F3 control path on real hardware | PASS (measured) |
| F4 block write on real hardware | PASS (measured) |
| F5 completion readback on real hardware | **FAIL** |

Upstream: `LNH-team/dspico-firmware` @
`472c9d8e9957ad18df367f14b9cc337b9b887e65`, pinned in `deps.lock`. Nothing is
vendored; the overlay is a copy-in patch of two adapter files plus the shared
core.

The host shim reproduces the cmd0/cmd1 dispatch, the PIO FIFO directive
protocol, the read-payload callback, the DMA response and the wire byte order.
It does not reproduce timing, the IRQ, scrambling, or the dropped-first-
transaction behaviour. Those are properties of the silicon.

## The F5 hardware fault

`docs/HARDWARE_DSPICO_V1.md` records that on real hardware `F5` returns a block
whose `GKC1` record does not validate: the magic arrives at byte offset 12
rather than 0, and the checksum field reads `0x01000000` — which is the
version field, value 1, sitting twelve bytes out of place.

The conformance run puts a firm boundary around what that can be. The identical
handler, driven with identical commands through the host shim, produces a
correctly aligned record beginning `47 4B 43 31 01 00 00 00`. So the fault is
**not** in the descriptor ABI, the completion queue, the record layout, the
selector or length encoding, or the byte order of the payload. All of those are
now excluded by evidence rather than by argument.

What remains is the 512-byte cartridge-to-console data phase itself: the
`ntrc_beginWrite` / `ntrc_dmaToBus` pair, the console-side block-size and
latency configuration, or the interaction between them. A twelve-byte skew is
three words, which is the shape of a FIFO priming or alignment problem rather
than a data problem.

This is exactly the split the conformance harness was built to produce: a
hardware fault that can no longer be confused with a protocol fault.

## Modeled, not measured

Every timing number in this repository outside `HARDWARE_DSPICO_V1.md` comes
from these three assumptions:

| Parameter | Value | Status |
|---|---|---|
| command latency | 25 us | **assumption** |
| bus bandwidth | 6 MiB/s | **assumption** |
| accelerator throughput | 1e8 ops/s | **assumption** |

They are configurable per device (`protocol::TimingModel`), and they exist so
that "deterministic" means the same numbers on every target — not because
anyone measured them. A modeled 1.383x speedup is a statement about arithmetic,
not about a New 2DS XL.

`results/dspico-v1.csv` and `results/dspico-v1.json` are deliberately empty of
measurements. Replacing these assumptions is the entire point of the hardware
phase, and a placeholder that looked like data would be worse than a blank
table.

One measured fact already contradicts the shape of the model: the NTR bus
block-size field encodes only 4 bytes or 512-and-up, so there is no
16/32/64/128/256-byte transfer. The model's smooth bytes-per-second curve does
not describe the real cost structure at small sizes, where the choice is
between N four-byte `F3` transactions and one 512-byte `F4`.

## What cannot be tested without hardware

- **F5 block egress.** The only known failure, and it needs a console.
- **Real latency and bandwidth.** Every number above is an assumption.
- **The dropped-first-transaction-after-a-pause behaviour.** Found on silicon,
  absent from any documentation, not reproducible in either emulator. The
  host-side workaround is to issue control reads twice and take the second.
- **Block-size and latency configuration.** Azahar carries block payloads
  through a register window, so it exercises no block-size field at all.
- **KEY2 scrambling and the mode transition.** Modelled nowhere.
- **Sustained throughput, alignment effects, maximum command rate, ARM11
  overhead.** All Phase 3 roadmap measurements, all still open.
- **Whether batching actually wins.** The 85.7% transfer reduction is a
  transfer count, which is real; the microsecond saving attached to it is
  modeled.

## Reproducing

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/gekkopak_conformance          # per-target and cross-target detail
bash prototype/azahar-ntr/scripts/run_model.sh
```

The Azahar E2E and the RP2040 cross-build run in CI; both need Linux and a
toolchain, and the Azahar build takes about forty minutes cold.
