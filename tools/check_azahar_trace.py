#!/usr/bin/env python3
"""Validate the GekkoPAK command stream a real Azahar guest produced.

The golden vectors in tests/conformance/vectors prove that the device answers
correctly. They say nothing about what a guest actually sends. This closes that
gap: it reads the GKPAK-TRACE lines the Azahar overlay emits when GEKKOPAK_TRACE
is set, decodes every command off the wire, and checks the stream against the
same protocol the vectors describe.

Run the E2E with GEKKOPAK_TRACE=1 and pipe its log in:

    python3 tools/check_azahar_trace.py azahar-ntr-e2e.log
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass

TRACE = re.compile(r"GKPAK-TRACE\s+([0-9A-Fa-f]{16})\s+depth=(\d+)")

MAGIC = (0x47, 0x4B)

WIRE_NAMES = {
    0xF0: "WRITE_REG",
    0xF1: "EXEC",
    0xF2: "READ_REG",
    0xF3: "WRITE_PAYLOAD_WORD",
    0xF4: "WRITE_BLOCK",
    0xF5: "READ_BLOCK",
}

HIGH_NAMES = {
    1: "HELLO",
    2: "GET_CAPS",
    3: "ALLOC",
    4: "UPLOAD",
    5: "SUBMIT",
    6: "POLL",
    7: "COLLECT",
    8: "FREE",
    9: "COMPLETE",
}

EVENT_COMPLETION_DEPTH = 0xFE
BLOCK_BYTES = 512
SELECTOR_SUBMISSION = 0
SELECTOR_COMPLETION = 1


@dataclass
class Command:
    index_in_stream: int
    opcode: int
    index: int
    word: int
    depth: int

    def __str__(self) -> str:
        name = WIRE_NAMES.get(self.opcode, f"0x{self.opcode:02X}")
        if self.opcode == 0xF1:
            name += f" {HIGH_NAMES.get(self.index, self.index)}"
        return f"#{self.index_in_stream} {name} index={self.index} word=0x{self.word:08X}"


def parse(path: str) -> tuple[list[Command], list[str]]:
    commands: list[Command] = []
    errors: list[str] = []
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            match = TRACE.search(line)
            if not match:
                continue
            raw = bytes.fromhex(match.group(1))
            n = len(commands)
            if (raw[1], raw[2]) != MAGIC:
                errors.append(
                    f"command #{n}: missing GK discriminator, got {raw[1]:02X} {raw[2]:02X}"
                )
            # Canonical big-endian value word.
            word = int.from_bytes(raw[4:8], "big")
            commands.append(Command(n, raw[0], raw[3], word, int(match.group(2))))
    return commands, errors


def check(commands: list[Command]) -> list[str]:
    errors: list[str] = []

    for cmd in commands:
        if cmd.opcode not in WIRE_NAMES:
            errors.append(f"{cmd}: opcode outside the F0-F5 GekkoPAK range")
        if cmd.opcode == 0xF1 and cmd.index not in HIGH_NAMES:
            errors.append(f"{cmd}: unknown high-level command")
        if cmd.opcode in (0xF0, 0xF2) and cmd.index >= 16 and cmd.index != EVENT_COMPLETION_DEPTH:
            errors.append(f"{cmd}: staging register index out of range")
        if cmd.opcode == 0xF4:
            if cmd.index != SELECTOR_SUBMISSION:
                errors.append(f"{cmd}: F4 must target the submission queue (selector 0)")
            length = cmd.word & 0xFFFF
            if not 0 < length <= BLOCK_BYTES:
                errors.append(f"{cmd}: F4 meaningful length {length} outside 1..512")
        if cmd.opcode == 0xF5:
            if cmd.index != SELECTOR_COMPLETION:
                errors.append(f"{cmd}: F5 must target the completion queue (selector 1)")
            offset = cmd.word & 0xFFFF
            length = cmd.word >> 16
            if offset != 0:
                errors.append(f"{cmd}: F5 offset {offset} is not supported")
            if not 0 < length <= BLOCK_BYTES:
                errors.append(f"{cmd}: F5 requested length {length} outside 1..512")

    execs = [c for c in commands if c.opcode == 0xF1]
    order = [c.index for c in execs]

    if not order:
        errors.append("no EXEC commands in the trace: the guest never reached the protocol")
        return errors

    if order[0] != 1:
        errors.append(f"first EXEC is {HIGH_NAMES.get(order[0], order[0])}, expected HELLO")
    if order[-1] != 9:
        errors.append(f"last EXEC is {HIGH_NAMES.get(order[-1], order[-1])}, expected COMPLETE")

    # ALLOC must precede any use of a handle, and FREE must follow it.
    if 3 in order:
        alloc_at = order.index(3)
        for command_id in (4, 5):
            if command_id in order and order.index(command_id) < alloc_at:
                errors.append(
                    f"{HIGH_NAMES[command_id]} appears before ALLOC"
                )
        if 8 in order and order.index(8) < alloc_at:
            errors.append("FREE appears before ALLOC")

    # A completion read should only follow something that could have queued one.
    for cmd in commands:
        if cmd.opcode == 0xF5 and cmd.index_in_stream > 0:
            earlier = commands[: cmd.index_in_stream]
            if not any(c.opcode == 0xF4 for c in earlier):
                errors.append(f"{cmd}: F5 with no preceding F4 to have queued a completion")
            break

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", help="Azahar E2E log captured with GEKKOPAK_TRACE=1")
    # The v1 block-transport guest needs 19 transactions for a cold start:
    # HELLO, GET_CAPS and ALLOC over F0-F2, then one F4 + one F2 event read +
    # one F5, then FREE and COMPLETE. The superseded F0-F3 guest needed 46 for
    # the same work, which is where a higher threshold would have come from.
    parser.add_argument(
        "--min-commands",
        type=int,
        default=12,
        help="fail if fewer commands were traced than this (default 12)",
    )
    parser.add_argument(
        "--require-block",
        action="store_true",
        help="fail unless the guest used F4/F5 block transport, not just F0-F3",
    )
    parser.add_argument("--print-stream", action="store_true")
    args = parser.parse_args()

    commands, errors = parse(args.log)
    errors += check(commands)

    if args.print_stream:
        for cmd in commands:
            print(f"  {cmd}")

    counts: dict[str, int] = {}
    for cmd in commands:
        counts[WIRE_NAMES.get(cmd.opcode, "?")] = counts.get(WIRE_NAMES.get(cmd.opcode, "?"), 0) + 1
    execs = [HIGH_NAMES.get(c.index, str(c.index)) for c in commands if c.opcode == 0xF1]

    print(f"traced commands: {len(commands)}")
    for name, count in sorted(counts.items()):
        print(f"  {name:<20} {count}")
    print(f"high-level sequence: {' '.join(execs)}")

    if len(commands) < args.min_commands:
        errors.append(
            f"only {len(commands)} commands traced, expected at least {args.min_commands}; "
            "the guest probably did not run to completion"
        )

    if args.require_block:
        if not any(c.opcode == 0xF4 for c in commands):
            errors.append("no F4 WRITE_BLOCK: the guest did not use block transport")
        if not any(c.opcode == 0xF5 for c in commands):
            errors.append("no F5 READ_BLOCK: the guest did not use block transport")

    if errors:
        print("\nTRACE CHECK FAIL")
        for error in errors:
            print(f"  {error}")
        return 1

    print("\nTRACE CHECK PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
