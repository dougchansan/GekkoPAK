# Azahar NTRCARD GekkoPAK prototype

This is the second emulator prototype. Unlike the original libretro-memory bridge, the ARM guest now speaks through the 3DS NTRCARD register window:

- physical NTRCARD base: `0x10164000`
- ARM11 special-mapping virtual base: `0x1EC64000`
- `ROMCNT`: `+0x04`
- 8-byte command: `+0x08`
- FIFO: `+0x1C`

The guest retains the same high-level GekkoPAK transaction sequence:

`HELLO -> GET_CAPS -> ALLOC -> UPLOAD -> SUBMIT -> POLL -> COLLECT -> FREE -> COMPLETE`

## Wire protocol v0

The cartridge transport uses four currently unused DSpico game-mode opcodes for this prototype:

- `F0 47 4B RR VV VV VV VV`: write staging register `RR`
- `F1 47 4B OP SS SS SS SS`: execute high-level command `OP`
- `F2 47 4B RR 00 00 00 00`: read staging register `RR` through FIFO
- `F3 47 4B II VV VV VV VV`: upload one 32-bit payload word at index `II`

`47 4B` is the ASCII `GK` discriminator. This is intentionally a bring-up protocol, not a final high-throughput transport. Block commands will replace per-word payload transfers later.

## What the Azahar overlay changes

Stock Azahar 2126.0 does not implement ARM11 IO-area special mappings, so a stock core reports `0x1EC64004` as unmapped. The overlay:

1. adds a 4 KiB register-backed GekkoPAK NTR device;
2. maps physical `0x10164000` at virtual `0x1EC64000` for the development 3DSX/ELF process;
3. services the virtual cartridge at CPU-slice boundaries;
4. leaves the frontend out of command execution entirely.

The frontend may inspect ordinary guest RAM only after execution to validate the deterministic result summary. It never services NTR commands.

## Local model

```bash
./scripts/run_model.sh
```

Expected deterministic baseline:

```text
PASS offload=2530 us speedup=1.383x checksum=0xf269b734 wire_transfers=50
```

## Patched Azahar run

After building an Azahar libretro core with `azahar_overlay/apply_overlay.py` applied:

```bash
export AZAHAR_CORE=/path/to/azahar_libretro.so
./scripts/run_patched_core.sh
```

A passing run ends with `NTR E2E PASS`.

## Real-hardware caveat

The direct ARM11 IO mapping is an **Azahar development facility**, not a claim that an ordinary retail 3DS user process can directly map NTRCARD registers. The physical implementation will need the appropriate CFW/kernel/ARM9/Process9-side broker or equivalent privileged transport. The guest-facing GekkoPAK API should remain above that transport boundary.
