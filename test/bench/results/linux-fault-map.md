# Linux fault-map qualification

Measured on the same Linux x86-64 host and CPU 4 as the [first K4 baseline](linux-k4.md).
Both revisions were clean, published `main` when measured; no own build, test or probe ran
concurrently. The host was not isolated from other development. Each mode has three warm,
checked runs with pinned, unscaled user instruction/cycle groups. Commands, source/binary
hashes, image identities and all raw counts are in the receipts below.

`be24705` added copied native text ranges and guest line tables but forced RTDyld instead
of this host’s default JITLink. This increased executed host instructions and native bytes.
`72eaf9a` retains LLVM’s default linker and obtains attribution through linker notifications
and relocated debug objects. K4 and K5 return to the `fd44316` instruction counts and native
sizes. Preparation includes the new cold metadata work; elapsed times remain host-sensitive.

Each three-value cell is **minimum / median / maximum**. MGI/s means million guest
instructions per second. Guest/C++ is guest elapsed time divided by the separate Clang O3
reference elapsed time. The reference uses the same algorithm, `-fno-lto` and `-march=native`.

| Linker / revision | Kernel | Mode | Host instructions/GI | Host cycles/GI | MGI/s | Guest/C++ | Prepare s | Native bytes |
|---|---|---|---:|---:|---:|---:|---:|---:|
| Forced RTDyld `be24705` | K4 | Strict | 79.777835328 / 79.777836070 / 79.777836811 | 28.280499 / 28.319094 / 28.331398 | 126.345 / 127.791 / 127.867 | 157.990 / 164.006 / 165.127 | 13.347505 | 1216190 |
| Forced RTDyld `be24705` | K4 | FastFP32 | 79.777831036 / 79.777831036 / 79.777832096 | 28.356920 / 28.379782 / 28.393238 | 128.157 / 128.477 / 128.935 | 158.275 / 158.545 / 162.756 | 13.298185 | 1216190 |
| Forced RTDyld `be24705` | K5 | Strict | 14.292168737 / 14.292169009 / 14.292169146 | 3.924112 / 3.929725 / 3.939727 | 960.283 / 961.263 / 963.742 | 114.224 / 119.304 / 122.622 | 2.675390 | 134518 |
| Forced RTDyld `be24705` | K5 | FastFP32 | 14.292168737 / 14.292168873 / 14.292168873 | 3.911426 / 3.911589 / 3.923220 | 965.435 / 968.496 / 968.529 | 117.308 / 118.488 / 118.714 | 2.634137 | 134518 |
| Default JITLink `72eaf9a` | K4 | Strict | 78.194494873 / 78.194494873 / 78.194494926 | 26.941469 / 26.955764 / 26.983258 | 148.558 / 150.654 / 150.668 | 141.120 / 150.346 / 152.491 | 11.566799 | 1178592 |
| Default JITLink `72eaf9a` | K4 | FastFP32 | 78.194494767 / 78.194494820 / 78.194494926 | 26.931605 / 26.949802 / 26.950073 | 150.381 / 150.918 / 151.082 | 149.156 / 150.115 / 150.615 | 11.515334 | 1178592 |
| Default JITLink `72eaf9a` | K5 | Strict | 13.720002929 / 13.720002997 / 13.720003065 | 3.851034 / 3.864191 / 3.867657 | 1040.279 / 1047.671 / 1061.602 | 120.038 / 120.106 / 122.000 | 2.350831 | 125949 |
| Default JITLink `72eaf9a` | K5 | FastFP32 | 13.720002997 / 13.720003065 / 13.720003337 | 3.863035 / 3.863877 / 3.869814 | 408.729 / 886.332 / 1033.945 | 82.906 / 93.420 / 123.537 | 2.364603 | 125949 |

Native density uses 61 unique prepared instructions for K4 and 13 for K5. Across all
prepared contexts, the corrected images contain 19,321.180 and 9,688.385 native bytes
per unique guest instruction respectively; the forced-RTDyld values were 19,937.541
and 10,347.538. These include cold paths and every prepared MODE1 context.

An independent consumer pipeline compiled both complete kernels twice: retaining guest
debug locations and stripping them before O3. Both used LLVM’s default linker.
Relocatable executable text before linking was byte-identical for each pair in both modes:

| Kernel | Native text bytes | SHA-256 (both modes, with/without guest debug locations) |
|---|---:|---|
| K4 | 1178592 | `10b6fe660628ed780b8def62737abc6feec1a8b1ced1635db4ea4aa968a28395` |
| K5 | 125949 | `2542fa58b17d0bdc7c5eae996dfd7d24a27b7ac0206004052de7df7b03e66cb6` |

Main, source-only export and diagnostic-poison suites pass 11/11 at `72eaf9a`; the
installed SDK, production-only build, compiler-identity comparison, format and configured
static analysis pass. This is integer-only qualification. Strict floating point, the
complete ISA/VISA profile, K1–K3/K6 and macOS ARM64 remain open. The older K5 instruction
cost increase relative to `5017f6c` is still open. Direct/Exact selection and `host_fault`
are not implemented by these increments.

- Forced linker: [raw runs](linux-fault-map-be24705.jsonl), [host/source receipt](linux-fault-map-be24705-host.json).
- Corrected linker: [raw runs](linux-fault-map-72eaf9a.jsonl), [host/source receipt](linux-fault-map-72eaf9a-host.json).
- Reproduce all derived ranges with `python3 test/bench/summarize.py <raw-runs.jsonl>`.
