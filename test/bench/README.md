# Common benchmarks

K1 through K6 have checked Linux x86-64 executions. K3 has passed Strict in
Exact and Direct. The common measurement batch covers FastFP32 and the complete
mode matrix. macOS ARM64 and protected-page peripheral I/O remain open.

## Current Linux results

### K1 and K6

Medians of three unscaled runs on an Intel i5-1135G7 pinned to logical CPU 2,
with Clang/LLVM 19.1.7. The preparation time and native size refer to the shared
image, which is prepared once per numerical and memory mode.

| Kernel | Schedule | Identity | Memory | Host inst./guest inst. | Host cycles/guest inst. | M guest inst./s | Guest/Clang O3 | Prepare s | Native bytes |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| K1 | continuous | Strict | exact | 121.130 | 32.813 | 109.96 | 51.02 | 27.260 | 1,420,189 |
| K1 | bounded | Strict | exact | 128.054 | 35.359 | 102.17 | 56.84 | 27.260 | 1,420,189 |
| K6 | interrupts | Strict | exact | 129.566 | 35.670 | 101.18 | 52.33 | 27.260 | 1,420,189 |
| K1 | continuous | FastFP32 | exact | 121.130 | 33.985 | 106.21 | 48.32 | 28.850 | 1,420,189 |
| K1 | bounded | FastFP32 | exact | 128.054 | 39.638 | 91.32 | 56.27 | 28.850 | 1,420,189 |
| K6 | interrupts | FastFP32 | exact | 129.566 | 36.247 | 99.38 | 57.68 | 28.850 | 1,420,189 |
| K1 | continuous | Strict | direct | 120.810 | 32.457 | 111.51 | 49.27 | 28.817 | 1,404,861 |
| K1 | bounded | Strict | direct | 128.050 | 36.347 | 99.51 | 57.72 | 28.817 | 1,404,861 |
| K6 | interrupts | Strict | direct | 129.585 | 38.168 | 94.80 | 56.82 | 28.817 | 1,404,861 |
| K1 | continuous | FastFP32 | direct | 120.810 | 32.984 | 109.90 | 48.54 | 28.042 | 1,404,861 |
| K1 | bounded | FastFP32 | direct | 128.050 | 35.673 | 101.43 | 56.94 | 28.042 | 1,404,861 |
| K6 | interrupts | FastFP32 | direct | 129.585 | 36.229 | 99.90 | 56.81 | 28.042 | 1,404,861 |

### K4 and K5

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

Configure a Release build with Clang 19. Each benchmark runs Strict and FastFP32
with the same input. The K1 driver also runs the bounded K1 schedule and K6.
Use one logical CPU throughout the serial measurement batch.

```sh
nice -n 10 cmake --build build --target sharc_k1_bench sharc_k2_bench sharc_k3_bench \
  sharc_k4_bench sharc_k5_bench
```

CTest runs the correctness checks without hardware counters.

```sh
nice -n 10 ctest --test-dir build --output-on-failure -R '^bench_k[1-5](_direct)?$'
```

The measurement batch also verifies each run against the reference output.

```sh
mkdir -p build/local/common-kernels
for kernel in 1 2 3 4 5; do
  taskset -c 2 nice -n 10 "build/sharc_k${kernel}_bench" --measure \
    > "build/local/common-kernels/k${kernel}-exact.jsonl" || exit
  taskset -c 2 nice -n 10 "build/sharc_k${kernel}_bench" --measure --direct \
    > "build/local/common-kernels/k${kernel}-direct.jsonl" || exit
done
python3 test/bench/summarize.py build/local/common-kernels/*.jsonl \
  > build/local/common-kernels/summary.json
```

Choose CPU 2 when available or substitute the same allowed CPU in both commands.
The summary checks that the recordings agree on CPU and compiler identity and
that the kernel, schedule, precision and memory mode select a unique result.
Measurements require a fully scheduled, unscaled per-thread Linux hardware
counter group for user instructions and cycles. Stop other work owned by the
measurement session before recording results.

## Workloads

