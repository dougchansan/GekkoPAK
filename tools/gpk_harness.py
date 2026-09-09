#!/usr/bin/env python3
"""Drive a GekkoPAK DSpico cartridge from the host.

The hardware loop has been: build, copy to SD, swap the card, boot the console,
photograph the screen, read the photograph. Every hypothesis costs one of those,
which is why the F5 diagnostic was built to answer several questions per boot --
the cycle, not the difficulty of the faults, has been the dominant cost.

This talks to the cartridge instead, over the serial port its own USB connector
presents (prototype/dspico-v1/overlay/src/gekkopakLink.cpp). The cartridge is
the device under test and sees every transaction, so it can say what happened
rather than leaving it to be inferred from what the console failed to notice.

    gpk_harness.py status                 one snapshot of the cartridge
    gpk_harness.py trace --mask default   stream events as they happen
    gpk_harness.py dump staged            hex the block an F5 would send now
    gpk_harness.py flash DSpico.uf2       reflash, no BOOTSEL button
    gpk_harness.py watch                  status + trace until interrupted

Requires pyserial. The cartridge is USB-powered, so all of this works with no
console attached; with one attached, it works while the console runs.
"""

from __future__ import annotations

import argparse
import os
import shutil
import string
import sys
import time
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - environment problem, not a code path
    print(
        "pyserial is required: python -m pip install pyserial",
        file=sys.stderr,
    )
    raise SystemExit(2)

# Raspberry Pi's vendor ID and its CDC product ID -- the pair the Pico SDK's own
# stdio_usb uses, which is what this device is.
USB_VID = 0x2E8A
USB_PID = 0x000A
PRODUCT_HINT = "DSpico cartridge link"

# Mirrors the enum in overlay/src/gekkopakTrace.h. Deliberately restated rather
# than generated: if the firmware's kinds drift, an unknown kind shows up in the
# transcript as a number rather than being silently mislabelled.
TRACE_KINDS = {
    1: "reset",
    2: "F0.writereg",
    3: "F1.exec",
    4: "F2.readreg",
    5: "F3.payload",
    6: "F4.in",
    7: "F4.parse",
    8: "F5.arm",
    9: "F5.ack",
}
TRACE_BITS = {name: 1 << kind for kind, name in TRACE_KINDS.items()}

# Everything except the per-word F3 traffic, matching GPK_TRACE_MASK_DEFAULT_ON.
MASK_DEFAULT = sum(1 << k for k in TRACE_KINDS) & ~(1 << 5)
MASK_ALL = sum(1 << k for k in TRACE_KINDS)

BLOCK_DISPOSITION = {0: "refused", 1: "accepted"}
BLOCK_SOURCE = {0: "zeroes", 1: "diagnostic", 2: "completion"}

BUFFERS = {"staged": "d", "diag": "g", "in": "x"}


class LinkError(RuntimeError):
    pass


class Link:
    """One connection to a cartridge."""

    def __init__(self, port: str, timeout: float = 1.0):
        self.port = port
        self.serial = serial.Serial(port, timeout=timeout)
        self.banner: str | None = None

    def close(self) -> None:
        self.serial.close()

    def __enter__(self) -> "Link":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    def send(self, command: str) -> None:
        self.serial.write((command + "\n").encode("ascii"))
        self.serial.flush()

    def lines(self, deadline: float):
        """Yields decoded lines until `deadline`, whatever arrives."""
        buffer = b""
        while time.monotonic() < deadline:
            chunk = self.serial.read(256)
            if not chunk:
                continue
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                text = line.decode("ascii", "replace").strip()
                if text:
                    yield text

    def request(self, command: str, terminators: tuple[str, ...],
                timeout: float = 2.0) -> list[str]:
        """Sends a command and collects the reply.

        A reply ends at one of `terminators` -- the grammar has no length
        prefix, so a caller says what shape of answer it expects rather than
        waiting out the timeout on every request.
        """
        self.serial.reset_input_buffer()
        self.send(command)
        deadline = time.monotonic() + timeout
        collected: list[str] = []
        for line in self.lines(deadline):
            if line.startswith("# "):
                self.banner = line[2:]
                continue
            collected.append(line)
            if line.startswith("E "):
                raise LinkError(f"{command!r}: {line[2:]}")
            if line.split(" ", 1)[0] in terminators:
                return collected
        raise LinkError(f"{command!r}: no reply within {timeout:g}s")


