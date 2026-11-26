# Common benchmarks

K4 and K5 have checked Linux measurements. K1–K3, K6, macOS ARM64 and
protected-page peripheral I/O remain open.

## Current Linux results

Medians of three unscaled runs on an Intel i5-1135G7, pinned to logical CPU 3,
with Clang/LLVM 19.1.7. K4 was measured for [PR #15](https://github.com/Weichwolf/sharc/pull/15)
and K5 for [PR #16](https://github.com/Weichwolf/sharc/pull/16). Both workloads
check complete output and precise progress in each numerical identity.

| Kernel | Identity | Memory | Host inst./guest inst. | Host cycles/guest inst. | M guest inst./s | Guest/Clang O3 | Prepare s | Native bytes |
|---|---|---|---:|---:|---:|---:|---:|---:|
| K4 | Strict | exact | 24.278 | 5.683 | 657.9 | 39.33 | 5.823 | 361,783 |
| K4 | FastFP32 | exact | 24.278 | 5.694 | 655.7 | 39.24 | 5.832 | 361,783 |
| K4 | Strict | direct | 24.278 | 5.531 | 675.6 | 38.87 | 5.764 | 361,783 |
| K4 | FastFP32 | direct | 24.278 | 5.545 | 675.5 | 39.16 | 5.661 | 361,783 |
| K5 | Strict | exact | 7.720 | 2.037 | 1855.4 | 59.78 | 0.998 | 55,032 |
| K5 | FastFP32 | exact | 7.720 | 2.033 | 1861.8 | 60.05 | 1.004 | 55,032 |
| K5 | Strict | direct | 7.720 | 2.034 | 1862.6 | 59.50 | 1.015 | 55,037 |
| K5 | FastFP32 | direct | 7.720 | 2.032 | 1864.8 | 57.05 | 1.009 | 55,037 |

Keep raw JSONL, summaries and host receipts under `build/local/`. The baseline,
cost attribution and before/after measurements are recorded in
[K4 attribution](https://github.com/Weichwolf/sharc/issues/1#issuecomment-7191221986),
[K4/K5 lowering](https://github.com/Weichwolf/sharc/issues/2#issuecomment-7191886769),
and [K5 memory loans](https://github.com/Weichwolf/sharc/issues/3#issuecomment-7192058097).

## Running the benchmarks

Configure a Release build with Clang 19. Run the four CTest entries for checked
K4/K5 results in Exact and Direct. Each invocation exercises Strict and FastFP32.

```sh
nice -n 10 cmake --build build --target sharc_k4_bench sharc_k5_bench
ctest --test-dir build --output-on-failure -R '^bench_k[45](_direct)?$'
mkdir -p build/local
build/sharc_k4_bench --measure > build/local/k4-exact.jsonl
build/sharc_k5_bench --measure > build/local/k5-exact.jsonl
python3 test/bench/summarize.py build/local/k4-exact.jsonl
python3 test/bench/summarize.py build/local/k5-exact.jsonl
```

Append `--direct` to measure Direct and record it in a separate file. Older
records without `memory_mode` used Exact. The driver pins itself to its current
allowed CPU. Measurements require a fully scheduled, unscaled per-thread Linux
hardware counter group for user instructions and cycles. Stop other work owned
by the measurement session before recording results.

## Workloads

| Kernel | Assembly | Unique guest instructions | Instructions retired | Base cycles | Output SHA-256 |
|---|---|---:|---:|---:|---|
| K4 | [k4.asm](k4.asm) | 61 | 18,874,369 | 39,845,886 | `490294093fdf6b9e10740daa992391bae19396c6e78bff448f035b4e7ca37f25` |
| K5 | [k5.asm](k5.asm) | 13 | 14,682,625 | 23,071,742 | `e84b69ed409029f1af79718d2a57b8c0ab264a77e3979a939cb39b8087bdc589` |

K4 starts at state 3 with accumulator `0x2468ace0` and executes 1,048,576
transitions. The masked state selects one of eight DM table entries. The
handler adds state to the accumulator, XORs state+1, sets state to
`(state+3) mod 8` and calls a common subroutine. It XORs the accumulator with
a 32-bit right rotation of the new state by one, then adds 3. Arithmetic
wraps modulo `2^32`. Every handler is visited 131,072 times. The independently
derived output tuple is `(3, 0x24d0ace0, 0)`. Each run also checks the complete
register file, both return words, table and guards. The declared main,
handlers and common subroutine are at `0x100/0x200/0x300`; the read-only
table is at `0x90000`. The shared algorithm is specified in the
[armv5 benchmark description](https://github.com/Weichwolf/armv5/blob/a2c7a2b8b7a18eeb3ae49024a1ff8e6ca6e9aca8/test/bench/README.md).

K5 copies 4096 normal words and accumulates an unsigned 32-bit checksum for
512 passes, using scalar fixed ADD, two DAG indices, immediate MODIFY and
software loops. Starting at `0x243f6a88`, the generator advances
`x = (1664525*x + 1013904223) mod 2^32` once per word. The first two words
are replaced by `0x7fffffff` and `1` to force sticky signed overflow. The
last word is `0xc54bba88` and the checksum is `0xd9dd7e00`. Each run checks
all output bytes, unchanged input, guards, pointers, counters, flags and PC.

The drivers encode the authored assembly offline and lend complete 16 GiB
virtual normal-word mappings with separate code storage. K5 marks its regions
ordinary, enabling ordinary accesses in Exact. Direct preserves ordered
volatile accesses. These all-RAM workloads require no fault handler.

## Measurement scope

Each numerical mode gets its own prepared identity, preparation time and native
size. The independent runtime-parameterized algorithms in `reference.cpp` use
`-O3 -fno-lto` and `-march=native` on x86-64. Every run checks their output.
One full guest/reference warm-up precedes three measured pairs.

Counters cover `Core::run` and the small user timer/control wrapper. Wall time
brackets only `run`. Preparation, installation, restore and validation are
outside both. JSONL records paired elapsed times, raw counts and counter
enabled/running deltas. Equal nonzero deltas establish unscaled counts. The
summary derives per-instruction costs and throughput and the paired ratio to
Clang O3, reporting min/median/max. Native size includes all prepared mode
contexts and cold paths. Bytes per prepared instruction divide by the unique
guest instruction count in the table.
