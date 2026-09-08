# gekkopak_test.nds — physical transport validator

DS-side half of the GekkoPAK v1 hardware bring-up. It talks to a DSpico running
the GekkoPAK F0-F5 overlay over the real NTR cartridge bus and measures what the
bus actually does. It is **not** a GameCube runtime.

Full context, results and the bring-up findings are in
[`docs/HARDWARE_DSPICO_V1.md`](../../../docs/HARDWARE_DSPICO_V1.md).

## Layout

| File | Role |
|---|---|
| `include/gekkopak_ntr.h` | wire ABI, descriptor/completion structs, transport API |
| `source/gekkopak_ntr.c` | command serialization and the four bus primitives |
| `source/bench.c` | test stages and benchmark statistics |
| `source/main.c` | two-screen UI and SD report export |

`gekkopak_ntr.c` is the only file that knows about bus byte order. The command
header is assembled big-endian by hand (`OP 47 4B II VV VV VV VV`); payload data
is byte-transparent in both directions. See the header comment for why, and
`docs/HARDWARE_DSPICO_V1.md` for the PIO configuration that pins it down.

## Building

```bash
make                      # needs DEVKITARM
```

or without a local devkitPro install:

```bash
docker run --rm -v "$PWD:/work" -w /work devkitpro/devkitarm:latest make
```

Produces `gekkopak_test.nds`.

## Getting it onto hardware

DSpico is a cartridge emulator, so the ROM has to survive the console's full NTR
boot. Two things must be fixed up first — the KEY1 key table and the secure
area — both handled by `tools/dspico_rom_tool.py`:

```bash
python tools/dspico_rom_tool.py \
    --rom prototype/dspico-v1/ds-test/gekkopak_test.nds \
    --keytable /path/to/your/bios7.bin \
    --out dspico-firmware/roms/default.nds
ndstool -f dspico-firmware/roms/default.nds
```

The KEY1 table is Nintendo copyrighted data and is not in this repository;
supply your own ARM7 BIOS dump. Then build the firmware:

```bash
prototype/dspico-v1/build_firmware.sh <workdir> <gekkopak-repo> <default.nds>
```

## Controls

| Key | Action |
|---|---|
| `A` | quick test (discovery, legacy control, F4/F5, checksum) |
| `X` | full benchmark (adds latency, size sweep, batch 1/2/4/8) |
| `START` | rerun the last mode |
| `SELECT` | write `/gekkopak/results/latest.{txt,csv}` to the SD card |

Top screen is the status page, bottom screen is the detail log.

## Reading a failure

The app never just says the test failed. `gpk_transport_init()` reports the
first layer that broke:

| Layer | Meaning |
|---|---|
| `slot-1 owner (EXMEMCNT)` | the ARM9 could not take the card bus |
| `FC unscramble transition` | the cartridge did not enter unscrambled game mode |
| `F1/F2 HELLO no answer` | unscrambled mode reached, but no GekkoPAK response |
| `protocol mismatch` | GekkoPAK answered with an unexpected version |

Beyond that, the legacy stage runs before the block stage on purpose: a `F0`/`F2`
register round trip and an `F3` upload both reproduce the reference checksum, so
a byte-ordering fault shows up in the smallest possible reproducer rather than
in the middle of the F4 path.
