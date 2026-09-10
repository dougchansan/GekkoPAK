# Azahar mapped NTR register-page runtime pass

Status: **PASS** — independent validation path using the stock Azahar 2126.0 libretro core.

This is distinct from the source-patched Azahar NTRCARD backend. It exists to isolate and validate the ARM guest, register semantics, and GekkoPAK device state machine while the core-side backend is compiled/tested.

## What was tested

The test frontend uses C++ symbols exported by stock Azahar to:

1. obtain the running `Core::System`, kernel, and current ARM process;
2. allocate a real 4 KiB FCRAM-backed page at guest virtual address `0x1EC64000`;
3. obtain the host pointer to that mapped page;
4. run the normal ARMv6K NTR guest unchanged;
5. after each emulated frame, copy the 4 KiB register page into the standalone `gekkopak::ntr::Device`, call one device `Tick()`, and copy the resulting register state back;
6. inspect only the final guest summary after the ARM guest completes.

The guest therefore performs ordinary mapped CPU loads/stores to the NTR register addresses. The frontend does not synthesize high-level GekkoPAK commands or modify the guest summary.

This is a development harness, not the intended production architecture. The source-patched Azahar backend moves the `Tick()` into `Core::System::RunLoop()` and maps the page from Azahar core code.

## Clean deterministic result

```text
frame 0  transfers=1  complete=0
frame 1  transfers=2  complete=0
frame 2  transfers=3  complete=0
...
frame 44 transfers=45 complete=1
frame 45 transfers=46 complete=1
frame 46 transfers=46 complete=1

status          : 0x53534150 (PASS)
protocol        : 1.0
capabilities    : 0x0000000f
local RAM       : 32 MiB
allocation      : handle 1
job             : handle 1
modeled offload : 2530 us
speedup         : 1.383x
payload checksum: 0xf269b734

NTR E2E PASS
clean shutdown
```

That capability word is `0x0000000f`, the pre-block-transport value. Adding
F4/F5 set bit 4, so current runs report `0x0000001f`. The rest of the baseline
is unchanged. See `docs/CONFORMANCE_RESULTS.md`.

Exit status: `0`.

## What this proves

- The ARM guest's NTR command-word packing is correct.
- `ROMCNT` start/data-ready polling works when the register page is genuinely mapped into the ARM process.
- The F0/F1/F2/F3 bring-up wire protocol works end-to-end through ordinary guest memory accesses.
- The guest-to-device 16-byte payload arrives with the expected byte order and checksum.
- Persistent allocation/job handles survive the complete transaction.
- The guest now publishes PASS only after the `COMPLETE` command has been acknowledged.
- The deterministic device model reaches completion after 46 guest wire transactions.

## What this does not prove

- It does not prove the source-patched Azahar device integration yet; that has a separate CI E2E gate.
- It does not measure a real NTR cartridge bus, DSpico, RP2350, or FPGA.
- Device service occurs once per emulated frontend frame in this harness rather than once per Azahar core loop.

The next source-patched-core test must reproduce the same summary while also logging `Mapped MMIO window at 0x1EC64000` and `GekkoPAK NTR guest completed` from inside Azahar core code.
(The mapping line named the device until the MMIO layer was made device-agnostic; see `docs/AZAHAR_MMIO.md`.)
