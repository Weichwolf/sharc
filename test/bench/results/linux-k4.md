# Linux K4 baseline

Measured implementation: `fd443160161f2d2ea0ed2aa237339366e8db25bf`, Intel Core i5-10400, Linux x86-64,
Clang 19.1.7 Release, thread pinned to CPU 4. Each mode has one warm pair
and three measured pairs with complete output, register, table, stack and
progress checks. All six user hardware groups are unscaled. No build,
test or probe owned by this session ran concurrently with measurement.

Each run retires 18,874,369 guest instructions and 39,845,886 nominal cycles.
Preparation is measured once per image. Native text includes all mode
contexts and cold paths; text density divides by the 61 unique guest words.

| Mode | Host instructions / guest | Host cycles / guest | Guest instructions/s | Guest/C++ time | Prepare s | Native bytes | Bytes/prepared word |
|---|---:|---:|---:|---:|---:|---:|---:|
| Strict | 78.194495 | 26.904262 | 150385755 | 150.57 | 10.76204 | 1,178,592 | 19321.18 |
| FastFP32 | 78.194495 | 26.906238 | 149628708 | 148.24 | 10.75015 | 1,178,592 | 19321.18 |

Ranges over the same three runs:

| Mode | Host cycles / guest | Guest instructions/s | Guest/C++ time | ns / guest |
|---|---:|---:|---:|---:|
| Strict | 26.901135–26.950295 | 149764318–150465441 | 141.33–150.64 | 6.646–6.677 |
| FastFP32 | 26.899134–26.938214 | 148438486–150346333 | 147.70–152.62 | 6.651–6.737 |

The approximately 150-fold time ratio to the native C++ state machine is an
open performance gap. Source inspection finds a cycle in the conservative
grant graph: every RTS is connected to every local CALL continuation, and
a handler continuation here is itself an RTS. That admission issue and
PC-stack state traffic need separate measurement before attributing costs.

The independent C++ object contains an unrolled integer loop with rotate
instructions. It uses runtime parameters in a separate `-O3 -fno-lto
-march=native` translation unit and permits normal optimization.
Both guest modes execute integer arithmetic here; these results establish
no floating-point speedup or full Strict qualification.

The same revision also checks K5: 14,682,625 guest instructions and
23,071,742 nominal cycles per run, with six more unscaled groups.

| K5 mode | Host instructions / guest | Host cycles / guest | Guest/C++ time | Prepare s | Native bytes |
|---|---:|---:|---:|---:|---:|
| Strict | 13.720003 | 3.915087 | 126.95 | 2.25614 | 125,949 |
| FastFP32 | 13.720003 | 3.907871 | 127.62 | 2.25209 | 125,949 |

K5 remains at 13.720003 host instructions/guest, matching the preceding
register-transfer increment. The roughly 2.14% instruction-count increase
from 13.432280 at `5017f6c` remains open. Wall-time variation and the small
mode timing differences are retained without a speedup claim.

[K4 raw records](linux-k4.jsonl), [all K4 metrics](linux-k4-summary.json),
[K4 host/source receipt](linux-k4-host.json),
[current K5 raw records](linux-k5-fd44316.jsonl),
[all current K5 metrics](linux-k5-fd44316-summary.json) and
[current K5 receipt](linux-k5-fd44316-host.json) preserve all measured counts.
The [first K5 baseline](linux-k5.md) is retained at its original revision.
See [the benchmark protocol](../README.md). K1–K3, K6, full profile
qualification and macOS ARM64 remain open; this does not complete S3.
