# GekkoPAK DSpico v1 — hardware bring-up

First physical GekkoPAK prototype: a New 2DS XL driving a DSpico RP2040 over the
NTR cartridge bus, running the GekkoPAK F0-F5 overlay with 512-byte F4/F5 block
transport.

```
New 2DS XL
     |  NTR cartridge bus
     v
DSpico RP2040
     +-- GekkoPAK F0-F5 firmware
     +-- 512-byte F4/F5 block transport
```

This document records what was built, how it was built, and the measured
results. Where a number is still an assumption rather than a measurement, it
says so explicitly.

## Status

| Step | Result |
|---|---|
| GekkoPAK overlay applied and verified | PASS |
| DSpico firmware built (RelWithDebInfo, bootloader ROM) | PASS |
| `gekkopak_test.nds` built | PASS |
| Cartridge boot ROM prepared (DSRomEncryptor pipeline) | PASS |
| Firmware flashed to confirmed DSpico | PASS |
| Cartridge detected by New 2DS XL | PASS |
| Pico Loader menu boots with GekkoPAK overlay present | PASS |
| Bus, ROMCTRL, command byte order | **PASS (measured)** |
| GekkoPAK protocol layer (HELLO) | **PASS (measured)** |
| Legacy F0-F3 control path + checksum | **PASS (measured)** |
| F4 block write, all variants | **PASS (measured)** |
| F5 completion readback | **FAIL - record does not validate** |
| Benchmark timings | blocked on F5 |
| Benchmark timings | blocked on F4/F5 |

### Measured on hardware

These are readings from a New 2DS XL driving the DSpico, not model output.

| Reading | Value | Meaning |
|---|---|---|
| `B8` card id | `C00000C2` | bus, ROMCTRL setup, slot-1 ownership and command serialization all correct |
| `E4` sd status | `00000001` | cartridge is in unscrambled game mode; DSpico extended dispatch reached |
| protocol | `1.0` (`0x00010000`) | GekkoPAK overlay answering |
| capabilities | `0000001F` | `kBaseCaps | kCapBlockTransport` - block transport advertised |
| local RAM | 64 KiB | matches `GEKKOPAK_LOCAL_BYTES` |
| register round trip | `DEADBEEF` in, `DEADBEEF` out | command assembly is byte-exact |
| `ALLOC` | result `0` (Ok), handle `1`, size `16` | allocator works |
| **legacy F0-F3 checksum** | **`F269B734`** | **matches the reference exactly** |
| F2 read reliability | 31/32 at latency 4, 32/32 at 8 and above | register path is solid |
| bus timeouts | 0 | no transfer ever failed to complete |

The checksum is the significant one. The 16-byte reference pattern was staged
with F3, committed with `UPLOAD`, and hashed by the RP2040 with FNV-1a to
`0xf269b734` - the value predicted from the PIO shift directions, and distinct
from the `0x899bd1de` a word-swap would produce. **The wire byte order is now
confirmed end to end on silicon**, not merely derived from the source.

### F4 block transport works; F5 readback does not

With reads made self-validating and the actions bound to buttons, the F4 variant
matrix runs cleanly and every variant queues a completion:

```
80B inline   d1 e9  a9  c9  p6
512B inline  d1 e10 a10 c10 p7
64B noinl    d1 e11 a11 c11 p8
primed       d1 e13 a13 c13 p9
```

`d1` is the completion queue depth after each attempt, and the RP2040 counters
climb monotonically - entered, accepted, payload complete, descriptor parsed. A
quick test in the same session reports `event depth: 1`, `F4 PASS`, `blkh 2`,
`F4cnt e8 a8 c8 p5`, and the legacy path passing with `F269B734`.

So the 512-byte console-to-cartridge transfer, the descriptor ABI and the
completion queue all work on hardware. Several rounds were spent varying the
write - meaningful-byte count, inline against pre-uploaded payload, primer
blocks, latency 8 through 63 - when the write had been correct throughout and
the failures were lost *reads* of the result.

What remains is narrow. F5 returns a block, but the `GKC1` record does not
validate and its checksum field reads `01000000` where `F269B734` is expected.
The legacy path hashes the same bytes correctly, so the hash is right and the
data is arriving misaligned rather than wrong; `0x01000000` looks like the
version field, value 1, sitting a few bytes out of place. The test now dumps the
first 32 bytes of the block so the layout can be read instead of inferred - a
correct record begins `47 4B 43 31 01 00 00 00`.

