#!/usr/bin/env python3
"""The host harness and the firmware must agree about the link grammar.

They are written in different languages and cannot share a header, so the
harness restates the firmware's event kinds. That is a deliberate choice -- a
generated table would relabel a drifted kind silently, whereas a restated one
can be checked -- but it is only worth anything if something checks it.

So: the constants here are read out of the firmware sources and compared with
the harness's, and the harness's parsers are run against the exact bytes the
firmware's emitters produce. A mismatch fails here rather than by mislabelling
a hardware transcript, which is the kind of error that costs an afternoon.

Runs with no hardware and no pyserial.
"""

import re
import sys
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ROOT / "prototype/dspico-v1/overlay/src"


def load_harness():
    """Imports the harness with pyserial stubbed out.

    CI has no serial hardware and should not need the dependency to check that
    the two sides agree about a text protocol.
    """
    if "serial" not in sys.modules:
        stub = types.ModuleType("serial")
        stub.Serial = object
        stub.SerialException = Exception
        tools = types.ModuleType("serial.tools")
        ports = types.ModuleType("serial.tools.list_ports")
        ports.comports = lambda: []
        tools.list_ports = ports
        stub.tools = tools
        sys.modules["serial"] = stub
        sys.modules["serial.tools"] = tools
        sys.modules["serial.tools.list_ports"] = ports

    sys.path.insert(0, str(ROOT / "tools"))
    import gpk_harness  # noqa: E402

    return gpk_harness


def firmware_trace_kinds() -> dict[int, str]:
    """Reads the GPK_TRACE_* enumerators out of gekkopakTrace.h."""
    text = (OVERLAY / "gekkopakTrace.h").read_text()
    body = text.split("enum {", 1)[1].split("};", 1)[0]
    kinds = {}
    for name, value in re.findall(r"GPK_TRACE_(\w+)\s*=\s*(\d+)", body):
        if name == "KIND_COUNT":
            continue
        kinds[int(value)] = name
    if not kinds:
        raise AssertionError("no GPK_TRACE_* kinds found; did the header move?")
    return kinds


def test_kinds_match_firmware(harness):
    firmware = firmware_trace_kinds()
    assert set(firmware) == set(harness.TRACE_KINDS), (
        f"kind numbers differ: firmware {sorted(firmware)} "
        f"vs harness {sorted(harness.TRACE_KINDS)}"
    )
    # The names need not be identical -- the harness spells them for a reader,
    # as "F4.parse" rather than "BLOCK_PARSE" -- but every kind must be named.
    for kind in firmware:
        assert harness.TRACE_KINDS[kind], f"kind {kind} unnamed in the harness"
    print(f"harness: all {len(firmware)} firmware trace kinds accounted for")


def test_default_mask_matches_firmware(harness):
    text = (OVERLAY / "gekkopakTrace.h").read_text()
    assert "GPK_TRACE_MASK_DEFAULT_ON" in text
    # The firmware's convenience mask is everything but the per-word F3 traffic.
    # The harness computes the same thing; if either changes, the other has to.
    payload_bit = 1 << [k for k, v in firmware_trace_kinds().items()
                        if v == "PAYLOAD"][0]
    assert harness.MASK_DEFAULT == harness.MASK_ALL & ~payload_bit
    assert harness.MASK_DEFAULT & payload_bit == 0
    print(f"harness: default mask {harness.MASK_DEFAULT:#x} excludes F3 payload words")


def test_status_parsing(harness):
    # Exactly the shape emitStatus() produces: hex values, no leading 0x.
    line = ("S f4enter=1B f4acc=1B f4comp=1B f4parse=7 f5sent=2 staged=40 "
            "stage=1 depth=0 result=0 local=10000 mask=3DE tdepth=5 tdrop=0")
    fields = harness.parse_status(["# gekkopak-link 1 cdc", line])
    assert fields["f4enter"] == 0x1B == 27
    assert fields["f4parse"] == 7
    assert fields["local"] == 65536
    assert fields["depth"] == 0
    # The number the current hardware defect is about.
    assert fields["f4comp"] - fields["f4parse"] == 20
    print("harness: status line parsed, 27 delivered / 7 parsed reproduced")


