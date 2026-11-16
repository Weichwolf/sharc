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
model. `generate_expectations.py` checks 277 numeric/conversion/accumulator/shifter
anchors, 48,768 normal-range rounding positions and 27,648 limb-based multiplier
cases and can emit deterministic machine-readable expectations without loading
production code. Fixed ALU records include complete
modeled scalar flags and sticky effects, including fixed 80-bit arithmetic,
saturation, rounding and accumulator transfers. Qualified FIX/TRUNC records cover
finite integer and saturated results with flags, including exponent-based AV and
pre-rounded AUS; 10,972 signed-quotient comparisons and 1,828 required refusals
cross-check both conversion instructions. Floating value-only records remain
separate from new qualified complete ALU, compare, MANT/LOGB and multiply effects.
The latter pass 29,956 independent full-result checks and 5,132 required refusals;
unqualified negative-zero, NaN and MANT input-boundary status is not filled in.
Another 130,817 shifter, 15,449 bit FIFO and 65,536 short-float unpack cases use
independent bit routing, bit lists and rational value checks. Scalar shifter
expectations preserve unrelated flags and sticky bits. FIFO records refuse
undefined data/pointer results rather than supplying a guessed complete state.
Another 13,072 multifunction cases check dual-ALU flag unions, disjoint ALU/
multiplier flags, old operands and accumulator effects. Authored paired-lane
records include different old status/sticky bits and independent exceptions.
Register-overlap maps are numerical expectation data for future installed-image
fixtures; no instruction execution, memory ordering or SIMD control is claimed.
Another 133,832 FPACK field-only checks and 896 complete seed-special-case checks
cover exact short values, exponent clipping, signed zeros, infinities and qualified
invalid seed inputs. The 260 refused inexact PACK inputs, complete FPACK storage/
status and ordinary seed ROM entries remain gaps. Field-only records cannot
establish full instruction effects.
Another 928 numeric/status predicate and lane-aggregation checks use independent
truth tables. Conditional numerical records preserve inactive raw destinations,
old flags and SISD PEy state. These are not sequencer, DAG, memory-transfer or
pipeline execution checks.

Another 10,112 bank/writeback checks cover the documented producer priorities,
both PE files, all logical data registers and primary/alternate bank combinations;
256 winning-priority ties require explicit refusal. Complete raw-bank records
preserve old operands and compose overwritten ALU data with its independently
computed flags. This is numerical expectation generation; it does not qualify
parallel memory order, conditional transfers or native writeback execution.

S2 is **not complete**. The model explicitly refuses unqualified NaN/near-underflow
boundaries and conversion sentinel flags. Remaining exceptional floating status, computations,
OR-shift status, complete FPACK, full conditional execution/bank composition and parallel memory
ordering expectations are still required. See
[numeric-reference.md](numeric-reference.md) for the exact scope, source sections
and gaps. Oracle validation is not installed-image ISA execution coverage. No
production lowering, native-image execution or benchmark implementation has been
added by this increment.