The two highest-latency matrix rows (`lat32`, `lat63`) still return `FFFFFFFF`
counters, so those settings break the readback entirely rather than helping.
### F5 root cause: the handler was late, and running from flash

Both open faults -- the F5 readback and the dropped first transaction -- came
down to the same thing, and neither is on the bus. That is why sweeping every
bus parameter found nothing: latency from 4 to 63, EXEC settle from 0 to 1024,
priming, meaningful-byte counts, inline against pre-uploaded input. All of it
was measuring the wrong layer.

**The handlers ran from XIP flash.** Every one of DSpico's 22 cartridge IRQ
handlers is marked `__scratch_y("cpu0")`, which places it in SRAM. None of
GekkoPAK's were, so they executed from flash through the XIP cache. A miss
costs far more than the ~4.8 us a 32-bit word takes at the 6.7 MHz card clock,
and the console does not wait.

That also explains the dropped-first-transaction behaviour exactly: an idle bus
means an idle cache, so the first handler after a pause is slow and its
transaction is lost, while every one after it hits the cache and works. A code
fetch stall, not a timing margin -- which is precisely why no bus parameter
moved it.

**The transfer was armed too late, from a buffer built too late.** F5 called
`ntrc_beginWrite()` and `ntrc_dmaToBus()` in the *cmd1* handler, after building
the completion block. Every DSpico path that drives 512 bytes to the console
arms in *cmd0* and DMAs from a buffer that earlier, non-critical code already
filled -- the SD read, the USB read, the R4 save read and the ROM read page all
do exactly that. Four bytes has enough slack to be armed in cmd1, which is why
F2 register reads worked and F5 never did.

The recorded symptom fits both: `GKC1` arriving at byte offset 12 behind
**three words of `0xFFFFFFFF`**. `0xFF` is an undriven bus. The console was
clocking the data phase before the cartridge had armed anything, and lost
exactly as many words as the handler took to get there.

#### The fix

- The handlers that have a deadline are in scratch RAM. `SCRATCH_Y` is 4 KiB
  and DSpico's own handlers already fill most of it, so this is a budget:
  putting all of GekkoPAK's there overflows the region by 392 bytes. It is
  spent on every cmd0 handler -- the first code to run on an idle bus, and
  where a block data phase is armed -- plus F4's cmd1. F2 stays in flash
  because a four-byte response has slack, and the hardware already proved it
  (31/32 at latency 4, 32/32 above, in the same firmware where every F5 failed).
- F5 arms in cmd0, unconditionally, and DMAs a completion block staged outside
  the IRQ path. The selector is in the low byte of cmd0, so the source can be
  chosen without waiting for the value word; cmd1 delivers the verdict on the
  command afterwards.
- The staging is double-buffered, like DSpico's own SD read path. The host shim
  caught that restaging in cmd1 rewrites the buffer the DMA is still streaming.
- Staging uses a non-consuming peek, so an F2 event-depth check still sees a
  completion that has been staged but not yet sent.

`verify_overlay.py` asserts both rules structurally, so neither can regress
silently.

**Status: fixed in firmware, not yet confirmed on hardware.** The reasoning is
from the DSpico sources and the recorded symptom; the console run that closes it
has not happened yet.

### Raw F5 diagnostic

F5 selector `0x7E` returns a fixed 512-byte pattern with no allocator, job queue
or completion queue involved:

```
word[i] = 0xF5 << 24 | i << 16 | (i ^ 0x7F) << 8 | (i + 0xA5)
```

Each word carries its own index twice under a constant tag, so word order, byte
order, truncation, a stale buffer, a repeated FIFO word and any starting offset
all look different from each other and from a correct read. The test application
runs it before anything that interprets a completion record, and reports the
first mismatching word, how many leading words were undriven, and where the
pattern actually starts.

That separates the two questions the campaign has to keep apart: *can the
cartridge put 512 known bytes on the wire*, and *is the protocol above it
right*.

### The first transaction after a pause is dropped

The single most important hardware behaviour found, and it is not in any
documentation.

After any pause in bus traffic the next transaction is lost. It returns
undriven data - `FFFFFFFF`, or a partly driven value such as `00FFFFFF` - and
every transaction after it is stable and correct. A burst read makes it
unambiguous: reading `RESULT` and `OUT0` four times each returned
`00000000` x4 and `00000001` x4, while the single reads immediately before them
returned `FFFFFFFF`.

