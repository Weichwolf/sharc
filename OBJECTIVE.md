# Objective

Build a complete, independent, high-performance SIMD SHARC ISA/VISA native JIT
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

Production execution is JIT-only. Compile connected native regions along natural
control flow, keep values live, retain immutable translations and link native
successors. Support executable-memory/mapping changes and cache invalidation with
explicit ownership. No hidden interpreter fallback, instruction-count translation
cap, fabricated progress or application-specific instruction substitution.

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
SHARC+ and older derivative profiles are not automatically covered by ADSP-214xx.
Separate SRU, SPORT, SPI, DMA, external accelerators and board clocks from the core;
provide the architectural memory/I/O/interrupt boundary needed by consumers.

Provide reproducible independently authored ISA fixtures and independent expected
results. Verify state, flags, numerical edge cases, memory access/order, faults,
interrupt boundaries, self-modifying code, aliases and integration callbacks.
Production and independent verification must not share a semantic oracle. Report
exact ISA/mode coverage; unimplemented or unresolved cases prevent completion.
Tests are synthetic: manufacturer firmware and extracted instruction sequences
are not test assets. The firmware reconstruction's byte-only acceptance rule is
separate from this core's correctness requirements.

Measure cold/warm latency, guest instructions, documented base-cycle estimates,
generated code size and matched JIT/host-ASM throughput on both host platforms.
Check complete outputs independently. >=70% realistic warm host-ASM throughput is
a reference, not a device-integration gate. Measure sustainable consumer throughput
and bounded live latency separately; clock factors and unmeasured host results
must remain explicit. Profile-driven optimization must preserve all semantics.

Use C++23, LLVM 19, CMake/Ninja, strict builds, formatting, static analysis and
focused sanitizer verification before publishing implementation increments.
Every line must be understood, meaningfully named and pleasantly readable.
Completion requires a usable native engine, documented API, passing independent
ISA/JIT/core verification and demonstrated supported-host results. Documentation
and downloaded manuals alone do not meet this implementation objective.
