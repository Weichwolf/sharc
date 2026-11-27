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
        entry = {
            "kernel": record["kernel"], "mode": record["mode"], "identity": record["identity"],
            "memory_mode": record.get("memory_mode", "exact"),
            "guest_instructions": guest, "guest_base_cycles": record["guest_base_cycles"],
            "preparation_seconds": record["preparation_seconds"], "native_bytes": record["native_bytes"],
            "metrics": {name: {"min": min(values), "median": statistics.median(values), "max": max(values)}
                        for name, values in metrics.items()},
        }
        if "prepared_instructions" in record:
            prepared = record["prepared_instructions"]
            if prepared <= 0:
                raise ValueError("missing prepared instruction denominator")
            entry["prepared_instructions"] = prepared
            entry["native_bytes_per_prepared_instruction"] = record["native_bytes"] / prepared
        if "schedule" in record:
            entry["schedule"] = record["schedule"]
            for name in ("owner_calls", "owner_horizons", "irq_requested", "irq_accepted",
                         "irq_max_delay", "irq_total_delay"):
                values = [run[name] for run in runs]
                if any(value != values[0] for value in values):
                    raise ValueError(f"owner schedule changed between runs: {name}")
                entry[name] = values[0]
        result.append(entry)

    if not result:
        raise ValueError("no kernel records")
    return {"host": records[0], "results": result}


def combine(files):
    host = None
    results = []
    seen = set()
    for source in files:
        report = summary([json.loads(line) for line in source if line.strip()])
        if host is None:
            host = report["host"]
        elif host != report["host"]:
            raise ValueError(f"incompatible host or compiler in {source.name}")
        for entry in report["results"]:
            key = (entry["kernel"], entry.get("schedule", "continuous"),
                   entry["mode"], entry["memory_mode"])
            if key in seen:
                raise ValueError(f"duplicate benchmark result in {source.name}: {key}")
            seen.add(key)
            results.append(entry)
    return {"host": host, "results": results}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("jsonl", nargs="+", type=argparse.FileType("r"))
    args = parser.parse_args()
    print(json.dumps(combine(args.jsonl), indent=2))
