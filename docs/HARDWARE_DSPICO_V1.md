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
| DSpico firmware built (RelWithDebInfo, real ROM) | PASS |
| `gekkopak_test.nds` built | PASS |
| Cartridge boot ROM prepared (DSRomEncryptor pipeline) | PASS |
| Firmware flashed to confirmed DSpico | PASS |
| Cartridge detected by New 2DS XL | PASS |
| Pico Loader menu boots with GekkoPAK overlay present | PASS |
| GekkoPAK transport exercised on hardware | **PENDING** |

Two results are already worth recording. The GekkoPAK overlay does **not** break
DSpico's card emulation - a firmware carrying it boots the cartridge normally,
which had been an open hypothesis while nothing would boot at all. And the
device is back on its original picoLoader setup rather than a replacement boot
ROM, so iterating on the test application is now a file copy to the SD card
instead of a BOOTSEL cycle and a reflash.

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
| `DSpico-GekkoPAK.uf2` (606720 B, 1185 blocks) — current | `77b42823945a1cbcc828c459fbb40306df8603eb4c8397d0e5cfa97c150d0b6d` |
| `DSpico-GekkoPAK.uf2` (639488 B, 1249 blocks) — build 1, rejected | `a599457ca639b5d71dacf187809c354e3bbdfe8fe85cd646f797047f20b4e144` |
| `gekkopak_test.nds` (260096 B) | `16bd2afc1e95986586583d4f89b30f926c038cf86fd165c0cb5bb4862d51950e` |
| embedded `roms/default.nds` (260096 B) — current | `7819eee874f54a4cfacfade3c2a3db40ac43406c7e7e5082aea5abc8c4ac96b2` |

`arm-none-eabi-size DSpico.elf`:

```
   text	   data	    bss	    dec	    hex
 303132	      0	 216096	 519228	  7ec3c
```

216 KiB of BSS on a 264 KiB RP2040 is worth watching: 64 KiB of it is the
GekkoPAK local pool (`GEKKOPAK_LOCAL_BYTES`), and the two 512-byte block
buffers plus DSpico's own SD and USB buffers account for the rest. There is
headroom, but not enough to raise the local pool much without moving it.

## Preparing a homebrew ROM for DSpico

DSpico is a cartridge emulator, not a loader, so the ROM has to survive the
console's own NTR boot. Getting this right took one wrong attempt; both the
attempt and the correction are recorded here because the reasoning matters.

### KEY1 command encryption

`src/ntrCardRomNorm.c:98` calls `bf_init()` with pointers straight into the ROM
image at `0x1600` (P table) and `0x1C00` (S boxes). DSpico carries no key of its
own — it uses whatever the ROM has there, and a devkitPro homebrew ROM has
zeros. The upstream README states the requirement explicitly. The table lands in
header padding well before `arm9_rom_offset`, so injecting it is non-destructive.

The KEY1 table is Nintendo copyrighted data and is **never** stored in this
repository: `tools/dspico_rom_tool.py` takes `--keytable` pointing at a
user-supplied ARM7 BIOS dump (table at `+0x30`, `0x1048` bytes) and
entropy-checks it before use.

### The secure area — do not relocate

GBATEK says a secure area exists only when `arm9_rom_offset` is in
`0x4000..0x7FFF`, which is exactly what devkitPro emits. That reads as an
argument for relocating the ARM9 binary to `0x8000` so the console skips the
secure area a homebrew ROM does not have.

**That reasoning is wrong for this platform, and relocating breaks the cart.**
The first flashed image used it, and the 2DS XL then refused to show the
cartridge on the HOME menu at all — rejected before any GekkoPAK code could run.

The counter-evidence is the loader already working on this hardware.
`_picoboot.nds`, the boot ROM of the picoLoader setup previously flashed to this
same DSpico, has:

| Field | `_picoboot.nds` (works) | relocated build (rejected) |
|---|---|---|
| `arm9_rom_offset` | `0x4000` | `0x8000` |
| header size (`0x84`) | `0x4000` | `0x8000` |
| `0x4000..0x8000` | populated | all zeros |
| device capacity (`0x14`) | `0x02` (512 KiB) | `0x01` (256 KiB), image 270 KiB |

So on real 2DS XL hardware a homebrew ROM boots through DSpico with ARM9 at
`0x4000` and the secure-area region simply carrying the start of the ARM9
binary. Relocation additionally left the capacity field under-declaring the
grown image, so there were two independent defects in that build.

`--no-relocate` is therefore the correct mode, and the working configuration is:
stock devkitPro layout, KEY1 table injected into header padding, nothing else
touched. The relocation path is retained in the tool but is off by default and
should not be used against DSpico.

Why a keyless `_picoboot.nds` boots at all — whether picoLoader's build injects
the table before embedding, or the console's DS-mode path is more permissive
than the upstream README implies — is **not resolved**. Keys are injected
because upstream documents them as required and doing so costs nothing; that is
not the same as having proven they are necessary.

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
