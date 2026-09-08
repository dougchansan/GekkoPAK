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

15 vectors, 190 wire transactions, three targets.

| Result | Status |
|---|---|
| `host-core` against the golden responses | PASS |
| `azahar-model` against the golden responses | PASS |
| `dspico-shim` against the golden responses | PASS |
| all three byte-identical to each other | PASS |

Coverage: the full `HELLO`..`COMPLETE` lifecycle; the `GK` discriminator; bad
opcodes; bad handles on every command that takes one; allocation limits;
`COLLECT` before ready; out-of-range register indices; `F4`/`F5` single and
batch-8; block rejections; per-descriptor failure inside an accepted block;
completion-queue overflow; and a 256-byte round trip whose ramp covers every
byte value.

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

The full cold start is 19 transactions:

```
#0  EXEC HELLO          #7  WRITE_REG  ARG0=4096   #14 WRITE_REG ARG0=handle
#1  READ_REG RESULT     #8  EXEC ALLOC             #15 EXEC FREE
#2  READ_REG OUT0       #9  READ_REG RESULT        #16 READ_REG RESULT
#3  EXEC GET_CAPS       #10 READ_REG OUT0          #17 EXEC COMPLETE
#4  READ_REG RESULT     #11 WRITE_BLOCK  len=80    #18 READ_REG RESULT
#5  READ_REG OUT0       #12 READ_REG  event depth
#6  READ_REG OUT1       #13 READ_BLOCK  len=64
```

That stream also confirms the byte-order fix on the emulator side end to end:
`#7` decodes big-endian to `ARG0 = 0x1000`, which is 4096. Read the old way it
would have been `0x00100000`. `#11` and `#13` likewise confirm the corrected
`F4`/`F5` word encodings against a real guest rather than against a test
harness.

It is kept as `tools/testdata/azahar_guest_trace.log` so the checker is
exercised on every push, not only in the fourteen-minute Azahar job.

### The data phase is real, and so is the bus

`F4`/`F5` used to carry their 512-byte payload through a staging window inside
the register page. The protocol above it was exact and the bus transfer
underneath it was not modelled at all.

That window is gone, and so is the workaround that replaced it. The guest
assembles its block in ordinary RAM, programs ROMCNT's block-size field, and
streams 128 words through the FIFO. `CARD_START` stays asserted for the whole
transfer, and **reading the FIFO is what advances it and clears `DATA_READY`** —
exactly as on hardware. Nothing acknowledges anything.

That last part needed a change to Azahar itself. The emulator had no MMIO page
type at all: Citra's `MMIORegion` was removed, so a mapped page was simply RAM
and the device could not observe a read. The overlay adds one back as *generic*
infrastructure — a page type, a VMA type, and a handler registry that dispatches
by address. Azahar's own sources name GekkoPAK in exactly one place, a single
`Install()` call; the memory core and VM manager name it nowhere, and the
overlay verifier fails if that ever changes. The dynarmic JIT needed no change,
because it already falls back to the memory callbacks for any page whose pointer
is null. See `docs/AZAHAR_MMIO.md`.

A guest that programs the wrong block size for its opcode, or touches the FIFO
with no transfer open, is now a reported fault rather than something nothing
notices.

The observable protocol did not move through any of this: the traced command
stream is the same 19 transactions, in the same order, with the same values,
across the staging window, the tick-driven FIFO and the MMIO model. That is the
point — the transport changed three times and the protocol did not, and the
golden vectors prove it each time.

**What Azahar still cannot model is time.** A transfer completes as fast as the
guest can issue loads and stores; there are no card clocks, no latency settings
and no bus contention. The MMIO hook is what makes modelling them possible, but
nothing is modelled yet. Also unmodelled: the KEY2 scrambler, the cartridge IRQ,
and the dropped-first-transaction-after-a-pause behaviour found on silicon.
Those still need a console.


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
protocol, the read-payload callback, the DMA response, the wire byte order, and
the declared-versus-supplied data-phase length. It does not reproduce timing,
the IRQ, scrambling, or the dropped-first-transaction behaviour. Those are
properties of the silicon.

### The one remaining hand-maintained copy

`prototype/dspico-v1/ds-test/include/gekkopak_ntr.h` is the DS-side client. It
is C, built with devkitARM, and cannot include the shared C++ header, so it
restates the opcodes, the block-word encodings and the `GKD1`/`GKC1` layouts by
hand. All of them were checked against `include/gekkopak/protocol.h` field for
field, and the structs now carry `_Static_assert`s on their 64-byte size and on
eight descriptors filling a block -- because a padding change there would not
fail cleanly, it would shift the record, which is indistinguishable on-screen
from a bus fault.

Those assertions fire only when the DS application is built, and that build is
not in CI: it needs devkitARM and produces a `.nds` nobody can run without
hardware. So this file is the weakest link in the conformance chain, and worth
watching whenever the block ABI changes.

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

Two further candidates have since been excluded.

The DS-side client encodes the `F5` command correctly:
`(length << 16) | offset` with selector 1, matching the corrected specification
and the firmware.

And the handler supplies exactly as many bytes as its FIFO directive promises.
The host shim now checks that, because a handler that declares one length and
supplies another does not fail cleanly on hardware -- the console clocks out
exactly what the directive promised, so a short handler leaves it reading
undriven data and a long one strands words in the FIFO for the *next*
transaction to return first. Either way the payload arrives shifted, which is
precisely what a record at a non-zero byte offset looks like. The check is
verified to fire: injecting a twelve-byte short DMA into the `F5` handler turns
seven vectors red, and the real handler passes.

So the fault is not in the command encoding and not in the declared-versus-
supplied length. What remains is the data phase as the *silicon* runs it: the
`ntrc_beginWrite` / `ntrc_dmaToBus` pair against the real PIO, the console-side
block-size and latency configuration, or the interaction between them --
including the dropped-first-transaction behaviour, which is the one mechanism
already known to strand a transaction's worth of data. A twelve-byte skew is
three words, which is the shape of a FIFO priming problem rather than a data
problem.

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
- **Latency configuration and transfer timing.** Azahar now exercises the
  block-size field and dispatches every FIFO access to the device, but models no
  timing: a word crosses as fast as the guest can issue a load, not after a
  number of card clocks.
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
