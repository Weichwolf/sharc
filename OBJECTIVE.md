# Objective

Build a complete, independent, high-performance SIMD SHARC ISA/VISA native translation
library for arbitrary consumer programs. Target ADSP-214xx, starting with the
ADSP-21489 profile. Support Linux x86-64 and macOS ARM64 with identical guest
semantics. A firmware-specific subset
does not meet completion; every legal instruction/form and architectural mode in
the declared profile needs independently justified semantics and coverage.

Expose a small Core/Memory/Io shape through core.hpp and memory.hpp, aligned with
cpu12 and dsp56300. Core owns complete readable state; consumers own memory and
supply real I/O, clocks, peripheral models and scheduling. Preserve architectural
faults, exceptions, reset, interrupts, address aliases, execution state and memory
ordering. Unsupported operations must fail with useful guest context.

Follow doc/core-contract.md, the contract shared by all cores. Production
executes prepared LLVM images only: the consumer declares executable code,
entries, variants and memory layouts; the core lowers them to LLVM IR; the
consumer optimizes, caches and installs. Keep values live across natural control
flow and link known successors directly. A code or mapping change revokes the
binding and a declared variant is installed. No interpreter, no per-host emitter,
no decoding or compilation at run time, no instruction-count translation cap,
fabricated progress or application-specific instruction substitution. Complete
state save and restore and cacheable preparation are part of the interface.

Implement the complete documented ADSP-214xx core ISA for the declared profile:
classic 48-bit instructions and 16-/32-/48-bit VISA instructions. Preserve both
SIMD processing elements, primary/alternate registers, integer and 32-/40-bit
floating-point computation, multiplier/accumulator state, modes and sticky flags.
Model parallel instructions as one architectural operation with documented old
operands and effect order. Preserve delayed branches/calls/returns, hardware loops,
loop reentry, stacks, interrupts, DAG addressing, circular buffers and bit reversal.

Preserve word-addressed PM/DM spaces and physical aliases among short, normal,
long and instruction/extended-precision views. Keep ISA and VISA PC/address units
distinct. Host floating-point evaluation, FMA contraction, host denormal settings
or converting every 40-bit operation to ordinary float must not change results.
Implement defined NaN, rounding, saturation, exceptional and parallel-lane behavior.
These exactness requirements define Strict, the default execution mode.
SHARC+ and older derivative profiles are not automatically covered by ADSP-214xx.
Separate SRU, SPORT, SPI, DMA, external accelerators and board clocks from the core;
provide the architectural memory/I/O/interrupt boundary needed by consumers.

Also implement an explicit opt-in FastFP32 mode for consumers that accept reduced
internal floating-point precision. It rounds computational floating operands and
results to binary32, including operations that Strict performs at 40-bit precision.
Keep raw register/memory transfers, formats, integer semantics, guest mode-register
readback, addressing and instruction order intact. Floating results, derived flags
and subsequent control flow may differ from Strict; audio equivalence is not
guaranteed. This mode is not permission for FMA contraction, reassociation or other
compiler fast-math changes. Define and verify its independent numerical contract
and report its coverage/performance separately. It does not replace complete,
correct Strict execution. Both modes are required project deliverables; neither
is implemented by this documentation change.

Provide reproducible independently authored ISA fixtures and independent expected
results. Verify state, flags, numerical edge cases, memory access/order, faults,
interrupt boundaries, declared code changes and transitions, aliases and
integration callbacks.
Production and independent verification must not share a semantic oracle. Report
exact ISA/mode coverage; unimplemented or unresolved cases prevent completion.
Tests are synthetic: manufacturer firmware and extracted instruction sequences
are not test assets. The firmware reconstruction's byte-only acceptance rule is
separate from this core's correctness requirements.

Measure with the common benchmarks of the contract (kernels K1 to K6): host
instructions and host cycles per guest instruction from hardware counters, guest
instructions per second, ratio to the same algorithm in portable Clang O3 C++,
preparation seconds and native code bytes, every kernel checking its complete
output. Sustainable consumer throughput and live latency are measured in the
consumer; clock factors and unmeasured host results stay explicit.

Use C++23, LLVM 19, CMake/Ninja, strict builds, formatting, static analysis and
focused sanitizer verification before publishing implementation increments.
Every line must be understood, meaningfully named and pleasantly readable.
Completion requires a usable native engine, documented API, passing independent
ISA and core verification and demonstrated supported-host results. Documentation
and downloaded manuals alone do not meet this implementation objective.
