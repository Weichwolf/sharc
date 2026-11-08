# Target JIT architecture

This is the implementation target, not a claim of existing code. The initial
repository contains project instructions, architecture/verification plans and
reference materials. No Core API, decoder, interpreter or JIT is implemented yet.

```text
consumer-owned memory and I/O -> Core -> retained native regions and loops
                                           |                 |
                                      direct RAM      event / I/O / fault
```

## Ownership and public surface

Expose only include/sharc/core.hpp and memory.hpp when implementation starts.
Use namespace sharc and CMake target sharc::core. Keep LLVM types and native-code
details private. Core owns architectural state, progress, failure context and
translation lifetime. Consumers own storage, device models, clocks and scheduling.
One owner executes and mutates a Core. Separate cores can execute independently;
cross-core synchronization belongs to the consumer.

Execution, reset and memory mutation must have explicit ownership and reentry
rules. State inspection returns complete architectural values only when requested;
it must not force state materialization on every guest instruction. Expose useful
fault context and precise guest prefixes at observable exits.

## Translation and memory

Decode from actual guest entries. Normalize encodings into reviewed architectural
operations and retain the semantic order of old reads, computation and writes.
Lower ordinary operations natively through private LLVM 19 infrastructure on
both hosts. Real device callbacks are external effects; generic ISA helper gaps
must not masquerade as ordinary native coverage.

Compile natural regions and loops without fixed instruction-count caps. Retain
immutable native translations and link successors. Execution grants and deadlines
do not change block identities. Preserve precise interrupt/fault exits without a
per-instruction dispatcher. Keep registers live, RAM direct and cold failures out
of ordinary control flow. Hoist checks only with a valid alias/permission proof.

Record code identities, executable mappings and dependencies. Writes through data
aliases, owner mutations and guest architectural cache maintenance must invalidate
or retire affected native translations correctly. Use explicit generation changes
and ownership; do not continuously hash guest memory. Handle native-code lifetime
and host instruction-cache synchronization correctly on both hosts.

## ADSP-214xx profile

- Decode the classic 48-bit ISA and mixed 16-/32-/48-bit VISA. Encoding widths,
  PC increments and memory serialization are separate concepts; a boot-stream
  decoder is not a CPU instruction fetcher.
- Preserve PEx/PEy SIMD state and primary/alternate register files, universal
  registers, arithmetic state, modes, flags, loop/PC/status stacks and interrupts.
- Define one instruction's read/compute/write semantics before lowering it.
  Parallel transfers and arithmetic must read the documented old values; host
  scheduling cannot turn them into dependent sequential guest operations.
- Specify bit-exact fixed and floating arithmetic, 32-/40-bit precision, rounding,
  saturation, sticky/exception flags, NaNs, signed zero and SIMD lane effects.
  Do not assume host float/double/FMA are interchangeable with SHARC operations.
- DAG1/DAG2 addressing includes pre/post modification, circular buffers, bit-reverse
  behavior and mode-dependent effects. Preserve hardware-loop endpoints, delayed
  branches/returns and loop reentry (LR), including interruption boundaries.
- Map logical PM/DM and short/normal/long/instruction views onto consumer-owned
  backing storage without losing physical aliases. Code changes through any view
  must update translation ownership/coherency. Multiple DSP instances have separate
  Core state; synchronization and shared memory ownership are consumer decisions.
- Include architecturally visible core-timer state and its interrupt contract;
  the consumer supplies elapsed guest time, not host wall time.
- Chip-specific memory capacities/vectors are profile data. SRU/DAI/DPI, SPORT,
  SPI, DMA, external accelerators, boot loaders and physical timing are consumer
  responsibilities. Core-visible register effects must have a defined boundary.

Use ADSP-21489 as the first concrete profile. A SHARC family library name does not
claim SHARC+ compatibility or identical behavior across every older SHARC device.

## Floating-point execution modes

Both modes below are planned implementation requirements. Neither is implemented
yet. Consumers explicitly choose a mode when creating a Core; the selection stays
fixed for that Core's lifetime. Strict is the default. Include the mode in retained
and persistent translation identities so native code cannot cross numerical
contracts. Keep the existing minimal public-header shape; do not add a public
backend selector or per-instruction mode switch.

### Strict

Preserve the original 32-/40-bit computations, rounding, flags and special cases.
MODE1.RND32 selects the documented rounding boundary; 32-bit paths may use native
SIMD only when their entire numerical behavior is preserved. RND32 alone does
not justify truncating every register: some conversions use 40-bit precision
regardless of that bit. Registers and memory retain their documented raw views.

### FastFP32

This explicit opt-in mode is a user-requested precision approximation intended
to simplify AVX/NEON lowering for audio consumers. It is not bit-exact SHARC
execution and does not promise identical sound or a measured speedup.

- Convert each computational floating-point operand to binary32 and round each
  floating-point result to binary32, including operations that otherwise use
  40-bit precision. Specify rounding and conversions explicitly using the guest
  rules; do not inherit the compiler's unrestricted fast-math behavior.
- Preserve raw 40-bit storage and transfers that do not perform floating-point
  computation. Do not truncate integer registers, instruction words, addresses,
  stacks or memory formats. Precision reduction happens at computational views,
  not by globally shrinking the architectural register file or changing aliases.
- Keep MODE1 and other guest control-register values observable as written.
  FastFP32 overrides the computational precision choice without rewriting RND32.
  Keep the guest's rounding direction, documented special-value handling and
  exception/flag rules for the approximate operands/results. Flags, comparisons
  and subsequent program decisions can differ because the operands differ.
- Preserve old-value reads, instruction/parallel-effect ordering, feedback and
  control flow semantics. Do not reassociate expressions, contract multiply/add
  into FMA, eliminate NaNs or change denormal handling merely to improve speed.
- Preserve the host floating-point environment and isolate Core instances.
  Generate native SIMD paths where they meet this mode's contract; do not make
  translation correctness depend on ambient host rounding or flush settings.

Use independent synthetic expected results for each mode, including rounding
boundaries, conversions, flags and SIMD lanes. Strict expectations must continue
to match documented SHARC semantics. FastFP32 tests must match the explicit
approximation contract and also prove unchanged raw moves, integer behavior and
mode-register readback. Keep both numerical modes in the coverage inventory and
report their performance separately. Neither mode uses firmware as test data.

## Verification and performance

Use test/isa for encodings and semantics, test/core for architectural boundaries
and memory, test/jit for generated-code/ownership behavior, and test/bench for
continuous workloads. test/support/isa.hpp will be an independently authored
fixture builder with explicit instruction fields, not a wrapper around the
production decoder. Expected outputs need independent mathematical or documented
derivations. Include encoding collisions, rejected forms and precise partial exits.

First compare correctness, then measure Release builds against matched handwritten
host ASM with equal work and checked outputs. Record cold translation, warm runs,
code size, event-boundary cost, worst cases and numerical modes. Host SIMD is an
optimization only where the selected mode's contract and measured benefit are
established. Strict architectural identity is never waived by FastFP32 results.
No real-time, cycle-accuracy or cross-host claims follow from configuration alone.

Build implementation targets with C++23 and LLVM 19 using CMake/Ninja release and
strict presets; add separate thread sanitizer coverage for concurrent code. Keep
format/static checks strict and functions <=120 lines. The first implementation
increment must add real build targets and independent fixtures together; this
documentation seed intentionally contains no empty library or fake passing ISA test.