def find_port(explicit: str | None = None) -> str:
    if explicit:
        return explicit
    candidates = []
    for info in list_ports.comports():
        if info.vid == USB_VID and info.pid == USB_PID:
            candidates.append(info)
    if not candidates:
        raise LinkError(
            f"no cartridge found (looking for USB {USB_VID:04X}:{USB_PID:04X}). "
            "Is it plugged into the PC, and is this a --host-link firmware?"
        )
    # Prefer one that names itself, so a bare Pico on the same machine is not
    # mistaken for the cartridge.
    for info in candidates:
        if PRODUCT_HINT.lower() in (info.product or "").lower():
            return info.device
    return candidates[0].device


# ---------------------------------------------------------------------------
# Decoding
# ---------------------------------------------------------------------------


def parse_status(lines: list[str]) -> dict[str, int]:
    for line in lines:
        if not line.startswith("S "):
            continue
        fields = {}
        for token in line[2:].split():
            key, _, value = token.partition("=")
            fields[key] = int(value, 16)
        return fields
    raise LinkError("no status line in reply")


def describe_trace(record: tuple[int, int, int, int, int]) -> str:
    time_us, kind, index, aux, value = record
    name = TRACE_KINDS.get(kind, f"kind{kind:02X}")
    detail = f"idx={index:02X} val={value:08X}"
    if kind == 6:
        detail = f"sel={index:02X} {BLOCK_DISPOSITION.get(aux, aux)} len={value & 0xFFFF}"
    elif kind == 7:
        detail = f"sel={index:02X} result={aux} len={value & 0xFFFF}"
    elif kind == 8:
        detail = f"sel={index:02X} from={BLOCK_SOURCE.get(aux, aux)} staged={value}"
    elif kind == 9:
        detail = f"sel={index:02X} result={aux} dropped={value}"
    elif kind in (3,):
        detail = f"idx={index:02X} val={value:08X} result={aux}"
    return f"{time_us / 1e6:12.6f}  {name:<12} {detail}"


def parse_trace(lines: list[str]) -> tuple[list[tuple[int, ...]], int]:
    records = []
    dropped = 0
    for line in lines:
        if line.startswith("T "):
            parts = line.split()
            records.append(tuple(int(p, 16) for p in parts[1:6]))
        elif line.startswith("P "):
            dropped = int(line.split()[2], 16)
    return records, dropped


def parse_block(lines: list[str]) -> bytes:
    chunks: dict[int, bytes] = {}
    for line in lines:
        if not line.startswith("X "):
            continue
        _, offset, payload = line.split()
        chunks[int(offset, 16)] = bytes.fromhex(payload)
    if not chunks:
        raise LinkError("no block data in reply")
    return b"".join(chunks[k] for k in sorted(chunks))


def hexdump(data: bytes, width: int = 16) -> str:
    printable = set(string.printable[:95].encode("ascii"))
    out = []
    for offset in range(0, len(data), width):
        row = data[offset:offset + width]
        hexpart = " ".join(f"{b:02X}" for b in row).ljust(width * 3 - 1)
        text = "".join(chr(b) if b in printable else "." for b in row)
        out.append(f"{offset:04X}  {hexpart}  |{text}|")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------


def resolve_mask(text: str) -> int:
    if text == "default":
        return MASK_DEFAULT
    if text == "all":
        return MASK_ALL
    if text in ("none", "off", "0"):
        return 0
    if text.startswith("0x"):
        return int(text, 16)
    mask = 0
    for name in text.split(","):
        name = name.strip()
        if name not in TRACE_BITS:
            raise SystemExit(
                f"unknown trace category {name!r}; known: "
                + ", ".join(sorted(TRACE_BITS))
            )
        mask |= TRACE_BITS[name]
    return mask


