# sharc

- Follow doc/core-contract.md, the contract shared by all cores (cpu12, dsp56300,
  armv5, sharc). It wins over this file, OBJECTIVE.md and doc/architecture.md;
  change it only together with the other cores. In particular: declared code
  images lowered through LLVM only, no hand-written per-host emitter, and
  complete state save/restore is part of the public interface.
- Build an independent complete SIMD SHARC ISA/VISA native JIT. Read OBJECTIVE.md and
  doc/architecture.md before changing scope or claiming completion.
- Initial hardware profile: ADSP-214xx, initially ADSP-21489. A profile defines architectural behavior;
  it is not proof of the exact chip fitted to a consumer device.
- Keep application firmware, device clocks, peripherals, boot-stream containers,
  audio routing and multi-core scheduling in consumers. No foreign execution core.
- Expose only core.hpp and memory.hpp, following cpu12 and dsp56300. Core owns
  complete readable architectural state; consumers own memory and supply I/O.
  One owner executes each Core; distinct cores may run independently.
- Production is JIT-only. Lower ordinary guest semantics to native code on both
  x86-64 and ARM64. Real device I/O calls are distinct from ISA fallback gaps.
  Unsupported legal forms, host limitations and failed accesses remain explicit.
- Compile natural control-flow regions, retain immutable translations and link
  native successors. No instruction-count translation caps, budget-specific
  recompilation, per-instruction dispatch or public effect tickets.
- Keep native registers live and RAM direct. Code/mapping writes and aliases must
  invalidate affected translations correctly; this is mandatory for arbitrary
  consumer programs. No per-instruction hashes or fabricated retirement.
- Preserve original guest arithmetic, architectural quirks, faults and ordering.
  Every line must be understood and pleasantly readable. Byte similarity to one
  firmware does not prove core correctness or complete ISA coverage.
- Strict is the default execution mode. Also implement the user-requested,
  explicitly selected FastFP32 mode specified in doc/architecture.md. Limit its
  approximation to floating-point operand/result precision; preserve raw storage,
  integer semantics and instruction ordering. Do not turn on compiler fast-math.
- Keep execution modes fixed for a Core lifetime and separate translation
  identities, coverage and performance results. FastFP32 is not bit-exact SHARC
  execution and must never substitute for Strict acceptance.

## Working rules

- Speak German with the user; code, comments, documentation and commits are English.
- C++23, LLVM 19, CMake/Ninja for the implementation. Use strict warnings/linking,
  clang-format, zero-finding clang-tidy and functions <=120 lines. Do not suppress
  or weaken checks to accept incomplete semantics.
- Validate Linux x86-64 and macOS ARM64 independently. Mark untested hosts honestly.
  Use Release for performance, affected ASan/UBSan correctness checks and separate
  TSan checks for concurrent infrastructure. No GitHub workflows unless requested.
- Write independent synthetic ISA fixtures under test/support/isa.hpp; organize
  test/isa, test/core, test/jit and test/bench consistently with the sibling cores.
  Core correctness tests are required. The separate lead-a1-src reconstruction's
  byte-only acceptance policy does not replace tests for a new execution engine.
- Benchmarks must check complete results against independent expectations and
  compare matched host ASM work. Report cold translation, warm throughput, code
  size, boundary costs and every slow scenario. >=70% realistic warm host-ASM
  throughput is a reference, not proof of instrument real time. No invented MHz.
- Host SIMD and batching require proof of guest dependencies, the selected mode's
  numerical contract and measured benefit. Preserve feedback and observable
  partial exits; FastFP32 does not authorize reassociation or FMA contraction.
- Telemetry is runtime-selectable and off by default; no hot-path statistics.
- README describes what exists; OBJECTIVE defines acceptance; doc/architecture.md
  is the single target design. Keep doc/isa-coverage.md honest. No diaries or
  tracked result archives; do not advertise planned APIs as implemented.
- Manuals, firmware, samples, raw disassembly and private analysis stay ignored
  under doc/local/ or build/local/. Record reference origin, revision, size and
  SHA-256 in doc/reference-manifest.json. Manufacturer documents are not GPL code.
- Download/reference tooling is analysis support; production builds and synthetic
  tests must work offline without manuals or manufacturer program bytes.
- Keep cpu12, dsp56300, lead-a1-src, lead-a1-emu and other siblings untouched unless
  the user authorizes a specific integration edit. Read-only comparison is allowed.
  Do not resume the paused firmware goal or its stopped processes implicitly.
- Original project code is GPL-3.0-only with SPDX notices. Preserve provenance and
  dependency notices. Do not import GPL-2.0-only code or another emulator's core.
- Keep canonical main current; commit reviewed increments. Push only an already
  configured origin/main when authorized. No --all/--mirror or backup branches.
  The authorized GitHub destination is private Weichwolf/sharc; preserve privacy.
- Coordinate with the Lead A1 consumer only through private Weichwolf/lead-a1-src
  issue #1 when explicitly authorized. Do not copy private reconstruction material
  into public projects. No routine issue-status spam or hardware flashing.
