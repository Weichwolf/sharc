#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Summarize checked, unscaled hardware runs without estimating missing counts."""

import argparse
import json
import statistics


def summary(records):
    result = []
    for record in records[1:]:
        guest = record["guest_instructions"]
        runs = record["runs"]
        if guest < 10_000_000 or len(runs) != 3:
            raise ValueError("expected three full common-kernel runs")
        metrics = {name: [] for name in (
            "host_instructions_per_guest", "host_cycles_per_guest",
            "guest_instructions_per_second", "guest_to_reference_time", "nanoseconds_per_guest"
        )}
        for run in runs:
            if (run["enabled"] <= 0 or run["enabled"] != run["running"] or
                    not run["host_instructions"] or not run["host_cycles"] or
                    run["nanoseconds"] <= 0 or run["reference_nanoseconds"] <= 0):
                raise ValueError("missing, scaled or invalid measurement")
            metrics["host_instructions_per_guest"].append(run["host_instructions"] / guest)
            metrics["host_cycles_per_guest"].append(run["host_cycles"] / guest)
            metrics["guest_instructions_per_second"].append(guest * 1e9 / run["nanoseconds"])
            metrics["guest_to_reference_time"].append(run["nanoseconds"] / run["reference_nanoseconds"])
            metrics["nanoseconds_per_guest"].append(run["nanoseconds"] / guest)
        result.append({
            "kernel": record["kernel"], "mode": record["mode"], "identity": record["identity"],
            "guest_instructions": guest, "guest_base_cycles": record["guest_base_cycles"],
            "preparation_seconds": record["preparation_seconds"], "native_bytes": record["native_bytes"],
            "metrics": {name: {"min": min(values), "median": statistics.median(values), "max": max(values)}
                        for name, values in metrics.items()},
        })
    if not result:
        raise ValueError("no kernel records")
    return {"host": records[0], "results": result}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("jsonl", type=argparse.FileType("r"))
    args = parser.parse_args()
    print(json.dumps(summary([json.loads(line) for line in args.jsonl if line.strip()]), indent=2))
