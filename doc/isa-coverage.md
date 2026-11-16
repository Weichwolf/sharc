# ISA and implementation coverage

Every execution row remains **unimplemented**. The S1 foundation recognizes
top-level formats during preparation; that does not establish operand legality,
compute semantics or installed-image execution coverage. S1 is not complete yet.

| Area | Required evidence | Status |
|---|---|---|
| Classic 48-bit ISA | Every documented instruction class, condition, legal operand and parallel form | Unimplemented |
| VISA 16/32/48 | All compact encodings, fetch boundaries, PC units and mode transitions | Unimplemented |
| Arithmetic and SIMD | PEx/PEy, fixed point, 32-/40-bit float, rounding, modes, flags and exceptions | Unimplemented |
| Strict numerical mode (default) | Exact guest 32-/40-bit behavior, RND32, conversion exceptions and native-path identity | Unimplemented |
| FastFP32 numerical mode (opt-in) | Independent approximation expectations, rounding/flags, unchanged raw moves/integer behavior, separate translations | Unimplemented |
| Parallel operations | Old operands, memory aliases, register exchanges and ordered partial effects | Unimplemented |
| Address generation and memory | Both DAGs, modulo/reverse addressing, PM/DM and physical view aliases | Unimplemented |
| Sequencer and control | Delayed flow, DO/loop endpoints, LR, stacks, reset and interrupts | Unimplemented |
| Chip-profile boundaries | ADSP-21489 memory/vector parameters and explicit I/O ownership | Unimplemented |
| JIT coherence and hosts | Self-modifying aliases, natural loops, precise exits, x86-64 and ARM64 | Unimplemented |

## S1 preparation evidence

`test/core/preparation.cpp` drives only public `Core::lower` and checks structural
recognition of forms 1a/b, 2a/b/c, 3a/b/c, 4a/b, 5a/b, 6a, 7a/b, 8a,
9a/b, 10a (ISA only), 11a/c, 12a, 13a, 14a, 15a/b, 16a/b, 17a/b,
18a, 19a, 20a, 21a/c, 22a/c and 25a/c. Expectations use independently
written numerical fields. Additional cases check compact/classic prefix
collisions, 16/32/48-bit truncation, instruction-interior entry refusal, ISA/VISA
PC units, span overlap, vectors, transitions and input/mode identity separation.
All recognized instructions still fail preparation with identity, variant, PC,
raw encoding, width and an explicit unsupported-lowering reason.

`test/core/state.cpp` checks raw banks, DAGs, stacks, state replacement, snapshot
mode compatibility and clearing a later unprepared-execution fault. These are
storage/interface checks; hardware reset, interrupted continuation and execution
semantics are not qualified. See [state.md](state.md).

Open S1 work: operand/compute decoding and legality, a detailed encoding inventory,
and review of the architectural continuation needed by native execution. S2's
independent arithmetic model is absent. S3 remains gated on the announced Lead
update in lead-a1-src#2.

Linux x86-64: Clang/LLVM 19.1.7 Release build and public preparation/state tests.
macOS ARM64: untested. Hardware user counters `instructions:u,cycles:u` were
accessible with perf_event_paranoid=2; no guest benchmark measurements exist.
No native execution, complete ISA, real-time or benchmark claim.
