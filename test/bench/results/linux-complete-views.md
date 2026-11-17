# Linux complete-space read/write view qualification

Revision `e40d00f86fb416fa48a751e1dc7de3fa4b8fac44` was clean, published `main` at measurement start.
Linux x86-64 i5-10400, CPU 4, `perf_event_paranoid = 2`; three warm runs per
numerical/memory configuration, 24 fully running unscaled user instruction/cycle
groups. Each run checks the complete output, state and progress. No own build,
test or probe ran concurrently. The host was not isolated; unrelated work continued.

The kernels retain their canonical all-RAM reservations; their read and write
bases point to the same mapping. The new native binding can also select separate
consumer-lent bases. Independent installed-image tests use distinct read-only
read and writable write mappings, including protected-write faults. These kernel
measurements quantify the cost of supporting separate bases on the existing
bindings. They do not measure device I/O or whole-instrument throughput.

Both Exact and Direct now use typed volatile normal-word accesses, preserving
physical aliases behind different virtual addresses. Registers and flags remain
SSA. `Core::compile` retains LLVM O3 and Aggressive native code generation;
the separate portable reference retains Clang O3, `-fno-lto -march=native`.

Every three-value cell is **minimum / median / maximum**. MGI/s means million
guest instructions per second; Guest/C++ is the paired guest/reference time
ratio. Preparation includes lowering, optimization, code generation and fault
map collection. Native bytes include every prepared context and cold path.

| Kernel | Numerical mode | Memory | Host instructions/GI | Host cycles/GI | MGI/s | Guest/C++ | Prepare s | Native bytes |
|---|---|---|---:|---:|---:|---:|---:|---:|
| K4 | Strict | exact | 78.194495827 / 78.194495880 / 78.194495933 | 27.488508 / 27.528037 / 27.552148 | 64.634 / 64.749 / 65.685 | 306.473 / 310.911 / 315.330 | 18.492096 | 1178912 |
| K4 | FastFP32 | exact | 78.194495509 / 78.194495562 / 78.194495615 | 27.171301 / 27.226283 / 27.235623 | 143.265 / 144.047 / 145.651 | 147.909 / 149.836 / 153.436 | 12.745544 | 1178912 |
| K4 | Strict | direct | 78.194499111 / 78.194499111 / 78.194499164 | 27.537822 / 27.548633 / 27.644950 | 144.240 / 144.756 / 144.812 | 152.599 / 152.662 / 153.225 | 11.981105 | 1178912 |
| K4 | FastFP32 | direct | 78.194499006 / 78.194499058 / 78.194499058 | 27.055225 / 27.058363 / 27.083811 | 147.252 / 147.396 / 147.414 | 149.912 / 149.944 / 150.091 | 11.991619 | 1178912 |
| K5 | Strict | exact | 13.862871115 / 13.862871251 / 13.862871319 | 3.909687 / 3.912230 / 3.915299 | 1018.586 / 1019.324 / 1020.072 | 120.482 / 120.591 / 120.911 | 2.435185 | 128247 |
| K5 | FastFP32 | exact | 13.862871115 / 13.862871183 / 13.862871251 | 3.908874 / 3.910002 / 3.913434 | 1019.005 / 1019.815 / 1020.360 | 121.198 / 121.343 / 121.879 | 2.447666 | 128247 |
| K5 | Strict | direct | 13.862875610 / 13.862875678 / 13.862875746 | 3.908272 / 3.912002 / 3.915798 | 1018.377 / 1019.346 / 1020.661 | 119.089 / 119.910 / 120.279 | 2.449666 | 128247 |
| K5 | FastFP32 | direct | 13.862875610 / 13.862875678 / 13.862875746 | 3.912523 / 3.913506 / 3.921804 | 1016.605 / 1018.842 / 1019.384 | 120.286 / 120.494 / 120.610 | 2.447612 | 128247 |

K4 Strict Exact has 287–292 ms wall time but only about 143 ms enabled/running
counter time, consistent with descheduling on the shared host. Its instruction
count remains stable. All runs are retained; the large wall-time/reference ratio
does not establish a difference between the numerical modes.

Against the [published single-base baseline](linux-host-fault.md), K4 retains
about 78.1945 host instructions/GI. K5 increases from about 13.7200 to 13.8629
(**+1.04%**): about one extra host instruction per seven-instruction inner turn.
The inspected native K5 loop reloads the read base from a stack slot while
keeping the write base in a register. This cost also affects ordinary bindings
whose two bases happen to be equal. Cycle and wall-time ranges retain host noise;
the instruction increase is not a claim of an equal timing regression.

K4 native text is 1,178,912 bytes in both memory modes (Exact +320, Direct
unchanged). K5 is 128,247 bytes (Exact +2,298, Direct +2,181). Both numerical
modes execute integer algorithms here; these results do not qualify floating
arithmetic or establish a FastFP32 speedup. K1–K3, K6 and macOS ARM64 remain open.

The implementation passes all 16 tests in the main, source-only and diagnostic
poison builds, plus the installed two-header SDK, production-only build, equal
preparation identities, formatting and configured static analysis. The source
export contains no Git metadata or local reference material. Implementation
line delta: `src/ +91` (4,788), `test/ +322` (12,057). S3 remains incomplete.

Raw runs: [K4 Exact](linux-complete-views-k4-exact.jsonl),
[K4 Direct](linux-complete-views-k4-direct.jsonl),
[K5 Exact](linux-complete-views-k5-exact.jsonl),
[K5 Direct](linux-complete-views-k5-direct.jsonl). The
[host and provenance receipt](linux-complete-views-host.json) records source,
binary and raw-file SHA-256 values, measurement scope and the exact revision.
