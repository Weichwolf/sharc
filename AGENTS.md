# sharc

Goal: OBJECTIVE.md. Target design: doc/architecture.md. Contract shared by all
cores: doc/core-contract.md. The contract wins over every other document here
and changes only together with the other cores (cpu12, dsp56300, armv5, sharc).
Decide everything else yourself.

## Fixed limits

- Build an independent, complete SIMD SHARC ISA/VISA core. Initial hardware profile: ADSP-214xx, initially ADSP-21489.
  A profile defines architectural behavior; it is not proof of the exact chip
  fitted to a consumer device.
- The core translates one instruction set and owns the processor state. Firmware,
  device clocks, peripherals, boot-stream containers, audio routing, scheduling
  and multi-core coupling belong to consumers. No device or firmware knowledge
  and no special case for one firmware's code patterns in this repository.
- Production executes prepared LLVM images only: the consumer declares code and
  entries, the core lowers them to LLVM IR, one code generator. No interpreter,
  no hand-written per-host emitter, no decoding or compilation at run time.
  Unsupported, undeclared or changed code fails explicitly with identity,
  address and reason.
- Preserve original guest arithmetic, architectural quirks, faults and ordering.
  Never change a correct test to hide a defect.
- Strict is the default execution mode. Also implement the explicitly selected
  FastFP32 mode specified in doc/architecture.md. Limit its approximation to
  floating-point operand/result precision; preserve raw storage, integer
  semantics and instruction ordering. Do not turn on compiler fast-math.
- Keep execution modes fixed for a Core lifetime and separate their prepared-code
  identities, coverage and performance results. FastFP32 is not bit-exact SHARC
  execution and never substitutes for Strict acceptance.
- Manuals, firmware, samples, raw disassembly and private analysis stay ignored
  under doc/local/ or build/local/. Record reference origin, revision, size and
  SHA-256 in doc/reference-manifest.json. Production builds and tests work
  offline without manuals or manufacturer program bytes. Tests are authored
  programs; manufacturer firmware is never a test asset in a core.
- GPL-3.0-only with SPDX notices; keep provenance and dependency notices. Do not
  import GPL-2.0-only code or another emulator's core. No GitHub workflows.
  Run as a regular user. No hardware flashing.

## How work is judged

- By independent instruction-set coverage through the public interface and by
  the common benchmarks of the contract (K1 to K6): host instructions and host
  cycles per guest instruction, ratio to the same algorithm in Clang O3 C++,
  preparation seconds, native bytes. A consumer's whole-instrument throughput
  is judged in the consumer.
- Less code is better. Report the src/ and test/ line delta with each published
  increment. Keep doc/isa-coverage.md honest.

## Freedom

Internal structure, tools, test strategy within the contract's three layers and
the order of work are your choice. Follow the lowering rules in
doc/architecture.md; they were paid for on cpu12 and dsp56300.

## Working together

- German with the user; English code, comments, docs, commits and issues. C++23,
  LLVM 19, CMake/Ninja, warnings as errors, clang-format, clang-tidy clean.
- Commit reviewed increments and push them to origin/main (private
  Weichwolf/sharc). No force pushes or backup branches.
- Linux x86-64 is the development host. macOS ARM64 is validated when a developer
  is available there; mark untested hosts honestly.
- Coordinate in issues of private Weichwolf/lead-a1-src. Comments starting with
  [Lead] come from the technical lead and set priorities. Contract changes are
  proposed in Weichwolf/blofeld-src#33. Write short, result-first comments:
  table, what changed, what is open. No routine status comments.
- Keep cpu12, dsp56300, lead-a1-src, lead-a1-emu and other siblings untouched;
  read-only comparison is welcome. Do not resume the paused firmware goal or its
  stopped processes.
- Sanitizer qualification (ASan/UBSan, separate TSan) of own and generated code
  happens at the goal, earlier only for a concrete risk.
