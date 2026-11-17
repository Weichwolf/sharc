# ISA and implementation coverage

Coverage is partial. Format recognition during preparation does not establish
operand legality or execution coverage. Initial installed LLVM images execute a
classic ISA subset described below. S2 expectation generation is complete at its
stated scope; full numerical qualification and S3 native execution are incomplete; no complete profile or benchmark acceptance
is claimed.

| Area | Required evidence | Status |
|---|---|---|
| Classic 48-bit ISA | Every documented instruction class, condition, legal operand and parallel form | Partial: register-immediate, fixed ALU, direct jump, NOP |
| VISA 16/32/48 | All compact encodings, fetch boundaries, PC units and mode transitions | Unimplemented |
| Arithmetic and SIMD | PEx/PEy, fixed point, 32-/40-bit float, rounding, modes, flags and exceptions | Partial: fixed ALU and bank/SIMD execution |
| Strict numerical mode (default) | Exact guest 32-/40-bit behavior, RND32, conversion exceptions and native-path identity | Unimplemented |
| FastFP32 numerical mode (opt-in) | Independent approximation expectations, rounding/flags, unchanged raw moves/integer behavior, separate translations | Unimplemented |
| Parallel operations | Old operands, memory aliases, register exchanges and ordered partial effects | Unimplemented |
| Address generation and memory | Both DAGs, modulo/reverse addressing, PM/DM and physical view aliases | Unimplemented |
| Sequencer and control | Delayed flow, DO/loop endpoints, LR, stacks, reset and interrupts | Partial: direct non-delayed jumps and compute-to-branch stalls |
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

Recognized instructions outside the implemented subset fail preparation with
identity, variant, PC, raw encoding, width and an explicit unsupported-lowering
reason.

`test/core/state.cpp` checks raw banks, DAGs, stacks, state replacement, snapshot
mode compatibility and clearing a later unprepared-execution fault. These are
storage/interface checks; hardware reset, interrupted continuation and execution
semantics are not qualified. See [state.md](state.md).

S1 provides all contract declarations, an installable `sharc::core` package,
top-level ISA/VISA and compute decoding, independent preparation fixtures, and
the programmer-visible state definition. Operand-level transfer restrictions,
ordered effects and hidden delayed/interrupt continuation still need native-path
implementation and installed-image qualification in S3/S4. They are not claimed
as execution coverage. S2 expectation generation is documented separately below.

## Initial native execution (S3 incomplete)

`Core::lower` emits unoptimized LLVM bitcode. The standalone compiler uses LLVM
O3 and eagerly resolves every export before installation; `run` does not decode
or compile. Artifacts include bitcode and mode-unit metadata, a compiler/input
identity and an integrity checksum. `load` parses and verifies those artifacts
without lowering. The two public headers expose no LLVM types or register layout.

Classic ISA support currently includes NOP, immediate moves into R0–R15, and
conditional compute with the 21 fixed ALU opcodes ADD/SUB, carry/borrow forms,
AVG, CMP/COMP, PASS, NEG, INC/DEC, ABS, AND/OR/XOR/NOT, MIN/MAX and CLIP. Fixed
dual ADD/SUB with distinct outputs is lowered but lacks installed ISA qualification.
Direct non-delayed absolute and PC-relative jumps use both-lane AND predicates in
SIMD, link known successors inside one LLVM function and preserve backward edges.
Taken jumps cost four nominal base cycles; a conditional jump immediately after
a compute adds one interlock cycle, including when not taken (Programming
Reference 2.4, 4-19–4-21 and 4-115–4-116). The preceding-compute history is SSA
state, stored at exits and included in snapshots. Cycle bounds may cross the
deadline to finish the current instruction; cycle credit cannot wrap under zero.
External-flag and loop conditions refuse preparation. VISA, floating computation,
multiplier, shifter, guest memory, calls/returns, delayed jumps, hardware loops, timer, reset and
interrupt entry remain unimplemented. `interrupt_ready` reports false.

`test/isa/fixed.cpp` obtains 116 paired-lane expectations from the independent
Python reference and executes them under both numerical identities, both register
halves, four physical bank selections and SISD/SIMD. It compares complete visible
register state after exact partial bounds and fully admitted blocks. This is
integer execution in FastFP32, not qualification of its floating contract.
`test/isa/control.cpp` consumes 2,944 independent paired-status/condition cases
under absolute/relative targets and both numerical identities. It checks full
visible state after instruction and cycle bounds, scalar and SIMD predicates,
24-bit address wrapping and an authored backward loop's flags and cycle costs.
`test/core/control.cpp` checks precise missing-target faults and replay in another
Core, including a snapshot between the compute and conditional branch. A million-iteration native
control loop checks progress and EMUCLK carry without recursive guest transfers.
Reserved branch fields refuse preparation; legal but unimplemented modifiers
refuse explicitly. These are ISA checks, not K4 benchmark results.
Each prepared unit starts at one declared entry and includes its reachable direct
control flow. Distinct entries receive independent units; a saved continuation
retains its originating entry. Normal entry links directly into that flow; only
cold continuation resumes dispatch on PC. Artifact metadata preserves the entry
identity across loading and binding. Disconnected and overlapping entry paths,
owner PC replacement and cross-Core artifact-bound replay compare complete state.

`test/core/native.cpp` checks cache round trips/refusal, consumer-linked LLVM IR,
precise bounds and undeclared exits, stopped installation, code validation,
memory rebinding and snapshot replay in another Core. The installed CMake
consumer executes its own literal instruction program.

Linux x86-64: Clang/LLVM 19.1.7 Release; the seven CTest checks pass, including an
installed consumer executing native instructions. clang-format and clang-tidy
are clean. A source export without manuals or Git metadata builds and passes the
same seven checks. macOS ARM64 remains untested. Hardware user counters
`instructions:u,cycles:u` are accessible with perf_event_paranoid=2. K1–K6,
hardware performance results and goal sanitizer
qualification remain open. This is the first S3 implementation increment; the S3 milestone remains open.

## S2 independent expectations (complete milestone, numerical gaps retained)

`test/support/reference.py` supplies a separate exact integer/rational numeric
model. `generate_expectations.py` checks 279 numeric/conversion/accumulator/shifter
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

Another 16,384 raw40 swap checks cover every Rx/Sy pair, all bank selections,
SISD/SIMD and all independent predicate pairs. Full raw cells, including distinct
extension bytes, come from old sources. In SISD PEx admits both halves; in SIMD
each half has its own predicate. Authored ADD/swap expectations preserve old AZ
selection and new ALU flags even when a swap replaces the ALU data result.
This is expectation generation, not installed-image swap coverage.

The S2 generation criterion is met; the model explicitly refuses unqualified NaN/near-underflow
boundaries and conversion sentinel flags. Remaining exceptional floating status, computations,
OR-shift status, complete FPACK, full conditional execution/bank composition and parallel memory
ordering expectations are still required. See
[numeric-reference.md](numeric-reference.md) for the exact scope, source sections
and gaps. Oracle validation is not installed-image ISA execution coverage. No
complete numerical or profile acceptance follows from these oracle checks.
Native coverage is limited to the separate subset above; benchmarks remain open.
