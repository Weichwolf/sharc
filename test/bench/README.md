# Common benchmarks

K1 through K6 have checked Linux x86-64 results in Strict and FastFP32 with
Exact and Direct memory. macOS ARM64 and protected-page peripheral I/O remain open.

## Current Linux results

Medians of three unscaled runs on an Intel i5-1135G7 pinned to logical CPU 2,
with Clang/LLVM 19.1.7 at measured revision `cdd517e`. K1 continuous, K1 bounded
and K6 reuse the same prepared image for each numerical and memory mode.

| Kernel | Schedule | Identity | Memory | Host inst./guest inst. | Host cycles/guest inst. | M guest inst./s | Guest/Clang O3 | Prepare s | Native bytes |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| K1 | continuous | Strict | exact | 121.130 | 31.997 | 113.92 | 51.28 | 25.966 | 1,420,189 |
| K1 | continuous | Strict | direct | 120.810 | 32.382 | 111.84 | 47.66 | 28.098 | 1,404,861 |
| K1 | continuous | FastFP32 | exact | 121.130 | 32.050 | 112.87 | 51.06 | 27.872 | 1,420,189 |
| K1 | continuous | FastFP32 | direct | 120.810 | 32.803 | 110.37 | 42.05 | 25.890 | 1,404,861 |
| K1 | bounded | Strict | exact | 128.054 | 35.005 | 104.10 | 59.24 | 25.966 | 1,420,189 |
| K1 | bounded | Strict | direct | 128.050 | 36.082 | 100.34 | 60.32 | 28.098 | 1,404,861 |
| K1 | bounded | FastFP32 | exact | 128.054 | 34.539 | 104.81 | 54.50 | 27.872 | 1,420,189 |
| K1 | bounded | FastFP32 | direct | 128.050 | 35.558 | 102.05 | 56.07 | 25.890 | 1,404,861 |
| K2 | continuous | Strict | exact | 65.432 | 17.776 | 181.69 | 24.89 | 190.149 | 5,277,010 |
| K2 | continuous | Strict | direct | 65.576 | 19.150 | 170.78 | 26.52 | 209.526 | 5,301,851 |
| K2 | continuous | FastFP32 | exact | 65.432 | 18.105 | 177.87 | 25.03 | 208.633 | 5,277,010 |
| K2 | continuous | FastFP32 | direct | 65.576 | 18.732 | 170.88 | 26.73 | 205.044 | 5,301,851 |
| K3 | continuous | Strict | exact | 57.326 | 15.098 | 241.39 | 16.39 | 958.294 | 22,847,601 |
| K3 | continuous | Strict | direct | 57.326 | 15.448 | 236.17 | 16.99 | 918.490 | 22,706,971 |
| K3 | continuous | FastFP32 | exact | 57.326 | 27.200 | 133.30 | 23.36 | 953.960 | 22,847,601 |
| K3 | continuous | FastFP32 | direct | 57.326 | 15.405 | 234.21 | 16.93 | 826.272 | 22,706,971 |
| K4 | continuous | Strict | exact | 24.278 | 5.745 | 629.81 | 39.59 | 6.193 | 361,783 |
| K4 | continuous | Strict | direct | 24.278 | 5.543 | 652.82 | 38.73 | 6.165 | 361,783 |
| K4 | continuous | FastFP32 | exact | 24.278 | 5.648 | 639.07 | 38.22 | 6.161 | 361,783 |
| K4 | continuous | FastFP32 | direct | 24.278 | 5.775 | 625.04 | 39.91 | 6.122 | 361,783 |
| K5 | continuous | Strict | exact | 7.863 | 2.042 | 1763.21 | 71.22 | 1.159 | 55,540 |
| K5 | continuous | Strict | direct | 7.863 | 2.040 | 1763.77 | 68.75 | 1.124 | 55,609 |
| K5 | continuous | FastFP32 | exact | 7.863 | 2.150 | 1679.50 | 74.34 | 1.134 | 55,540 |
| K5 | continuous | FastFP32 | direct | 7.863 | 2.039 | 1774.06 | 67.21 | 1.092 | 55,609 |
| K6 | interrupts | Strict | exact | 129.566 | 36.772 | 99.13 | 59.22 | 25.966 | 1,420,189 |
| K6 | interrupts | Strict | direct | 129.585 | 36.215 | 99.93 | 54.47 | 28.098 | 1,404,861 |
| K6 | interrupts | FastFP32 | exact | 129.566 | 35.957 | 100.53 | 58.02 | 27.872 | 1,420,189 |
| K6 | interrupts | FastFP32 | direct | 129.585 | 35.779 | 101.14 | 57.71 | 25.890 | 1,404,861 |

K3 preparation took 826.3 to 958.3 seconds per image, with 22,706,971 to
22,847,601 native bytes. The FastFP32 Exact measurement for this fixed-point
workload recorded 27.200 host cycles per guest instruction, compared with
15.098 to 15.448 across the other three K3 cases. The cause is open.

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
