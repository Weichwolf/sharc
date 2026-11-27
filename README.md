# sharc

Independent SHARC native translation project, initially targeting ADSP-214xx SIMD cores
with ISA and VISA decoding.

Target implementation: C++23, LLVM 19, Linux x86-64 and macOS ARM64,
GPL-3.0-only. The library follows the Core/Memory/Io ownership model of the
adjacent cpu12 and dsp56300 projects. See [OBJECTIVE.md](OBJECTIVE.md) for
acceptance and [doc/architecture.md](doc/architecture.md) for the target design.

## Current state

Installed LLVM images execute a classic ISA subset that includes fixed-point
compute, single-function floating arithmetic and comparison, memory transfers,
delayed control flow, hardware loops and interrupt continuations. The API
supports artifact caching, snapshots and declared code transitions. See the
[coverage inventory](doc/isa-coverage.md) for legal forms and remaining gaps,
[state storage](doc/state.md), [memory access](doc/memory.md) and the
[independent numeric reference](doc/numeric-reference.md). The
[benchmark report](test/bench/README.md) lists the checked Linux K1–K6 results.

```sh
nice -n 10 cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-19
nice -n 10 cmake --build build
nice -n 10 ctest --test-dir build --output-on-failure
```

Requirements: C++23, LLVM 19.1, CMake 3.25 and Ninja; Python 3 for tests.
The tests work offline
without local reference manuals. An installed CMake consumer is checked by CTest.
Linux x86-64 is tested; macOS ARM64 is untested.

## Numerical modes

Strict is the default and uses the guest's 32-/40-bit rules for supported
floating operations. FastFP32 is an opt-in mode that rounds computational
operands and results to binary32, preserving raw storage, integer semantics
and instruction order. Both modes use separate prepared image identities.
See the [numerical contracts](doc/architecture.md#floating-point-execution-modes).

## Reference material

Read [doc/references.md](doc/references.md). Manufacturer PDFs and extracted text
are kept locally under ignored doc/local/; their origins, revisions, byte lengths
and SHA-256 identities are recorded in doc/reference-manifest.json.

```sh
python3 tools/references.py verify
python3 tools/references.py fetch
python3 tools/references.py text
```

Verification is offline. Fetch downloads missing pinned copies; text requires
pdftotext. These commands inspect reference material and do not execute firmware
or test an instruction set. Manuals are not production-build inputs.

## Implementation order

1. Establish the complete profile/encoding inventory and independent fixtures.
2. Add the minimal architectural state/memory API and real strict build targets.
3. Implement native lowering on both hosts with independent ISA/core/JIT checks.
4. Complete faults, interrupts, numerical modes and code/mapping coherence.
5. Measure checked continuous workloads and integrate through consumer-owned I/O.

Device firmware, peripheral models, audio scheduling and application code remain
in consumers. The separate Nord firmware reconstruction remains paused.

[License](LICENSE) | [Provenance](THIRD_PARTY.txt)
