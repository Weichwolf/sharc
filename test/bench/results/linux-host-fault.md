# Linux terminal host-fault qualification

Revision `4dcd542` was clean, published `main` during measurement on the Linux
x86-64 i5-10400, CPU 4, `perf_event_paranoid = 2`. Each configuration has three
warm runs per numerical mode, complete output/state/progress checks and fully
running, unscaled user instruction/cycle groups. No own build, test or probe ran
concurrently. Other development continued; the host was not isolated.

Exact and Direct execute the same K4/K5 programs over a complete all-RAM word
space. These valid accesses already have constant address/permission proofs in
Exact. Direct introduces terminal fault recovery and volatile guest accesses;
it does not remove significant admission work from these two bindings. The
instruction counts remain essentially unchanged. Ordinary Direct and volatile
Direct also have the same executed instruction cost at this scope; cycle and
wall-time variation do not establish a speed advantage. This is no claim about
unimplemented kernels, floating computation or other memory views.

The **ordinary counterfactual is unsound**, and is not a production option or
acceptance mode. In an ignored copy of the qualified source, it changes only
three `setVolatile(observable)` calls in `src/lower.cpp` to `setVolatile(false)`
and marks the now-unused local `observable` constant `[[maybe_unused]]`.
All other production and benchmark source files match the qualified source. Its own compiler identity,
source hashes and binary hashes distinguish it in the receipt. All-RAM kernels
can pass while the independently authored protected-access tests fail: a dead
load can disappear, or an overwritten store can cease to be the first fault.
Only the volatile Direct form qualifies for arbitrary protected views.

Every three-value cell is **minimum / median / maximum**. MGI/s means million
guest instructions per second; Guest/C++ divides guest time by the separate
portable Clang O3 reference time (`-fno-lto -march=native`). Preparation includes
lowering, O3, native compilation and fault-map collection. Native bytes include
all prepared contexts and cold paths; density denominators are 61 unique guest
instructions for K4 and 13 for K5.

| Kernel | Numerical mode | Access form | Host instructions/GI | Host cycles/GI | MGI/s | Guest/C++ | Prepare s | Native bytes |
|---|---|---|---:|---:|---:|---:|---:|---:|
| K4 | Strict | exact | 78.194495509 / 78.194495562 / 78.194495562 | 27.083509 / 27.084072 / 27.089780 | 147.208 / 147.296 / 147.338 | 148.079 / 150.084 / 153.808 | 11.908823 | 1178592 |
| K4 | FastFP32 | exact | 78.194495509 / 78.194495509 / 78.194495562 | 27.005711 / 27.012425 / 27.015630 | 147.524 / 148.218 / 148.925 | 140.057 / 148.411 / 150.474 | 11.840768 | 1178592 |
| K4 | Strict | direct-volatile | 78.194499006 / 78.194499058 / 78.194499058 | 27.003366 / 27.025168 / 27.084953 | 147.453 / 148.082 / 148.158 | 144.986 / 149.176 / 153.629 | 11.775076 | 1178912 |
| K4 | FastFP32 | direct-volatile | 78.194499058 / 78.194499111 / 78.194499164 | 27.036577 / 27.145044 / 27.146739 | 146.893 / 146.924 / 147.662 | 149.681 / 150.440 / 150.458 | 11.839136 | 1178912 |
| K4 | Strict | direct-ordinary-counterfactual | 78.194499006 / 78.194499058 / 78.194499058 | 27.002633 / 27.052339 / 27.059852 | 147.920 / 148.169 / 148.188 | 147.621 / 149.421 / 152.865 | 11.917836 | 1178592 |
| K4 | FastFP32 | direct-ordinary-counterfactual | 78.194499058 / 78.194499058 / 78.194499058 | 27.064406 / 27.103270 / 27.109846 | 147.142 / 147.366 / 147.460 | 149.950 / 149.967 / 153.631 | 11.871040 | 1178592 |
| K5 | Strict | exact | 13.720003814 / 13.720003882 / 13.720003950 | 3.856035 / 3.859222 / 3.870231 | 1043.949 / 1047.378 / 1050.818 | 118.928 / 120.231 / 120.815 | 2.394026 | 125949 |
| K5 | FastFP32 | exact | 13.720003882 / 13.720003950 / 13.720003950 | 3.864240 / 3.866222 / 3.869978 | 1028.623 / 1030.510 / 1032.261 | 115.472 / 116.249 / 116.542 | 2.396480 | 125949 |
| K5 | Strict | direct-volatile | 13.720008309 / 13.720008377 / 13.720008445 | 3.865856 / 3.868174 / 3.876127 | 1028.846 / 1030.970 / 1031.739 | 116.680 / 117.259 / 117.276 | 2.422397 | 126066 |
| K5 | FastFP32 | direct-volatile | 13.720008309 / 13.720008377 / 13.720008445 | 3.898752 / 3.899129 / 3.906996 | 655.931 / 1020.047 / 1022.200 | 117.588 / 117.989 / 180.674 | 2.441019 | 126066 |
| K5 | Strict | direct-ordinary-counterfactual | 13.720008513 / 13.720008718 / 13.720008922 | 3.911482 / 3.917133 / 3.925187 | 557.878 / 763.122 / 831.596 | 145.697 / 159.975 / 217.762 | 2.956659 | 125949 |
| K5 | FastFP32 | direct-ordinary-counterfactual | 13.720008377 / 13.720008377 / 13.720008445 | 3.897392 / 3.899853 / 3.929339 | 1014.525 / 1021.925 / 1022.959 | 118.864 / 120.419 / 122.105 | 2.598230 | 125949 |

