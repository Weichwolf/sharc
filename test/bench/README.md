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

Configure a Release build with Clang 19, then run:

```sh
cmake --build build --target sharc_k4_bench sharc_k5_bench
build/sharc_k4_bench --check
build/sharc_k5_bench --check
build/sharc_k4_bench --check --direct
build/sharc_k5_bench --check --direct
mkdir -p build/local
build/sharc_k4_bench --measure > build/local/k4.jsonl
build/sharc_k5_bench --measure > build/local/k5.jsonl
python3 test/bench/summarize.py build/local/k4.jsonl > build/local/k4-summary.json
python3 test/bench/summarize.py build/local/k5.jsonl > build/local/k5-summary.json
```

Add `--direct` after `--measure` to measure Direct instead of the default Exact
memory contract. Each JSON record names `memory_mode`; keep those results
separate, as well as the Strict/FastFP32 numerical modes. Historical records
without a memory-mode field used Exact. Direct uses the same complete word-space
binding and checked outputs, with ordered volatile accesses. These all-RAM
benchmarks do not expect a host fault and do not install a handler.

`bench_k4` and `bench_k5` are correctness tests in the benchmark layer. They run the complete
kernels through the public interface in Strict and FastFP32 without requiring
counter access. Measurement requires per-thread Linux user hardware counters;
`--measure` fails if its instruction/cycle group cannot run fully and unscaled.
No wall-time estimate substitutes for a counter. The process pins its own thread
to its current allowed CPU. Stop other work owned by the measurement session;
preparation, validation and native-reference execution stay outside guest counts.

## K4 definition