It is not a timing-margin problem. Latency was swept from 4 to 63 with no
effect, and an EXEC settle delay was swept from 0 to 1024 with no effect. The
command itself is dropped: the RP2040 is still finishing the previous handler
when the next `CEB` edge arrives, so its PIO never captures that command at all.

This is why the symptoms moved around so much. It looked like an EXEC problem
when EXEC-based commands failed, and like a wire-format problem when `B8` and
the `DEADBEEF` round trip failed, but the common factor was always a
transaction following a gap. The DS-side fix is to issue control reads twice and
take the second, and to prime before an isolated block transfer. Sequential
traffic - which is what the benchmarks measure - is unaffected.

For the RP2040 side, this is worth addressing at source rather than papering
over from the host: the handler should hand its response to the PIO before doing
any other work.

### Iterating without swapping the card

`prototype/dspico-v1/build_usb_tools.sh` builds the DSpico USB mass-storage
application, DLDI-patched for this cartridge. Launch it from Pico Loader with a
USB cable attached and the DSpico's microSD mounts on the host while the card
stays in the cartridge, so a new test build can be copied and results read back
without a physical swap.

That swap, twice per iteration, was the dominant cost of this bring-up - not the
difficulty of the faults. It is worth setting up before any further hardware
work.

It needs a pre-calico devkitARM: libtwl, which the USB platform is built on,
redefines `REG_IME`/`REG_IE`/`REG_IF` and calls `setVectorBase()`, all of which
collide with calico in current libnds and fail the build in `rtosIrq.c`. Tag
`20230622` is the newest verified to work and the script pins it.

A full CDC debug channel - routing the application's own log to the host - is a
larger job. The platform is libtwl plus tinyusb on a custom ARM7 rather than a
libnds module, so it means porting the test application to libtwl and adding
ARM7/ARM9 IPC, and the USB path uses card commands on the same bus being
measured. Workable by buffering results and transmitting after timing finishes,
but a rewrite rather than an addition.

### SD result export: abandoned

Results are collected by photographing the summary page. Writing them to the
card was tried at length and never worked reliably - only the first write of a
run reached the card, with later writes reporting success and vanishing. Four
explanations were investigated and all were wrong: an unflushed libfat cache,
the path prefix, an `E4` desync of DSpico's SD state machine, and a missing
readback. The test application therefore renders everything onto one screen.

Reading the card over USB afterwards showed what was actually happening. The
DLDI write test wrote `dldi-post-2
`, twelve bytes, read it back, compared
equal and reported `WRITE OK` - and the file on the card is **eleven** bytes.
So writes do reach the card but arrive truncated, and a read-back immediately
afterwards is served from cache and agrees with what was written. That single
behaviour accounts for all four symptoms above: files that appear to write
successfully, arrive wrong or not at all, and verify fine at the time. It is a
defect in this libfat/DLDI path, not in the test application, and it is the
reason results are collected from the screen rather than the filesystem.

### Getting a homebrew ROM to boot: what actually works

Five builds tried to make `gekkopak_test.nds` itself the cartridge boot ROM by
editing its header. That approach is a dead end and the record is kept here so
it is not repeated:

| Attempt | Change | Result |
|---|---|---|
| 1 | ARM9 relocated to 0x8000, raw key table injected | not detected |
| 2 | no relocation, raw key table injected | not detected |
| 3 | + NTR-only unit code, header CRC fixed by hand | not detected |
| 4 | + DSRomEncryptor (secure area encrypted) | detected, booted, white screen |
| 5 | build 4 plus app-side diagnostics only | not detected |

Build 5 was structurally identical to build 4 through the same pipeline - same
unit code, same ARM9 offset, encrypted secure area, both CRCs valid - and
differed only inside the ARM9 payload, which the console does not inspect during
detection. No structural explanation for the difference was found.

The supported path avoids all of it. `prototype/dspico-v1/build_bootloader.sh`
builds the official DSpico Bootloader with BlocksDS, DLDI-patches it, runs it
through DSRomEncryptor and embeds it as `roms/default.nds`. The cartridge then
boots Pico Loader, which chainloads `fat:/_picoboot.nds`, and the test
application runs as ordinary homebrew from `/roms` with a working DLDI driver.