def cmd_status(link: Link, _args) -> int:
    fields = parse_status(link.request("s", ("S",)))
    order = [
        ("f4enter", "F4 commands seen"),
        ("f4acc", "F4 data phases started"),
        ("f4comp", "F4 payloads delivered"),
        ("f4parse", "F4 blocks parsed"),
        ("f5sent", "F5 data phases armed"),
        ("depth", "completions queued"),
        ("staged", "staged bytes"),
        ("stage", "staging buffer"),
        ("result", "RESULT register"),
        ("local", "device pool bytes"),
        ("mask", "trace mask"),
        ("tdepth", "trace records waiting"),
        ("tdrop", "trace records dropped"),
    ]
    for key, label in order:
        if key in fields:
            print(f"  {label:<26} {fields[key]}")
    # The number the current defect is about: blocks that arrived complete and
    # were then rejected by the shared core.
    if "f4comp" in fields and "f4parse" in fields:
        rejected = fields["f4comp"] - fields["f4parse"]
        if rejected:
            print(f"\n  {rejected} delivered block(s) rejected by the device core")
    if fields.get("tdrop"):
        print(f"  transcript has holes: {fields['tdrop']} record(s) dropped")
    return 0


def cmd_trace(link: Link, args) -> int:
    mask = resolve_mask(args.mask)
    link.request(f"t{mask:X}", ("K",))
    if args.clear:
        link.request("p", ("P",))
    print(f"# tracing mask {mask:#x}; Ctrl-C to stop", file=sys.stderr)
    seen_dropped = 0
    try:
        while True:
            records, dropped = parse_trace(link.request("p", ("P",)))
            for record in records:
                print(describe_trace(record), flush=True)
            if dropped != seen_dropped:
                print(f"# {dropped - seen_dropped} record(s) dropped", flush=True)
                seen_dropped = dropped
            if args.once:
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("", file=sys.stderr)
    return 0


def cmd_dump(link: Link, args) -> int:
    data = parse_block(link.request(BUFFERS[args.buffer], ("K",), timeout=4.0))
    if args.out:
        Path(args.out).write_bytes(data)
        print(f"{len(data)} bytes -> {args.out}")
    else:
        print(hexdump(data))
    return 0


def cmd_reset(link: Link, _args) -> int:
    link.request("z", ("K",))
    print("device state reset (the card transport was not touched)")
    return 0


