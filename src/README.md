# Implementation ownership

`decode.cpp` validates declared image boundaries and recognizes classic ISA/VISA
formats during preparation. `compute.cpp` normalizes compute fields and rejects
reserved encodings. These structural checks do not establish execution coverage.

`lower.cpp` emits SSA LLVM IR for the subset in `doc/isa-coverage.md`: one fixed
mode and declared entry per connected unit, direct CFG edges, unit/loop grants,
exact cold prefixes and state writeback at exits. `abi.cpp` supplies shared
pinned-state reload/spill and native tail transfers between declared entries.
`image.cpp` owns portable artifact serialization/loading, stopped code-storage
validation and LLVM ORC
compilation. `memory.cpp` validates declared layouts and direct storage bindings;
the actual normal-word accesses, DAG updates and entry proofs are LLVM IR in
`lower.cpp`.
`core.cpp` supplies public lifecycle, native binding/execution and snapshots.
Generated code uses a private pointer/control ABI rather than public C++ object
layouts. Production contains no interpreter or host-specific emitter.

LLVM supplies preparation SHA-256, bitcode and the only native code generator.
Device knowledge, consumer storage and peripherals remain outside the core.