Three things a hand-edited ROM was missing, all handled by that pipeline:

- **Key blocks must be transformed by the ROM game code**
  (`KeyTransform.TransformTable(gameCode, 2, 8, ntrBlowfish)`), not copied raw
  from an ARM7 BIOS dump. Raw injection was wrong in principle.
- **The secure area at 0x4000-0x4800 must be KEY1-encrypted** with the
  `encryObj` marker the console validates. `ndstool -se` refuses to create one
  for homebrew; DSRomEncryptor builds it.
- **A TWL-hybrid ROM needs TWL key blocks and a TWL area.** The bootloader is
  hybrid (`unit_code & 2`, TWL flags bit 0), so it needs both key sets.

`ndstool -f` does **not** fix the header CRC (ndstool 2.3.1 exits 0 and leaves
0x15E untouched), so any hand edit inside the first 0x15E bytes ships a stale
checksum. Compute it directly.

DSRomEncryptor only encrypts when the stored secure-area CRC at 0x6C disagrees
with what it computes. BlocksDS leaves 0x6C unset so the check always fires;
ndstool fills it in, which makes a devkitPro ROM look already-prepared and
silently skips encryption.

Blowfish tables are Nintendo copyrighted data and are never stored in this
repository. The build script reads user-supplied BIOS dumps and locates the TWL
table by hash rather than a fixed offset, because dumps vary: the arm7i dump on
this machine matched neither documented whole-file hash but carried the correct
table at 0xC6D0.

### Confirming the flash without picotool

`picotool` was unavailable and the RP2 Boot interface had no WinUSB driver bound,
so the firmware could not be read back. The flash was instead confirmed
behaviourally, using the fact that DSpico reboots to BOOTSEL when it starts
without an SD card:

1. With the SD in the host card reader, the device sat in BOOTSEL. Ambiguous — a
   blank RP2040 and a healthy DSpico with no SD look identical from the host.
2. Inserting the SD changed nothing, which is correct: card insertion does not
   reset the RP2040, and the firmware only probes for an SD at startup.
3. Writing the UF2 with the SD present made the device disappear from USB within
   300 ms and **stay** gone.

Step 3 is the discriminating observation. The disappearance proves the
bootloader accepted the image and reset; not coming back proves the firmware
booted and found its SD, because a blank part or an SD-less DSpico would have
re-enumerated as `RPI-RP2` immediately.

## Hardware

| Item | Value |
|---|---|
| Console | New 2DS XL |
| Cartridge | DSpico RP2040 |
| RP2040 USB identity | `VID_2E8A` / `PID_0003` (BOOTSEL) |
| RP2040 serial | `E0C9125B0D9B` |
| BOOTSEL volume | `RPI-RP2`, UF2 Bootloader v3.0, Board-ID `RPI-RP2` |
| DSpico SD | 3.6 GB FAT32 (within the 4 GB limit R4 mode requires) |
| Prior firmware | picoLoader (`_pico/picoLoader{7,9}.bin`, boot ROM `_picoboot.nds`) |

Identity was confirmed on three independent signals before flashing: the PnP
VID/PID, the PnP instance serial, and the `version=` field in the bootloader's
`INDEX.HTM` — all agreeing on `E0C9125B0D9B`.

## Software revisions

| Component | Revision |
|---|---|
| GekkoPAK | branch `phase2-v1-dspico`, parent `b361f3bbe8ec1b45126424c95cfc50df074e0ec3` |
| DSpico firmware | `develop` @ `472c9d8e9957ad18df367f14b9cc337b9b887e65` |
| Pico SDK | `6a7db34ff63345a7badec79ebea3aaef1712f374` |
| arm-none-eabi-gcc | 14.2.1 (20241119) |
| CMake / Ninja | 4.2.3 / 1.13.2 |
| devkitARM | `devkitpro/devkitarm:latest` (ndstool 2.3.1) |
| Host OS | Windows 11 Pro 26200; builds in WSL2 Ubuntu 26.04 and Docker |

## Artifacts