def find_bootloader_drive(timeout: float) -> Path:
    """Waits for the RP2040 bootloader to appear as a drive."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        roots: list[Path] = []
        if os.name == "nt":
            roots = [Path(f"{letter}:/") for letter in string.ascii_uppercase]
        else:
            for base in ("/media", "/run/media", "/mnt", "/Volumes"):
                base_path = Path(base)
                if base_path.is_dir():
                    roots.extend(p for p in base_path.rglob("*") if p.is_dir())
        for root in roots:
            try:
                if (root / "INFO_UF2.TXT").exists():
                    return root
            except OSError:
                continue
        time.sleep(0.25)
    raise LinkError(
        f"the bootloader drive did not appear within {timeout:g}s. "
        "Hold BOOTSEL and replug to flash manually."
    )


def cmd_flash(link: Link, args) -> int:
    image = Path(args.image)
    if not image.is_file():
        raise SystemExit(f"no such image: {image}")
    if image.suffix.lower() != ".uf2":
        raise SystemExit(f"expected a .uf2 image, got {image.name}")

    print(f"asking {link.port} to reboot into the bootloader...")
    try:
        link.request("B", ("K",), timeout=2.0)
    except LinkError:
        # The port can disappear as the reply is being read, which is success,
        # not failure -- the reboot is the thing that was asked for.
        pass
    link.close()

    drive = find_bootloader_drive(args.timeout)
    print(f"bootloader at {drive}; copying {image.name} ({image.stat().st_size} bytes)")
    shutil.copy2(image, drive / image.name)

    print("waiting for the cartridge to come back...")
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        try:
            port = find_port(args.port)
        except LinkError:
            time.sleep(0.5)
            continue
        try:
            with Link(port) as fresh:
                # A banner is not a reply -- it has no terminator --
                # so ask for something with an end to it.
                fresh.request("s", ("S",), timeout=3.0)
                print(f"back on {port}: {fresh.banner or 'no banner'}")
                return 0
        except (LinkError, serial.SerialException):
            time.sleep(0.5)
    raise LinkError(
        "the cartridge did not answer after flashing. It may have enumerated "
        "without the link, which would mean the image was not a --host-link build."
    )


def cmd_watch(link: Link, args) -> int:
    mask = resolve_mask(args.mask)
    link.request(f"t{mask:X}", ("K",))
    print(f"# watching, mask {mask:#x}; Ctrl-C to stop", file=sys.stderr)
    previous: dict[str, int] = {}
    try:
        while True:
            records, dropped = parse_trace(link.request("p", ("P",)))
            for record in records:
                print(describe_trace(record), flush=True)
            fields = parse_status(link.request("s", ("S",)))
            changed = {k: v for k, v in fields.items()
                       if k not in ("tdepth",) and previous.get(k) != v}
            if changed and previous:
                summary = " ".join(f"{k}={v}" for k, v in sorted(changed.items()))
                print(f"# {summary}", flush=True)
            previous = fields
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("", file=sys.stderr)
    return 0


def cmd_ports(_link, _args) -> int:
    found = False
    for info in list_ports.comports():
        if info.vid == USB_VID and info.pid == USB_PID:
            found = True
            print(f"{info.device}  {info.product or '?'}  serial={info.serial_number or '?'}")
    if not found:
        print("no GekkoPAK cartridge found")
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", help="serial port; found automatically if omitted")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("ports", help="list attached cartridges").set_defaults(
        handler=cmd_ports, needs_link=False)
    sub.add_parser("status", help="one snapshot of cartridge state").set_defaults(
        handler=cmd_status, needs_link=True)
    sub.add_parser("reset", help="reset device state, leaving the transport alone"
                   ).set_defaults(handler=cmd_reset, needs_link=True)

    trace = sub.add_parser("trace", help="stream transport events")
    trace.add_argument("--mask", default="default",
                       help="default, all, none, a hex value, or a comma-separated "
                            "list of: " + ", ".join(sorted(TRACE_BITS)))
    trace.add_argument("--interval", type=float, default=0.2)
    trace.add_argument("--once", action="store_true", help="drain once and exit")
    trace.add_argument("--clear", action="store_true",
                       help="discard anything already buffered first")
    trace.set_defaults(handler=cmd_trace, needs_link=True)

    dump = sub.add_parser("dump", help="hex a 512-byte cartridge buffer")
    dump.add_argument("buffer", choices=sorted(BUFFERS))
    dump.add_argument("--out", help="write raw bytes to this file instead")
    dump.set_defaults(handler=cmd_dump, needs_link=True)

    flash = sub.add_parser("flash", help="reboot to BOOTSEL and write a .uf2")
    flash.add_argument("image")
    flash.add_argument("--timeout", type=float, default=30.0)
    flash.set_defaults(handler=cmd_flash, needs_link=True)

    watch = sub.add_parser("watch", help="trace plus a running diff of the counters")
    watch.add_argument("--mask", default="default")
    watch.add_argument("--interval", type=float, default=0.5)
    watch.set_defaults(handler=cmd_watch, needs_link=True)

    args = parser.parse_args()
    try:
        if not args.needs_link:
            return args.handler(None, args)
        port = find_port(args.port)
        with Link(port) as link:
            return args.handler(link, args)
    except LinkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except serial.SerialException as exc:
        print(f"serial error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
