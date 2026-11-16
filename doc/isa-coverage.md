# ISA and implementation coverage

Every execution row remains **unimplemented**. The S1 foundation recognizes
top-level formats during preparation; that does not establish operand legality,
compute semantics or installed-image execution coverage. S1 covers the
preparation/storage boundary; native execution is still absent.

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

## S1 preparation evidence (complete milestone)

`test/core/preparation.cpp` drives only public `Core::lower` and checks structural
recognition of forms 1a/b, 2a/b/c, 3a/b/c, 4a/b, 5a/b, 6a, 7a/b, 8a,
9a/b, 10a (ISA only), 11a/c, 12a, 13a, 14a, 15a/b, 16a/b, 17a/b,
18a, 19a, 20a, 21a/c, 22a/c and 25a/c. Expectations use independently
written numerical fields. Additional cases check compact/classic prefix
collisions, 16/32/48-bit truncation, instruction-interior entry refusal, ISA/VISA
PC units, span overlap, vectors, transitions and input/mode identity separation.
`test/core/compute.cpp` adds single-function, immediate/short-compute, dual-ALU,
multiplier/ALU and accumulator-transfer cases. It checks reserved opcodes,
invalid modifiers and class collisions through the same public preparation path.
The decoder normalizes these compute fields while retaining raw encoding and
ISA/VISA identity. The detailed inventory is in [encoding.md](encoding.md).

All recognized instructions still fail preparation with identity, variant, PC,
raw encoding, width and an explicit unsupported-lowering reason.

`test/core/state.cpp` checks raw banks, DAGs, stacks, state replacement, snapshot
mode compatibility and clearing a later unprepared-execution fault. These are
storage/interface checks; hardware reset, interrupted continuation and execution
semantics are not qualified. See [state.md](state.md).

S1 provides all contract declarations, an installable `sharc::core` package,
top-level ISA/VISA and compute decoding, independent preparation fixtures, and
the programmer-visible state definition. Operand-level transfer restrictions,
ordered effects and hidden delayed/interrupt continuation still need native-path
implementation and installed-image qualification in S3/S4. They are not claimed
as execution coverage. S2 is in progress as described below and still precedes S3.

Linux x86-64: Clang/LLVM 19.1.7 Release build, four CTest checks (preparation,
compute, state and installed consumer), clang-format and clang-tidy clean. A
source export without manuals or Git metadata builds and passes the same four
checks. Only the public headers are installed; no LLVM types leak through them.
macOS ARM64: untested. Hardware user counters `instructions:u,cycles:u` were
accessible with perf_event_paranoid=2; no guest benchmark measurements exist.
No native execution, complete ISA, real-time or benchmark claim.

## S2 independent expectations (in progress)

`test/support/reference.py` supplies a separate exact integer/rational numeric
model. `generate_expectations.py` checks 90 numeric/conversion/accumulator
anchors, 48,768 normal-range rounding positions and 27,648 limb-based multiplier
cases and can emit deterministic machine-readable expectations without loading
production code. Fixed ALU records include complete
modeled scalar flags and sticky effects, including fixed 80-bit arithmetic,
saturation, rounding and accumulator transfers. Floating records currently cover
numeric values and exception metadata only; they are not complete instruction results.

S2 is **not complete**. The model explicitly refuses unqualified NaN/near-underflow
boundaries and still needs floating status, remaining conversions/computations,
shifter/packing and SIMD/parallel expectations. See
[numeric-reference.md](numeric-reference.md) for the exact scope, source sections
and gaps. Oracle validation is not installed-image ISA execution coverage. No
production lowering, native-image execution or benchmark implementation has been
added by this increment.
