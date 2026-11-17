# sharc

Independent SHARC native translation project, initially targeting ADSP-214xx SIMD cores
with ISA and VISA decoding.

Target implementation: C++23, LLVM 19, Linux x86-64 and macOS ARM64,
GPL-3.0-only. The library follows the Core/Memory/Io ownership model of the
adjacent cpu12 and dsp56300 projects. See [OBJECTIVE.md](OBJECTIVE.md) for
acceptance and [doc/architecture.md](doc/architecture.md) for the target design.

## Current state

The preparation implementation provides the two public headers, the `sharc::core`
build target, ISA/VISA format and compute-field decoding, preparation diagnostics
and raw architectural state/snapshot storage. Initial LLVM lowering executes
classic ISA register-immediate moves, fixed ALU operations, non-delayed jumps,
calls/returns, direct normal-word memory moves and NOP through
installed images, with separate mode identities, artifact caching and snapshots.
Other instruction families refuse lowering explicitly. The CMake package installs
the `sharc::core` target and exactly two public headers. The S2 independent
expectation-generation milestone is complete; numerical gaps and the remaining
S3 native execution are explicit. Checked Linux K4 and K5 baselines are available
in [benchmark results](test/bench/README.md); the other common kernels remain open. See
[coverage](doc/isa-coverage.md), [state storage](doc/state.md), [memory access](doc/memory.md) and the
[independent numeric reference](doc/numeric-reference.md).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-19
cmake --build build
ctest --test-dir build --output-on-failure
```

Requirements: C++23, LLVM 19.1, CMake 3.25 and Ninja; Python 3 for tests.
The tests work offline
without local reference manuals. An installed CMake consumer is checked by CTest.
Linux x86-64 is tested; macOS ARM64 is untested.

## Planned numerical modes

Strict will preserve SHARC 32-/40-bit behavior and remain the default. The
explicit opt-in FastFP32 mode will limit floating computations to binary32 for
consumers that accept precision differences. It preserves raw storage, integer
semantics and instruction order, and does not enable unrestricted compiler
fast-math. Installed integer programs run under both mode identities; floating
computation in either mode remains unimplemented. See the
[numerical contracts](doc/architecture.md#floating-point-execution-modes).

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