The authored [K1 FIR](k1.asm) runs the shared 64-tap Q1.31 definition for 256
blocks of 1024 samples with continuous history. The host provides the input
and its first 63 zero history words. The guest walks samples backward in DM
and reads coefficients forward from a circular PM buffer. It primes the two
operands and uses a one-word hardware loop for 63 parallel MAC and load
packets. A final MAC, round-to-even and saturation produce each output word.
An independent signed 64-bit C++ dot product checks every output against the
contract SHA-256 `e0dd28045a3afa7150f68203e73f97539933139b0823efd64705cc1c63ea7462`.

The FIR driver runs K1 continuously and with absolute bounds every 256 base
cycles. K6 adds an IRQ2 request every 4096 base cycles and counts each accepted
request in the guest. All three schedules use the same image within a given
numerical and memory mode. K6 timing includes owner bounds, eligibility checks,
steps taken during interrupt deferral and vector entry. The JSON reports calls,
horizons, requests, accepted interrupts and the maximum and sum of acceptance
delays in base cycles. Portable reference timing covers the same FIR arithmetic.

| Kernel | Assembly | Unique guest instructions | Instructions retired | Base cycles | Output SHA-256 |
|---|---|---:|---:|---:|---|
| K1 | [k1.asm](k1.asm) | 18 | 19,661,570 | 20,710,143 | `e0dd28045a3afa7150f68203e73f97539933139b0823efd64705cc1c63ea7462` |
| K2 | [k2.asm](k2.asm) | 38 | 29,098,754 | 29,885,183 | `ab87d136a23ea837637b11274be389f0f31473c1d447d4f6fc752e823b20fe3a` |
| K3 | [k3.asm](k3.asm) | 46 | 13,142,529 | 13,668,862 | Full C++ output comparison |
| K4 | [k4.asm](k4.asm) | 61 | 18,874,369 | 39,845,886 | `490294093fdf6b9e10740daa992391bae19396c6e78bff448f035b4e7ca37f25` |
| K5 | [k5.asm](k5.asm) | 13 | 14,682,625 | 23,071,742 | `e84b69ed409029f1af79718d2a57b8c0ab264a77e3979a939cb39b8087bdc589` |
| K6 | [k1.asm](k1.asm) with IRQ2 | 20 | 19,671,704 | 20,755,746 | Same as K1 |

K1 takes 80,899 owner calls with bounds alone. K6 requests and delivers 5,067
IRQs in 81,078 absolute horizons plus 452 single-instruction calls while
waiting for eligibility. The longest IRQ delay is three base cycles and the
sum is 964. These counts agree across Strict, FastFP32, Exact and Direct.
The native size denominator uses all 20 prepared guest sites for both kernels,
including the IRQ2 handler.

[K2](k2.asm) feeds the same signal through four transposed direct-form II
biquads. The guest loads the two old state words, puts each state contribution
in the signed fractional multiply accumulator and combines it with products
before rounding and saturating. Both updated state words and the stage output
have their own quantization. State persists over all 256 blocks. The independent
C++ implementation checks the full output and the eight final state words
against the contract hashes. The final state SHA-256 is
`7fa60e5ab99a82116e669d32aec3e3af0cba013e494fd3b2af331d553c27e94f`.

[K3](k3.asm) processes 512 copies of a 256-point complex radix-2 transform.
The real and imaginary input components at natural index `i` are
`((i*97+13)&4095)-2048` and `((i*53+7)&4095)-2048`, scaled by 2^16 from
the common Q1.15 input to Q1.31. The owner stages bit-reversed input for both
implementations. The guest walks interleaved complex words in place using eight
PM stage descriptors and Q1.31 twiddles from exp(-2πij/256). Twiddles round to
the nearest integer at scale 2^31 and clip symmetrically to ±(2^31−1). The guest
rounds each complete complex product and divides each butterfly output by two
with nearest-even rounding. Output bins are in natural order with total scale
1/256. The C++ reference iterates twiddles before groups and independently
computes signed products and quantization. Each test compares all 512 words
of every transform.

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

Counters and elapsed time cover the guest call and its owner wrapper. The
bounded schedules include calls to `run_until`, and K6 includes interrupt
delivery. Preparation, installation, restore and validation take place outside
the timed region. JSONL records paired elapsed times, raw counts and counter
enabled/running deltas. Equal nonzero deltas establish unscaled counts. The
summary derives per-instruction costs, throughput and the paired ratio to
Clang O3, reporting min/median/max. Native size includes all prepared mode
contexts and cold paths. Bytes per prepared instruction divide by the number
of guest instruction sites in the prepared image.
