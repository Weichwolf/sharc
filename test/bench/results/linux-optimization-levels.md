# Linux LLVM IR optimisation-level experiment

The available K4/K5 kernels pass at O1, O2, O3 and Os, in both numerical and
memory contracts: 16 configurations, 32 records and 96 checked, unscaled hardware
groups. Every run checks the complete output, state and progress against the
same independent expectations. K1–K3 and K6 remain unimplemented; this is a
partial kernel experiment, not a complete profile or six-kernel acceptance.

No one level wins across these results. O1 reduces K4 host instructions by about
1.0% and native text by about 2.7% against O3. Os reduces K5 host instructions by
about 1.0% and text by about 17.1%, but increases K4 instruction cost by about 4.3%
and its text size. O2/O3 have essentially equal instruction counts and sizes at
this scope. Cycle and wall results are retained as measured on a shared host;
these samples do not establish a universal default. No production default is
changed by this data increment.

Only the final **LLVM IR default pipeline** changes. O3 uses clean published
`9b7e95e`; ignored copies of that source replace the single
`buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3)` argument in
`src/image.cpp` with O1, O2 or Os. All other production and benchmark source
files match. Native code generation stays `CodeGenOptLevel::Aggressive`, the host
library stays Release, and the separate portable C++ reference stays Clang O3
with `-fno-lto -march=native`. No per-function attributes, custom pipeline or
profile-guided optimisation are introduced. The receipt records each distinct
compiler identity, changed-source hash, binary hash and command.

Measurements use the Linux x86-64 i5-10400, CPU 4, `perf_event_paranoid = 2`, three
warm runs per record. User instructions/cycles run together without scaling;
preparation, output checks and reference execution are outside the guest group.
No own build, test or probe runs concurrently. Other development may continue;
the host is not isolated. Each driver prepares Strict before FastFP32, so the
first preparation also includes its process's initial LLVM setup. Preparation
seconds are individual observations, not a repeated timing distribution.

Every three-value cell is **minimum / median / maximum**. MGI/s means million
guest instructions per second; Guest/C++ is guest elapsed time divided by the
paired native-reference elapsed time. Native bytes include all prepared contexts
and cold paths. There are 61 unique prepared guest instructions for K4 and 13
for K5. Both numerical modes execute integer-only kernels here; these results do
not qualify Strict floating computation or FastFP32 approximation.

