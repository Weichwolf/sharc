# Common benchmarks

K5 is implemented. K1–K4 and K6 remain open; this is not complete S3 or a full
profile performance result. The current driver targets Linux with Clang. macOS
ARM64 and protected-page I/O are untested.

Configure a Release build with Clang 19, then run:

```sh
cmake --build build --target sharc_k5_bench
build/sharc_k5_bench --check
mkdir -p build/local
build/sharc_k5_bench --measure > build/local/k5.jsonl
python3 test/bench/summarize.py build/local/k5.jsonl > build/local/k5-summary.json
```

`bench_k5` is a correctness test in the benchmark layer. It runs the complete
kernel through the public interface in Strict and FastFP32 without requiring
counter access. Measurement requires per-thread Linux user hardware counters;
`--measure` fails if its instruction/cycle group cannot run fully and unscaled.
No wall-time estimate substitutes for a counter. The process pins its own thread
to its current allowed CPU. Stop other work owned by the measurement session;
preparation, validation and native-reference execution stay outside guest counts.

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
Generated accesses use current SSA indices over that binding. Code storage is
separate. This is an all-RAM benchmark binding, not a model of the chip's memory
map or peripheral protection. No handler, device or firmware asset is involved.

## Measurement scope

Each numeric mode gets its own prepared image, identity, preparation seconds and
native bytes. One full guest/reference warm-up precedes three measured pairs.
The portable C++ copy/checksum in `reference.cpp` receives runtime spans and a
pass count; CMake compiles that separate translation unit with `-O3 -fno-lto`.
On x86-64, `-march=native` selects host features, as the LLVM guest code generator
does. It has no shared production implementation, compiler barriers or disabled
optimization. Its complete output and checksum are checked independently.

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

K5 is an integer workload: the two numerical modes have the same arithmetic
contract here. Separate measurements do not claim a floating-point speedup,
floating qualification, or consumer whole-instrument throughput. The first [Linux K5 baseline](results/linux-k5.md) includes raw counts,
min/median/max metrics and the host/source receipt.
