# Linux immediate ROT qualification

Measured clean published revision `0c5ef636dc1302eeab625a16d79636d9dac27019` on CPU 4,
`perf_event_paranoid = 2`, Linux x86-64 i5-10400. Each of the four K4/K5 and
Exact/Direct configurations has three warm runs in each numerical mode: 24 fully
running, unscaled user instruction/cycle groups, with complete output, state
and progress checks. No own build, test or probe ran concurrently; the shared
host was not isolated and unrelated developer activity continued.

K4 uses about 75.00005 host instructions/GI, down **4.09%** from 78.19450.
Native text falls by 28,704 bytes to 1,150,208. K5 instruction costs and native
size remain effectively unchanged; the tables retain every run and comparison.

K4 now uses standalone immediate `ROT R0 BY -1` in its common subroutine
(destination R2), replacing the register-count form. R6 no longer needs initial
-1; it retains its zero initial value. The algorithm, all 61 declared instruction
locations, 18,874,369 retired guest instructions and 39,845,886 nominal cycles
are unchanged. This compares two authored programs for the same K4 algorithm,
with the new form exposing its constant count directly to LLVM. The core uses
the same rotation, predicate, flag and writeback lowering for both count forms.
The portable reference and K5 program are unchanged.

Preparation uses the standalone LLVM O3 pipeline with Aggressive native code
generation; the separate portable reference uses Clang O3, `-fno-lto` and
`-march=native`. Every three-value cell is **minimum / median / maximum**.
MGI/s means million guest instructions per second; Guest/C++ is the paired
guest/reference time ratio. Preparation includes lowering, optimization, native
code and fault-map collection. Native bytes include all contexts and cold paths.

| Kernel | Numerical mode | Memory | Host instructions/GI | Host cycles/GI | MGI/s | Guest/C++ | Prepare s | Native bytes |
|---|---|---|---:|---:|---:|---:|---:|---:|
| K4 | Strict | exact | 75.000051181 / 75.000051234 / 75.000051286 | 26.292365 / 26.300568 / 26.351373 | 151.607 / 151.645 / 151.673 | 145.309 / 145.759 / 145.780 | 11.738762 | 1150208 |
| K4 | FastFP32 | exact | 75.000051181 / 75.000051234 / 75.000051234 | 26.421875 / 26.459771 / 26.473817 | 150.958 / 150.964 / 151.045 | 145.867 / 146.404 / 149.973 | 11.722416 | 1150208 |
| K4 | Strict | direct | 75.000054677 / 75.000054677 / 75.000054730 | 26.181007 / 26.205601 / 26.238914 | 152.650 / 152.982 / 154.026 | 138.499 / 144.211 / 147.081 | 11.668629 | 1150208 |
| K4 | FastFP32 | direct | 75.000054677 / 75.000054730 / 75.000054889 | 26.194904 / 26.232379 / 26.270176 | 152.479 / 152.506 / 152.784 | 144.649 / 144.956 / 148.471 | 11.614718 | 1150208 |
| K5 | Strict | exact | 13.862871115 / 13.862871183 / 13.862871251 | 3.906823 / 3.908178 / 3.913527 | 1019.194 / 1020.604 / 1020.912 | 120.950 / 121.081 / 121.444 | 2.402381 | 128247 |
| K5 | FastFP32 | exact | 13.862871183 / 13.862871183 / 13.862871251 | 3.903463 / 3.906679 / 3.922060 | 1015.280 / 1027.603 / 1033.532 | 122.480 / 122.890 / 124.105 | 2.403870 | 128247 |
| K5 | Strict | direct | 13.862875678 / 13.862875678 / 13.862875746 | 3.900779 / 3.909635 / 3.913930 | 1024.490 / 1033.583 / 1043.669 | 97.479 / 122.475 / 123.604 | 2.403834 | 128247 |
| K5 | FastFP32 | direct | 13.862875610 / 13.862875746 / 13.862875746 | 3.909175 / 3.915822 / 3.916329 | 1017.357 / 1018.321 / 1020.264 | 120.963 / 121.020 / 121.188 | 2.412364 | 128247 |

Comparison with the [complete-view baseline](linux-complete-views.md), measured
at published `e40d00f`:

| Kernel | Numerical mode | Memory | Previous host instructions/GI | Instruction change | Native byte change |
|---|---|---|---:|---:|---:|
| K4 | Strict | exact | 78.194495880 | -4.085255% | -28704 |
| K4 | FastFP32 | exact | 78.194495562 | -4.085255% | -28704 |
| K4 | Strict | direct | 78.194499111 | -4.085255% | -28704 |
| K4 | FastFP32 | direct | 78.194499058 | -4.085255% | -28704 |
| K5 | Strict | exact | 13.862871251 | <0.000001% | +0 |
| K5 | FastFP32 | exact | 13.862871183 | <0.000001% | +0 |
| K5 | Strict | direct | 13.862875678 | <0.000001% | +0 |
| K5 | FastFP32 | direct | 13.862875678 | <0.000001% | +0 |

All measured runs are retained. Enabled/running times are per-thread counter
times; wall times include descheduling on the shared host. Their difference
must not be interpreted as numerical-mode performance. These are integer-only
kernels; no floating qualification or FastFP32 speedup is established.

The implementation passes 16/16 tests in main, source-only and diagnostic-poison
builds, plus installed SDK, production-only build, equal preparation identities,
formatting and configured static analysis. All 256 immediate ROT encodings are
checked against an independent bit permutation in both numerical identities,
with full-unit and exact-prefix execution, predicates, banks, SIMD, raw storage,
flags, snapshots and branch interlocks. Parallel memory forms and other shifter
operations remain open. S3 is incomplete; K1–K3, K6 and macOS ARM64 are unqualified.
Implementation line delta: `src/ +35` (4,823), `test/ +85` (12,281).

Raw runs: [K4 Exact](linux-immediate-rotate-k4-exact.jsonl),
[K4 Direct](linux-immediate-rotate-k4-direct.jsonl),
[K5 Exact](linux-immediate-rotate-k5-exact.jsonl),
[K5 Direct](linux-immediate-rotate-k5-direct.jsonl). The
[host receipt](linux-immediate-rotate-host.json) records revision, scope and
source, binary and raw-file SHA-256 values.