Default-linker comparisons retain or strip guest debug information before O3.
Both forms produce identical native text in each numerical/memory mode. Exact
also matches the previously published text; ordered Direct adds 320 native bytes
for K4 and 117 for K5, across every prepared context. Both numerical modes are
integer-only here and produce the same text within each memory contract.

| Kernel | Memory contract | Native text bytes | SHA-256 (both numerical modes, with/without debug information) |
|---|---|---:|---|
| K4 | Exact | 1178592 | `10b6fe660628ed780b8def62737abc6feec1a8b1ced1635db4ea4aa968a28395` |
| K4 | Direct | 1178912 | `d7f216025813283132ebf30076c4d3e5d8df6451562fb697f9ff847d828ffc2f` |
| K5 | Exact | 125949 | `2542fa58b17d0bdc7c5eae996dfd7d24a27b7ac0206004052de7df7b03e66cb6` |
| K5 | Direct | 126066 | `5487170561679ee52bf6e22544ec5a24568ca6a8e278dc9cadac37100abe57bf` |

Main, source-only and diagnostic-poison suites pass 14/14. Installed SDK,
production-only build, equal compiler identities, format and configured static
analysis pass. GDB observes zero `rt_sigprocmask` stops across 1,000 successful
Direct owner calls. Actual SIGSEGV/SIGBUS fixtures independently verify terminal
attribution, ordered paired effects, mask restoration, chaining and ownership.
These statements do not promise a coherent terminal register snapshot.

The rebased publication adds only upstream policy/contract documents and a
coverage note to the qualified sources. `src/` totals 4,697 (+292) and `test/`
10,515 (+850) in the code increment. K1–K3/K6, Strict floating computation,
complete ISA/VISA coverage, v10 complete read/write views and parallel preparation,
and macOS ARM64 qualification remain open. The older K5 instruction-cost increase
against `5017f6c` and the large guest/C++ gap remain open. Historical baselines
are retained unchanged.

[Host, source, binary and qualification receipt](linux-host-fault-4dcd542-host.json).
Recompute each range with `python3 test/bench/summarize.py <raw-runs.jsonl>`.

- K4, exact: [raw runs](linux-host-fault-4dcd542-k4-exact.jsonl).
- K4, direct-volatile: [raw runs](linux-host-fault-4dcd542-k4-direct-volatile.jsonl).
- K4, direct-ordinary-counterfactual: [raw runs](linux-host-fault-4dcd542-k4-direct-ordinary-counterfactual.jsonl).
- K5, exact: [raw runs](linux-host-fault-4dcd542-k5-exact.jsonl).
- K5, direct-volatile: [raw runs](linux-host-fault-4dcd542-k5-direct-volatile.jsonl).
- K5, direct-ordinary-counterfactual: [raw runs](linux-host-fault-4dcd542-k5-direct-ordinary-counterfactual.jsonl).
