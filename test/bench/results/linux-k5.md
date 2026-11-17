# Linux K5 baseline

Measured implementation: `53e07f34e4a6526037c08483b18080de087622d9`, on an Intel
Core i5-10400, Linux x86-64, Clang 19.1.7 Release. The thread is pinned to CPU 4.
The host reference uses `-O3 -march=native -fno-lto` in its own translation unit.
Preparation, validation and reference execution are outside guest counters. No
build, test or probe owned by this session ran concurrently with measurement.

All six measured runs pass the complete output, checksum, guards, register and
progress checks: 14,682,625 guest instructions and 23,071,742 nominal cycles each.
All six hardware groups are unscaled, with equal nonzero enabled/running times.
Medians below use three warmed pairs per mode. Preparation is measured once per
image, separately; native bytes include every prepared context and cold path.

| Mode | Host instructions / guest | Host cycles / guest | Guest instructions/s | Guest/C++ time | Prepare s | Native bytes |
|---|---:|---:|---:|---:|---:|---:|
| Strict | 13.432316 | 3.854150 | 901066917 | 96.99 | 1.89046 | 111,613 |
| FastFP32 | 13.432316 | 3.854692 | 937877971 | 106.23 | 1.88044 | 111,613 |

Ranges across the same three runs:

| Mode | Host cycles / guest | Guest instructions/s | Guest/C++ time | ns / guest |
|---|---:|---:|---:|---:|
| Strict | 3.847559–3.856878 | 438681053–1051883549 | 85.65–117.84 | 0.951–2.280 |
| FastFP32 | 3.847434–3.856584 | 479599765–1062301605 | 78.08–119.21 | 0.941–2.085 |

Raw host instructions range from 197,221,653 to 197,221,656 in Strict and from
197,221,653 to 197,221,655 in FastFP32. Wall times vary substantially more than
hardware cycles; the ranges are retained. This integer kernel has the same
arithmetic contract in both modes, and these timings establish no FP32 speedup.
The roughly hundredfold gap to the vectorized native C++ algorithm remains open.

This is the initial scalar pre-indexed/software-loop K5 implementation. Hardware
loops, post-modify transfers, other kernels, full profile qualification and
macOS ARM64 remain open. This workload selects complete all-RAM DM backing;
peripheral callbacks and cross-unit transfers still need their own evidence.

[Raw driver records](linux-k5.jsonl), [all min/median/max metrics](linux-k5-summary.json)
and [host/build/source receipt](linux-k5-host.json) retain the measured counts,
identities, code revision and binary/source hashes. The protocol and input
definition are in [the benchmark README](../README.md).
