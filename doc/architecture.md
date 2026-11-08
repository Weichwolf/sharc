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
optimization only where architectural identity and measured benefit are established.
No real-time, cycle-accuracy or cross-host claims follow from configuration alone.

Build implementation targets with C++23 and LLVM 19 using CMake/Ninja release and
strict presets; add separate thread sanitizer coverage for concurrent code. Keep
format/static checks strict and functions <=120 lines. The first implementation
increment must add real build targets and independent fixtures together; this
documentation seed intentionally contains no empty library or fake passing ISA test.