def test_trace_parsing_and_labels(harness):
    lines = [
        "T 0001E240 06 00 0001 00000050",  # F4 accepted, 80 bytes
        "T 0001E280 07 00 0000 00000050",  # parsed, result 0
        "T 0001E2C0 08 01 0002 00000040",  # F5 armed from the completion buffer
        "T 0001E300 09 01 0000 00000001",  # acked, one record dropped from queue
        "P 4 0",
    ]
    records, dropped = harness.parse_trace(lines)
    assert len(records) == 4
    assert dropped == 0
    assert records[0][0] == 0x1E240

    rendered = [harness.describe_trace(r) for r in records]
    assert "accepted" in rendered[0]
    assert "len=80" in rendered[0]
    assert "from=completion" in rendered[2]
    assert "dropped=1" in rendered[3]
    # A refused F4 must not read like an accepted one.
    refused = harness.describe_trace((0, 6, 0, 0, 0))
    assert "refused" in refused and "accepted" not in refused
    print("harness: trace records decoded, refused and accepted distinguishable")


def test_unknown_kind_is_visible_not_mislabelled(harness):
    rendered = harness.describe_trace((0, 0x1F, 0, 0, 0))
    assert "kind1F" in rendered
    print("harness: an unknown kind shows as a number rather than a wrong name")


def test_block_parsing(harness):
    # A GKC1 completion record at offset 0, as the working F5 readback produced.
    head = "474B4331" + "01000000" + "01000000" + "07000000"
    lines = []
    for offset in range(0, 512, 32):
        payload = (head + "00" * 16) if offset == 0 else "00" * 32
        lines.append(f"X {offset:03X} {payload}")
    lines.append("K b 200")
    data = harness.parse_block(lines)
    assert len(data) == 512
    assert data[:4] == b"GKC1"
    dump = harness.hexdump(data[:16])
    assert "47 4B 43 31" in dump and "|GKC1" in dump
    print("harness: 512-byte block reassembled, GKC1 recognised at offset 0")


def test_out_of_order_block_lines(harness):
    # The link emits in order, but a reader that assumed so would corrupt a
    # dump silently if that ever stopped being true.
    lines = [f"X {offset:03X} " + f"{offset // 32:02X}" * 32
             for offset in range(480, -1, -32)]
    lines.append("K b 200")
    data = harness.parse_block(lines)
    assert data[0] == 0x00 and data[32] == 0x01 and data[480] == 0x0F
    print("harness: block lines reassembled by offset, not by arrival order")


def test_mask_resolution(harness):
    assert harness.resolve_mask("default") == harness.MASK_DEFAULT
    assert harness.resolve_mask("all") == harness.MASK_ALL
    assert harness.resolve_mask("none") == 0
    assert harness.resolve_mask("0x1c") == 0x1C
    named = harness.resolve_mask("F4.in,F4.parse")
    assert named == harness.TRACE_BITS["F4.in"] | harness.TRACE_BITS["F4.parse"]
    # A typo must fail loudly: a silently-empty mask would look like a
    # transport that produced no events.
    try:
        harness.resolve_mask("F4.enter")
    except SystemExit as exc:
        assert "unknown trace category" in str(exc)
    else:
        raise AssertionError("an unknown category was accepted")
    print("harness: mask spellings resolved, unknown categories rejected")


def main() -> int:
    harness = load_harness()
    test_kinds_match_firmware(harness)
    test_default_mask_matches_firmware(harness)
    test_status_parsing(harness)
    test_trace_parsing_and_labels(harness)
    test_unknown_kind_is_visible_not_mislabelled(harness)
    test_block_parsing(harness)
    test_out_of_order_block_lines(harness)
    test_mask_resolution(harness)
    print("harness protocol: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
