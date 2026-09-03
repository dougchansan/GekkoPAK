#!/usr/bin/env python3
"""Convert a gekkopak_test.nds SD result CSV into the repository result files.

The on-device app writes /gekkopak/results/latest.csv on the DSpico SD card.
This turns it into results/dspico-v1.csv (normalised) and results/dspico-v1.json
(machine readable, for the simulator to ingest).

  python tools/parse_hw_results.py latest.csv \
      --csv results/dspico-v1.csv --json results/dspico-v1.json
"""

import argparse
import csv
import json
import sys

# The provisional figures the Azahar/DSpico model has been using. They are
# carried through only so a report can state the delta; they are never used as
# a fallback when a measurement is missing.
MODEL_BANDWIDTH_MIB_S = 6.0
MODEL_CMD_LATENCY_US = 25.0


def parse(path):
    stats, sizes, batches = {}, {}, {}
    with open(path, newline="") as f:
        for row in csv.reader(f):
            if not row or row[0] == "metric":
                continue
            name, unit = row[0], row[1] if len(row) > 1 else ""
            vals = row[2:7] + [""] * 5

            def num(v):
                if v in ("", None):
                    return None
                return float(v)

            if name.startswith("size_"):
                _, size, kind = name.split("_", 2)
                sizes.setdefault(int(size), {})[kind] = num(vals[1])
            elif name.startswith("batch_"):
                batches[int(name.split("_")[1])] = num(vals[1])
            else:
                stats[name] = {
                    "unit": unit,
                    "min": num(vals[0]),
                    "median": num(vals[1]),
                    "mean": num(vals[2]),
                    "p95": num(vals[3]),
                    "max": num(vals[4]),
                }
    return stats, sizes, batches


def mib_per_s(bytes_moved, micros):
    if not micros:
        return None
    return (bytes_moved / (micros / 1_000_000.0)) / (1024 * 1024)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--csv", default="results/dspico-v1.csv")
    ap.add_argument("--json", default="results/dspico-v1.json")
    args = ap.parse_args()

    stats, sizes, batches = parse(args.input)
    if not stats:
        raise SystemExit(f"{args.input}: no rows parsed")

    write_med = (stats.get("f4_write_512") or {}).get("median")
    read_med = (stats.get("f5_read_512") or {}).get("median")
    cmd_med = (stats.get("cmd_latency") or {}).get("median")

    out = {
        "target": "dspico-v1",
        "console": "New 2DS XL",
        "cartridge": "DSpico RP2040",
        "protocol": "GekkoPAK v1 (F0-F5, 512-byte block transport)",
        "latency_us": stats,
        "payload_sweep_us": {str(k): v for k, v in sorted(sizes.items())},
        "batch_us_per_job": {str(k): v for k, v in sorted(batches.items())},
        "bandwidth_mib_s": {
            "write": mib_per_s(512, write_med),
            "read": mib_per_s(512, read_med),
        },
        "model_comparison": {
            "assumed_bandwidth_mib_s": MODEL_BANDWIDTH_MIB_S,
            "assumed_cmd_latency_us": MODEL_CMD_LATENCY_US,
            "measured_cmd_latency_us": cmd_med,
        },
    }

    with open(args.json, "w") as f:
        json.dump(out, f, indent=2)
        f.write("\n")

    with open(args.csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["metric", "unit", "min", "median", "mean", "p95", "max"])
        for name, s in stats.items():
            w.writerow([name, s["unit"], s["min"], s["median"], s["mean"], s["p95"], s["max"]])
        for size, kinds in sorted(sizes.items()):
            for kind, v in kinds.items():
                w.writerow([f"size_{size}_{kind}", "us", "", v, "", "", ""])
        for n, v in sorted(batches.items()):
            w.writerow([f"batch_{n}", "us_per_job", "", v, "", "", ""])

    print(f"wrote {args.csv} and {args.json}")
    for label, measured, assumed in (
        ("cmd latency us", cmd_med, MODEL_CMD_LATENCY_US),
        ("write MiB/s", out["bandwidth_mib_s"]["write"], MODEL_BANDWIDTH_MIB_S),
        ("read MiB/s", out["bandwidth_mib_s"]["read"], MODEL_BANDWIDTH_MIB_S),
    ):
        if measured is None:
            print(f"  {label:16s} measured: (missing)  assumed: {assumed}")
        else:
            print(f"  {label:16s} measured: {measured:.3f}  assumed: {assumed}  "
                  f"ratio {measured / assumed:.2f}x")
    return 0


if __name__ == "__main__":
    sys.exit(main())