K4 follows the shared 32-bit state-machine definition in the public
[armv5 benchmark description](https://github.com/Weichwolf/armv5/blob/a2c7a2b8b7a18eeb3ae49024a1ff8e6ca6e9aca8/test/bench/README.md).
Starting at state 3 and accumulator `0x2468ace0`, run 1,048,576 transitions.
Mask the state to three bits and use it to select one of eight handler addresses.
The handler adds state to the accumulator, XORs state+1, sets state to
`(state+3) mod 8`, and calls a common subroutine. That subroutine XORs the
accumulator with a 32-bit right rotation of the new state by one and adds 3.
All accumulator arithmetic wraps modulo `2^32`. Both stack frames return before
the loop counter is decremented. Every handler is visited 131,072 times.

The complete algorithm output is `(state, accumulator, last_handler)` =
`(3, 0x24d0ace0, 0)`. Its twelve unsigned little-endian bytes have SHA-256
`490294093fdf6b9e10740daa992391bae19396c6e78bff448f035b4e7ca37f25`.
`control_expectations.py` computes that result and the first eight transitions
using unbounded integers, a literal successor table and bit/quotient rotation.
The separate portable C++ reference uses `std::rotr` and runtime initial state
and transition count. Neither expected-result calculation shares production code.

`k4.asm` is 79 source lines and 61 authored classic ISA instructions. `k4.cpp`
encodes them without an assembler, places main/handlers/common at PM addresses
`0x100/0x200/0x300`, and supplies eight table words at DM `0x90000`. The common
routine rotates by the register count in R6, initialized to -1. This is the
encoding used in the complete-view measurements and the baseline for lowering
comparisons.
Main dispatches indirectly, each handler calls the common
routine directly, and RTS unwinds both architectural PC-stack frames. The
caller's return site is a prepared continuation, not a public entry.

Each transition retires 18 instructions. Its 38 nominal cycles include two
DAG-load stalls at each of the table load and indirect CALL, three bubbles for
each of the four call/return transfers, and the final conditional branch's
three bubbles plus one flag interlock. The last branch saves three cycles and
one final NOP retires. Each complete run therefore checks **18,874,369 guest
instructions** and **39,845,886 base cycles**, the full register file, flags,
all PC-stack cells, both final return words, table contents and surrounding
guards. The complete normal-word DM reservation is made read-only after table
initialization, so any unexpected write fails the process. Code storage is
separate. The benchmark uses a consumer all-RAM mapping.

## K5 definition

The guest copies 4096 normal words and accumulates an unsigned 32-bit running
checksum, repeating 512 times. Saturation and SIMD are disabled. The scalar
kernel is thirteen classic instructions in `k5.asm`, encoded in `kernel()` in
`k5.cpp` for offline preparation without a vendor assembler; it uses two
DAG indices, immediate MODIFY, fixed ADD, counted software loops and conditional
branches. Pointer updates are separated from their next memory use by other
instructions and the loop branch. Hardware loops and post-modify transfers are not implemented yet.
This form measures the existing direct ISA lowering and whole-turn grants;
it does not establish their final performance after those ISA forms are added.

The input generator starts at `0x243f6a88`, advances
`x = (1664525*x + 1013904223) mod 2^32` once per word, then replaces the first
two words by `0x7fffffff` and `1`. These force a known sticky signed overflow
without changing unsigned checksum arithmetic. The complete output is the input
array, serialized as unsigned little-endian words. Its SHA-256 is
`e84b69ed409029f1af79718d2a57b8c0ab264a77e3979a939cb39b8087bdc589`;
the last word is `0xc54bba88`. Independently summing the 4096 words as integers,
multiplying by 512 and reducing modulo `2^32` yields checksum `0xd9dd7e00`.

Each run must retire exactly **14,682,625 guest instructions** and **23,071,742
nominal base cycles**. One inner pass costs `7*4096` instructions and
`11*4096-3` cycles; each outer pass adds five instructions and nine cycles,
except the final outer branch saves three cycles. A final NOP retires once.
Every run checks all output bytes, unchanged input and output guards, checksum,
loop counters, indices, final PC, clock, ASTAT/STKY and public progress.

The consumer reserves a complete 32-bit normal-word space with a 16 GiB lazy
virtual mapping, commits only touched pages, and lends the bytes through Memory.
The K5 variant declares ordinary regions. Exact uses canonical pointers with
ordinary accesses, and Direct retains ordered volatile accesses.
Generated accesses use current SSA indices over that binding. Code storage is
separate. This is an all-RAM benchmark binding, not a model of the chip's memory
map or peripheral protection. No handler, device or firmware asset is involved.

## Measurement scope

Each numeric mode gets its own prepared image, identity, preparation seconds and
native bytes. One full guest/reference warm-up precedes three measured pairs.
The portable C++ algorithms in `reference.cpp` receive runtime parameters; CMake compiles that separate translation unit with `-O3 -fno-lto`.
On x86-64, `-march=native` selects host features, as the LLVM guest code generator
does. It has no shared production implementation, compiler barriers or disabled
optimization. Its complete output and checksum are checked independently.

Both drivers share reservation, counter and timing code in `runner.hpp` and
`counters.hpp`. JSONL also reports native bytes per unique prepared guest
instruction: 61 for K4 and 13 for K5. This denominator counts declared words
once, while native bytes include every prepared mode context.

The JSONL stores every guest wall time, adjacent native-reference time, raw user
instructions/cycles and counter enabled/running times. Counts cover the public
`Core::run` call and the small userspace timer/control wrapper; wall time brackets
only `run`. Preparation, installation, snapshot restore, output clearing and
checks are outside both. Enabled/running are differences across each run because
Linux RESET does not reset their lifetime totals. Equal nonzero times establish
an unscaled group; no count is adjusted or inferred. Kernel and hypervisor work
are excluded. Report min/median/max across the three runs, deriving instructions
and cycles per guest instruction, guest instructions/second, and each paired
ratio to the native-reference time. Native bytes include all prepared mode
contexts and cold paths, not only the executed context.

K4 and K5 execute integer workloads. Each numerical mode gets its own
preparation identity and measurement.