| Kernel | Numerical mode | Memory | IR level | Host instructions/GI | Host cycles/GI | Native bytes | Prepare s | MGI/s | Guest/C++ |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| K4 | Strict | exact | O1 | 77.409773381 / 77.409773487 / 77.409773487 | 26.792447 / 26.858386 / 26.860921 | 1147040 | 13.000015 | 139.822 / 140.146 / 141.279 | 145.655 / 148.230 / 149.738 |
| K4 | FastFP32 | exact | O1 | 77.409773487 / 77.409773540 / 77.409773858 | 26.763140 / 26.864027 / 26.872636 | 1147040 | 12.087571 | 138.933 / 139.179 / 140.288 | 146.348 / 148.914 / 150.822 |
| K4 | Strict | exact | O2 | 78.194495615 / 78.194495721 / 78.194495774 | 27.132129 / 27.143857 / 27.151577 | 1178592 | 13.078014 | 136.901 / 137.154 / 137.741 | 149.066 / 152.094 / 153.356 |
| K4 | FastFP32 | exact | O2 | 78.194495562 / 78.194495562 / 78.194495615 | 26.968459 / 26.989830 / 26.996878 | 1178592 | 12.190472 | 147.890 / 147.902 / 148.492 | 148.819 / 149.417 / 153.178 |
| K4 | Strict | exact | O3 | 78.194495509 / 78.194495562 / 78.194495562 | 26.986589 / 26.999504 / 27.002591 | 1178592 | 12.430657 | 147.485 / 147.695 / 148.507 | 148.824 / 149.633 / 153.597 |
| K4 | FastFP32 | exact | O3 | 78.194495509 / 78.194495562 / 78.194495562 | 27.040432 / 27.045175 / 27.112944 | 1178592 | 11.864229 | 147.081 / 147.554 / 148.391 | 138.998 / 148.935 / 150.259 |
| K4 | Strict | exact | Os | 81.527829089 / 81.527829089 / 81.527829089 | 28.088571 / 28.096279 / 28.123832 | 1199408 | 11.074790 | 143.819 / 144.008 / 144.722 | 156.485 / 156.793 / 157.523 |
| K4 | FastFP32 | exact | Os | 81.527829672 / 81.527829672 / 81.527829831 | 28.461926 / 28.565302 / 28.595354 | 1199408 | 12.067659 | 129.139 / 133.977 / 135.194 | 163.443 / 164.927 / 170.968 |
| K4 | Strict | direct | O1 | 77.409776878 / 77.409776878 / 77.409777090 | 27.188569 / 27.205806 / 27.239037 | 1147360 | 11.923631 | 146.319 / 146.433 / 146.597 | 150.214 / 150.771 / 150.877 |
| K4 | FastFP32 | direct | O1 | 77.409776772 / 77.409776825 / 77.409776878 | 26.515659 / 26.546343 / 26.546927 | 1147360 | 11.641875 | 150.404 / 150.956 / 151.658 | 144.943 / 149.373 / 150.063 |
| K4 | Strict | direct | O2 | 78.194499006 / 78.194499058 / 78.194499111 | 27.184750 / 27.229123 / 27.369478 | 1178912 | 11.894912 | 145.670 / 146.439 / 146.675 | 150.680 / 150.876 / 151.719 |
| K4 | FastFP32 | direct | O2 | 78.194499006 / 78.194499111 / 78.194499111 | 27.122920 / 27.180522 / 27.188319 | 1178912 | 12.006369 | 146.662 / 146.695 / 147.027 | 150.170 / 150.260 / 150.649 |
| K4 | Strict | direct | O3 | 78.194499006 / 78.194499058 / 78.194499111 | 27.186832 / 27.327941 / 27.379334 | 1178912 | 12.203720 | 145.594 / 145.862 / 146.666 | 150.634 / 151.460 / 151.795 |
| K4 | FastFP32 | direct | O3 | 78.194499006 / 78.194499058 / 78.194499058 | 27.198103 / 27.204426 / 27.251525 | 1178912 | 12.176766 | 146.327 / 146.560 / 146.619 | 149.820 / 150.699 / 151.033 |
| K4 | Strict | direct | Os | 81.527832639 / 81.527832692 / 81.527832692 | 28.244746 / 28.335927 / 28.446045 | 1199920 | 11.761935 | 140.141 / 140.699 / 141.183 | 156.492 / 156.853 / 157.095 |
| K4 | FastFP32 | direct | Os | 81.527833487 / 81.527833593 / 81.527833858 | 28.572734 / 28.683192 / 28.785029 | 1199920 | 13.555172 | 119.998 / 121.934 / 124.277 | 162.858 / 164.437 / 165.716 |
| K5 | Strict | exact | O1 | 13.720038821 / 13.720038821 / 13.720038890 | 3.877835 / 3.886013 / 3.888369 | 128115 | 2.556982 | 882.769 / 941.093 / 946.239 | 115.480 / 117.878 / 126.448 |
| K5 | FastFP32 | exact | O1 | 13.720038958 / 13.720039094 / 13.720039639 | 3.869383 / 3.871316 / 3.882575 | 128115 | 2.629767 | 867.498 / 869.023 / 871.774 | 127.316 / 128.528 / 128.630 |
| K5 | Strict | exact | O2 | 13.720003882 / 13.720003950 / 13.720003950 | 3.876384 / 3.877369 / 3.887670 | 125949 | 4.622006 | 460.924 / 465.736 / 466.472 | 237.140 / 239.160 / 239.906 |
| K5 | FastFP32 | exact | O2 | 13.720004155 / 13.720004563 / 13.720004563 | 3.881889 / 3.894280 / 3.894745 | 125949 | 4.665750 | 344.434 / 381.248 / 388.069 | 280.168 / 293.675 / 325.578 |
| K5 | Strict | exact | O3 | 13.720003950 / 13.720004018 / 13.720004018 | 3.881569 / 3.892258 / 3.895554 | 125949 | 3.260930 | 875.862 / 942.474 / 946.845 | 117.233 / 117.654 / 127.509 |
| K5 | FastFP32 | exact | O3 | 13.720003882 / 13.720003882 / 13.720003950 | 3.873661 / 3.878216 / 3.891237 | 125949 | 2.808502 | 973.167 / 976.394 / 978.033 | 113.028 / 117.339 / 117.946 |
| K5 | Strict | exact | Os | 13.576856727 / 13.576856795 / 13.576856795 | 3.855829 / 3.870478 / 3.876538 | 104416 | 1.965936 | 956.787 / 971.494 / 977.123 | 112.871 / 114.167 / 114.233 |
| K5 | FastFP32 | exact | Os | 13.576856727 / 13.576856727 / 13.576856795 | 3.886654 / 3.886935 / 3.895984 | 104416 | 1.956633 | 971.989 / 974.317 / 974.682 | 114.218 / 114.499 / 114.754 |
| K5 | Strict | direct | O1 | 13.720043248 / 13.720043248 / 13.720043248 | 3.878396 / 3.884166 / 3.885526 | 128247 | 2.379918 | 1010.360 / 1012.216 / 1026.746 | 113.733 / 116.337 / 116.641 |
| K5 | FastFP32 | direct | O1 | 13.720043248 / 13.720043248 / 13.720043317 | 3.884833 / 3.890260 / 3.898694 | 128247 | 2.258140 | 1022.837 / 1024.685 / 1025.974 | 117.700 / 118.040 / 118.343 |
| K5 | Strict | direct | O2 | 13.720008377 / 13.720008377 / 13.720008445 | 3.879548 / 3.883884 / 3.885556 | 126066 | 2.524795 | 1001.928 / 1017.832 / 1026.471 | 113.104 / 115.662 / 116.048 |
| K5 | FastFP32 | direct | O2 | 13.720008309 / 13.720008445 / 13.720008445 | 3.869361 / 3.871813 / 3.882697 | 126066 | 2.487356 | 1026.578 / 1029.786 / 1030.513 | 115.710 / 115.791 / 115.994 |
| K5 | Strict | direct | O3 | 13.720008309 / 13.720008445 / 13.720008445 | 3.878222 / 3.887079 / 3.887980 | 126066 | 2.593592 | 973.932 / 974.197 / 976.753 | 114.029 / 115.442 / 115.608 |
| K5 | FastFP32 | direct | O3 | 13.720008377 / 13.720008445 / 13.720008445 | 3.916217 / 3.921763 / 3.929129 | 126066 | 2.671196 | 939.976 / 948.034 / 963.704 | 114.938 / 116.054 / 116.391 |
| K5 | Strict | direct | Os | 13.576861154 / 13.576861290 / 13.576861290 | 3.874359 / 3.875338 / 3.880712 | 104416 | 1.977961 | 976.076 / 976.582 / 977.353 | 124.009 / 124.304 / 124.491 |
| K5 | FastFP32 | direct | Os | 13.576861222 / 13.576861222 / 13.576861699 | 3.904943 / 3.917297 / 3.927290 | 104416 | 1.987835 | 962.781 / 966.720 / 969.391 | 122.325 / 125.212 / 125.808 |