| Artifact | SHA-256 |
|---|---|
| `DSpico-GekkoPAK.uf2` (bootloader + overlay) — current | `dd98e0e3415dfa8fab9382ff9cbc4adf085cf183a8f7aa7b7157d21cde649f5f` |
| embedded `roms/default.nds` (DSpico Bootloader, 557056 B) | `62fff559fcae9ed82d771fbb6b53c8cc57d600a6d9b911cb0489320167c23c35` |
| `gekkopak_test.nds` (261120 B, runs from /roms) | `ee452ddf462643e06ea29043dbe3cca0bba64b9522a8434fbbc911bfb8b6c21a` |

`arm-none-eabi-size DSpico.elf`:

```
   text	   data	    bss	    dec	    hex
 600092	      0	 216096	 816188	  c743c
```

216 KiB of BSS on a 264 KiB RP2040 is worth watching: 64 KiB of it is the
GekkoPAK local pool (`GEKKOPAK_LOCAL_BYTES`), and the two 512-byte block
buffers plus DSpico's own SD and USB buffers account for the rest. There is
headroom, but not enough to raise the local pool much without moving it.

## Wire byte order — settled

`docs/NTR_WIRE_V1.md` flagged this boundary as the one that must not be guessed.
It is now pinned to the DSpico PIO configuration rather than to convention:

- `sm_config_set_in_shift(&c, false, true, 32)` — **shift left**. The first
  command byte lands in bits 31:24, so `cmd0 >> 24` is the opcode and the
  8-byte command header is big-endian: `OP 47 4B II VV VV VV VV`.
- `sm_config_set_out_shift(&c, true, true, 32)` — **shift right**. Cart→console
  words go out LSB first, so ROM/F5 memory order equals wire order.
- `ntrCardIrq.S` applies `rev` to each received payload word before storing, so
  console→cart bytes land in DSpico SRAM in the order the DS emitted them.

Net result: **the command header is big-endian; the payload is byte-transparent
in both directions.** The host owns only the command assembly, exactly as the
overlay README requires. This is confirmed arithmetically — the reference
16-byte pattern

```
44 33 22 11  88 77 66 55  DD CC BB AA  0D F0 AD 0B
```

hashes under DSpico's FNV-1a to `0xf269b734` when the bytes are laid into the
block in written order, and to `0x899bd1de` if word-swapped. The test asserts
`0xf269b734`, so a byte-order regression fails loudly rather than silently.

## Test application

`prototype/dspico-v1/ds-test/` builds `gekkopak_test.nds` with libnds. It is a
transport validator, not a GameCube runtime. Stages:

1. **Discovery** — claim slot-1 for the ARM9, transition the cartridge to
   unscrambled game mode with `FC`, then `F1 HELLO` / `F2` register reads for
   protocol, capabilities, local RAM and transport count.
2. **Legacy control** — `F0`/`F2` register round trip with `0xDEADBEEF` (which
   would fail immediately on a big/little-endian command mistake), then
   `F3` + `UPLOAD` reproducing the reference checksum through the legacy path.
   This isolates F3 ordering from the F4 block path.
3. **F4 WRITE_BLOCK** — one `GKD1` descriptor plus the 16-byte pattern inline
   (80 meaningful bytes), checksum asserted against `0xf269b734`.
4. **Event/completion** — compact `F2` index `0xFE` depth read with a short
   bounded retry, not a busy loop.
5. **F5 READ_BLOCK** — validates `GKC1` magic, version, sequence, status,
   job handle and checksum.
6. **Batch 1/2/4/8** — eight 64-byte descriptors exactly fill a 512-byte block,
   so batch-8 carries no inline input and references a pre-uploaded allocation.

The FC transition is attempted first with KEY2 command scrambling on and then
off, and the app reports which worked, because whether the console's boot path
leaves KEY2 active in game mode is exactly the kind of thing that should be
measured rather than assumed.

### Failure reporting

A failure is never reported as "hardware test failed". `gpk_transport_init()`
returns the first layer that broke — slot-1 ownership, the FC transition,
HELLO, or protocol mismatch — and the top screen names it.

## Benchmark method

- 100 iterations per measurement after a 16-iteration warm-up.
- Timing from cascaded hardware timers 0/1 at 33.513982 MHz, read without
  stopping them, with a re-read of the low half to catch a carry landing
  between the two reads.
- Reported as min / median / mean / p95 (nearest-rank) / max.
- Bus latency is a parameter, not a constant: `docs/commands.md` requires ≥4
  cycles for cart→console and ≥8 for console→cart. Defaults are 8 and 16, and
  the values used are printed with every result so a number is reproducible.

### A bus fact that changes the cost model

