# GekkoPAK Protocol v0.1

This document defines the transport-neutral command model for the virtual device and future cartridge hardware.

The protocol is intentionally simple enough to implement in a host emulator, RP-class MCU firmware, or FPGA command processor.

## 1. Byte order

All multi-byte protocol fields are little-endian.

GameCube guest endianness is handled by the static-recomp/runtime layer or by specific accelerator kernels. The transport itself follows the native little-endian environment of the 3DS ARM host and proposed MCU implementations.

## 2. Packet header

Every command begins with a fixed 32-byte header.

```text
offset  size  field
0x00    4     magic = ASCII "GKPK"
0x04    2     protocol_major
0x06    2     opcode
0x08    4     sequence
0x0C    4     payload_bytes
0x10    4     flags
0x14    4     arg0
0x18    4     arg1
0x1C    4     reserved
```

`sequence` is selected by the host and echoed by the device. It is used to associate asynchronous responses/completions with submitted commands.

`reserved` must be transmitted as zero in v0.1.

## 3. Responses

Responses use the same 32-byte framing. The response opcode is the request opcode ORed with `0x8000`.

For responses:

```text
arg0 = status code
arg1 = opcode-specific result
```

A response may also include a payload.

## 4. Status codes

```text
0   OK
1   BAD_MAGIC
2   BAD_VERSION
3   BAD_OPCODE
4   BAD_ARGUMENT
5   NO_MEMORY
6   INVALID_HANDLE
7   UNSUPPORTED
8   BUSY
9   NOT_READY
10  INTERNAL_ERROR
```

## 5. Core opcodes

```text
0x0000  PING
0x0001  HELLO
0x0002  GET_CAPS
0x0003  RESET

0x0010  ALLOC
0x0011  FREE
0x0012  UPLOAD
0x0013  DOWNLOAD

0x0020  SUBMIT
0x0021  POLL
0x0022  COLLECT
0x0023  CANCEL

0x0030  GET_STATS
```

Kernel-specific opcodes are not exposed directly. `SUBMIT` carries a kernel/job identifier so transport and accelerator implementation remain separate.

## 6. HELLO

Host sends:

```text
protocol_major = 1
arg0 = minimum supported minor version
arg1 = maximum supported minor version
```

Device response returns the selected protocol version and implementation identifier.

The host must not assume a specific MCU or FPGA from the implementation identifier. Features are discovered through `GET_CAPS`.

## 7. GET_CAPS

The device returns a capability block containing at minimum:

```text
protocol version
capability bits
local memory bytes
maximum command payload
maximum transfer payload
maximum in-flight jobs
alignment requirements
implementation/build identifier
```

Initial capability bits:

```text
bit 0  local memory
bit 1  DSP/audio kernels
bit 2  paired-single/vector kernels
bit 3  texture conversion kernels
bit 4  decompression kernels
bit 5  asynchronous queue
bit 6  profiling counters
```

## 8. Local-memory commands

### ALLOC

Request:

```text
arg0 = requested bytes low 32
arg1 = requested bytes high 32
payload = optional allocation parameters
```

Response:

```text
arg1 = BufferHandle
```

### FREE

```text
arg0 = BufferHandle
```

### UPLOAD

```text
arg0 = BufferHandle
arg1 = destination offset
payload = bytes to copy
```

Large transfers may be split across multiple commands.

### DOWNLOAD

```text
arg0 = BufferHandle
arg1 = source offset
payload_bytes = requested byte count
```

Response payload contains the bytes.

## 9. Job submission

`SUBMIT` must be non-blocking whenever the device advertises asynchronous queue capability.

Request:

```text
arg0 = kernel ID
arg1 = job flags
payload = kernel-specific descriptor
```

Response:

```text
arg1 = JobHandle
```

A kernel descriptor should prefer local-memory handles and compact scalar parameters over large inline buffers.

## 10. POLL

Request:

```text
arg0 = JobHandle
```

Response `arg1`:

```text
0 = queued
1 = running
2 = complete
3 = failed
4 = cancelled
```

## 11. COLLECT

Request:

```text
arg0 = JobHandle
```

The response returns completion metadata and any small inline result payload.

Large results should normally be written to a local-memory buffer and retrieved explicitly through `DOWNLOAD`.

## 12. Kernel IDs

Initial experimental IDs:

```text
0x00000000  NULL / protocol test
0x00000001  DSP_FRAME
0x00000002  PAIRED_SINGLE_BATCH
0x00000003  TEXTURE_CONVERT
0x00000004  DECOMPRESS

0x80000000-0xFFFFFFFF  game/project-specific experimental kernels
```

No kernel ABI is frozen in Phase 0. Kernel descriptors require their own version field.

## 13. Profiling

`GET_STATS` should eventually expose per-command and per-job timing including:

```text
bytes received
bytes transmitted
queue delay
execution cycles/time
DMA cycles/time
command count
errors
```

The host should record its own observed end-to-end latency as well. Device-side counters alone do not reveal cartridge/host scheduling cost.

## 14. Error recovery

The protocol must tolerate:

- unsupported kernels;
- device reset;
- stale handles;
- malformed lengths;
- command timeout;
- missing accelerator hardware behind the transport MCU.

GekkoCTR must always be able to disable GekkoPAK and continue through its software path after a recoverable accelerator failure.

## 15. Security/robustness boundary

All sizes, handles, offsets and payload lengths are untrusted at every transport boundary. Firmware and emulator implementations must bounds-check commands before touching local memory or DMA engines.

The protocol must never treat a cartridge-supplied pointer as a host virtual address.