The expected K4 output SHA-256 is
`490294093fdf6b9e10740daa992391bae19396c6e78bff448f035b4e7ca37f25`;
K5 is `e84b69ed409029f1af79718d2a57b8c0ab264a77e3979a939cb39b8087bdc589`.
All levels retain those complete outputs. This experiment exercises all-RAM
normal-word bindings, not protected faults, I/O or other memory views. It makes
no macOS ARM64 claim. The large guest/C++ gap, remaining ISA/VISA/numerical work,
complete read/write views and parallel preparation remain open.

[Host, source, binary and output receipt](linux-optimization-9b7e95e-host.json).
Recompute each range with `python3 test/bench/summarize.py <raw-runs.jsonl>`.
The historical O3 baselines and host-fault measurements remain unchanged.

- K4, exact, O1: [raw runs](linux-optimization-9b7e95e-k4-exact-O1.jsonl).
- K4, exact, O2: [raw runs](linux-optimization-9b7e95e-k4-exact-O2.jsonl).
- K4, exact, O3: [raw runs](linux-optimization-9b7e95e-k4-exact-O3.jsonl).
- K4, exact, Os: [raw runs](linux-optimization-9b7e95e-k4-exact-Os.jsonl).
- K4, direct, O1: [raw runs](linux-optimization-9b7e95e-k4-direct-O1.jsonl).
- K4, direct, O2: [raw runs](linux-optimization-9b7e95e-k4-direct-O2.jsonl).
- K4, direct, O3: [raw runs](linux-optimization-9b7e95e-k4-direct-O3.jsonl).
- K4, direct, Os: [raw runs](linux-optimization-9b7e95e-k4-direct-Os.jsonl).
- K5, exact, O1: [raw runs](linux-optimization-9b7e95e-k5-exact-O1.jsonl).
- K5, exact, O2: [raw runs](linux-optimization-9b7e95e-k5-exact-O2.jsonl).
- K5, exact, O3: [raw runs](linux-optimization-9b7e95e-k5-exact-O3.jsonl).
- K5, exact, Os: [raw runs](linux-optimization-9b7e95e-k5-exact-Os.jsonl).
- K5, direct, O1: [raw runs](linux-optimization-9b7e95e-k5-direct-O1.jsonl).
- K5, direct, O2: [raw runs](linux-optimization-9b7e95e-k5-direct-O2.jsonl).
- K5, direct, O3: [raw runs](linux-optimization-9b7e95e-k5-direct-O3.jsonl).
- K5, direct, Os: [raw runs](linux-optimization-9b7e95e-k5-direct-Os.jsonl).