The NTR bus block-size field only encodes 4 bytes (`CARD_BLK_SIZE(7)`) or
512 bytes and up (`CARD_BLK_SIZE(1..6)`). **There is no 16/32/64/128/256-byte
bus transfer.** So a "64-byte payload" is either sixteen 4-byte `F3`
transactions or one 512-byte `F4` whose meaningful region is 64 bytes. The
sweep measures both, because the crossover between them is the real shape of
the transport cost and the simulator currently models neither.

## Results

Measured numbers land in `results/dspico-v1.csv` and `results/dspico-v1.json`
once the console run completes; the on-device run also writes
`/gekkopak/results/latest.{txt,csv}` to the DSpico SD card.

**No measurements are recorded yet.** The emulator's provisional
6 MiB/s / 25 µs command latency figures remain assumptions and are deliberately
not copied into the results files — the entire purpose of this exercise is to
replace them, so a placeholder that looks like data would be worse than an
empty table.

## Building the artefacts

Both builds are containerised or pinned, so a result can be traced to an exact
input. Nothing here needs a hand-configured toolchain.

**Test ROM** -- devkitARM is not installed on the host and should not be:

```bash
./prototype/dspico-v1/build_ds_test.sh
```

Uses `devkitpro/devkitarm:latest`; override with `GEKKOPAK_DEVKITARM_IMAGE` to
reproduce an older result. The application needs calico (`pmMainLoop`), which
the older tagged images predate.

**Firmware** -- needs the prepared boot ROM from the original bring-up, because
the Blowfish tables it embeds are Nintendo copyrighted data and are never stored
in this repository:

```bash
./prototype/dspico-v1/build_firmware.sh <workdir> <gekkopak-repo> <default.nds>
```

`DSPICO_REF` is pinned to the `deps.lock` commit rather than tracking
`develop`. Diagnosing a hardware defect against a moving upstream is how a fix
gets attributed to the wrong change.

### This campaign's artefacts

| Artefact | Value |
|---|---|
| `gekkopak_test.nds` | `8cec0e1f066cbd56bcbe679795f7446105cdf3a05aae29c86d4a18d6518234c6` (259072 B) |
| devkitARM image | `devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5` |
| devkitARM | 16.1.0 |
| DSpico firmware | `472c9d8e9957ad18df367f14b9cc337b9b887e65` (unchanged; the overlay carries the fix) |
| Pico SDK | `6a7db34ff63345a7badec79ebea3aaef1712f374` |
| RP2040 cross-build | green in CI with the scratch-RAM placement |

The firmware UF2 is not listed: it embeds the user-supplied boot ROM, so its
hash is specific to the machine that built it and is recorded with the results
of a run rather than here.

## Running it

1. Reinsert the DSpico's microSD into the DSpico (it must not be in the PC
   reader — DSpico deliberately reboots to BOOTSEL when it starts without one).
2. Insert the DSpico into the New 2DS XL and power on. The flashed firmware
   boots `gekkopak_test.nds` directly.
3. `A` runs the quick test, `X` the full benchmark, `START` reruns, `SELECT`
   writes the report to the SD card.
4. Copy `/gekkopak/results/latest.csv` back and run
   `python tools/parse_hw_results.py latest.csv --json results/dspico-v1.json`.

USB power on the DSpico is recommended during bring-up.

### Recovery

Eject the SD card and power the DSpico: it returns to BOOTSEL and any `.uf2`
copied to `RPI-RP2` replaces the firmware. The previous picoLoader boot ROM is
rebuildable from `_picoboot.nds` (backed up outside the repository, since it is
not GekkoPAK content).

`picotool` was not available on the build host and the RP2 Boot interface had no
WinUSB driver bound, so **the previously flashed firmware image could not be
read back before flashing**. Only the SD-side loader files were preserved. See
"Confirming the flash without picotool" above for how the new image was verified
instead.

## Next optimization

Deferred until there are real numbers, but the shape of the answer is already
visible from the code: every `F0`/`F1`/`F3` transaction costs a full command
phase for four bytes of useful payload, and the batch-8 descriptor block leaves
no room for inline input, forcing a separate upload pass. If the measurements
show command overhead dominating, the first change should be an `F4` block that
carries descriptors *and* their input data in one transfer — a packed
descriptor/payload layout rather than 8×64 bytes of descriptor alone.
