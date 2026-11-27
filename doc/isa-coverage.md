# ISA and implementation coverage

Coverage is partial. Format recognition during preparation does not establish
operand legality or execution coverage. Initial installed LLVM images execute a
classic ISA subset described below. S2 expectation generation is complete at its
stated scope; full numerical qualification and S3 native execution are incomplete; no complete profile or benchmark acceptance
is claimed.

| Area | Required evidence | Status |
|---|---|---|
| Classic 48-bit ISA | Every documented instruction class, condition, legal operand and parallel form | Partial: data/DAG transfers, fixed ALU/ROT, direct/indirect flow, normal-word pre-indexed and parallel post-indexed memory, immediate DAG modify/reverse, NOP |
| VISA 16/32/48 | All compact encodings, fetch boundaries, PC units and mode transitions | Unimplemented |
| Arithmetic and SIMD | PEx/PEy, fixed point, 32-/40-bit float, rounding, modes, flags and exceptions | Partial: fixed ALU, multiplier/MR, fixed multifunction, register-count ROT and bank/SIMD execution |
| Strict numerical mode (default) | Exact guest 32-/40-bit behavior, RND32, conversion exceptions and native-path identity | Partial, see [single-function floating compute](#single-function-floating-compute) |
| FastFP32 numerical mode (opt-in) | Independent approximation expectations, rounding/flags, unchanged raw moves/integer behavior, separate translations | Partial, see [single-function floating compute](#single-function-floating-compute) |
| Parallel operations | Old operands, memory aliases, register exchanges and ordered partial effects | Partial: raw register moves/swaps and type 1a normal-word PM/DM with supported fixed compute |
| Address generation and memory | Both DAGs, modulo/reverse addressing, PM/DM and physical view aliases | Partial: normal-word pre-indexing, immediate modify/reverse and DAG register-load dependencies |
| Sequencer and control | Delayed flow, DO/loop endpoints, LR, stacks, reset and interrupts | Partial: classic direct/indirect transfers, hardware DO and admitted reentry, reset, declared maskable vectors, admitted RTI and IDLE |
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
storage/interface checks. Core reset and IDLE have additional execution tests in
`test/core/control.cpp`. See [state.md](state.md).

S1 provides the original contract declarations, an installable `sharc::core` package,
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

Contract v9 fault attribution is implemented: `NativeSymbols::fault_map` supplies
linked executable section ranges and guest line intervals, validated and copied
by `bind`. The standalone compiler obtains them from LLVM's linker and DWARF
line tables. Guest locations use file `sharc-variant-<id>` and line `PC + 1`;
compiler-generated locations use line zero. Independent consumer linking tests
cover PC zero, the highest classic PC, full 32-bit variant IDs and malformed
maps, and compare native text with debug information stripped before O3.
The standalone compiler retains LLVM's default linker; JITLink supplies its
allocated executable sections and relocated debug objects, with RTDyld load
notifications supported separately. Fault attribution does not select a different
native code model.
Exact/Direct preparation has separate artifact identities. Direct initialized
complete normal-word views use ordered volatile word accesses with host
protection. Real Linux x86-64 SIGSEGV/SIGBUS tests cover PM/DM reads/writes,
SIMD second-word faults, dead loads, overwritten stores, top-to-zero wrapping,
native unit transitions, consumer-bound artifacts, mask restoration, chaining,
nested owners and independent threads in both numerical modes. A claimed fault
is terminal: diagnostics and completed-owner progress are defined, register
backing is not a snapshot. Exact checked faults remain precise and resumable.
Other views, peripheral fault emulation and macOS ARM64 escape qualification
remain open; this is not complete memory or v9 profile execution coverage.

Contract v10 `Memory::complete` read/write views with optional mirrored tails
are implemented for packed initialized normal-word layouts. Construction and
installation validate geometry; the consumer declares backing and tail aliases.
Linux x86-64 installed-image tests cover distinct read/write aliases in PM/DM,
store/load visibility, paired wrapping, rebinding and snapshot replay, Exact
checked denials and actual Direct write-view faults in both numerical modes.
Both memory modes use volatile word operations to preserve physical aliases.
Other formats and macOS ARM64 remain unqualified.
Preparation partitioned for concurrent consumer compilation remains open. Public
`lower`, `load` and `bind` pass a cold-start concurrent preparation test: four
workers prepare eight independent images with distinct code variants, both
numerical modes and both memory contracts. Serialized artifacts, consumer-linked
native text, complete final state and memory match serial preparation; arithmetic
and stored bytes also have literal independent expectations. This qualifies
concurrent whole-image preparation, not partitioning within one image. The current
complete normal-word layout binds read and write directions separately and
explicitly wraps paired word addresses at the top of the space.

Classic ISA support currently includes NOP, immediate moves into data/DAG registers, and
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
External conditions refuse preparation. VISA, timer evolution and remaining
shifter and memory forms remain unimplemented.

Single fixed multiplier computations include register and MR destinations,
integer and fractional signedness combinations, MAC, MSUB, convergent rounding,
saturation, clear, and MR0/MR1/MR2 transfers. Native code carries both logical
80-bit accumulators in SSA and applies SRCU to the physical banks at its boundary.
`test/core/compute.cpp` checks generated independent expectations in Strict and
FastFP32, with SISD, SIMD, and both bank selections. A multiply-accumulate sequence
also checks conditional write suppression and snapshot replay across owner bounds.
The fixed multifunction packets from Programming Reference Table 12-12 include
multiply or accumulate with ADD, SUB or AVG, plus the multiply/ADD/SUB
butterfly. Installed-image expectations check both lanes and numerical modes,
accumulator limbs, separate ALU and multiplier flags, old operands and ALU
write priority over the multiplier. The suite also checks per-lane predicate
suppression and rejection of a tied pair of ALU destinations. Floating
multifunction packets remain open.

Classic type 1a pairs normal-word PM and DM loads/stores with supported fixed
compute. Installed images exercise all four direction pairs, old operands,
DM-before-PM bus effects through an alias, per-lane DM/PM/compute writeback
priority, SISD broadcast, cross-bank modification and its DAG interlock,
circular loads and overflow, and BR0/BR8 address reversal in SIMD. A checked
refusal preserves the completed prefix before a missing PM lane. Linux
protected-page fixtures exercise ordering and terminal fault attribution on
both buses through the default and consumer-linked image providers.

Classic `IDLE` stops at retirement under either run bound. Tests cover both
numerical modes, frozen progression, reset and snapshot replay. IDLE in either
slot of a taken delayed branch keeps the delayed transfer pending. An eligible
declared interrupt wakes ordinary IDLE. Public installed-image tests cover
vector entry, masks, nesting, RTI and RTI (DB), absolute deadlines, a suspended
long DO fetch, short counted and arithmetic termination shadows, nested refills,
and replay into another Core. See [state.md](state.md) for the
admitted event model and open cases.

Non-delayed direct CALL and plain conditional RTS keep static call targets and
known return addresses inside the same LLVM function. CALL and RTS access one
architectural PC-stack cell at a time, with the depth in SSA until a unit exit.
Popped cells retain their contents. CALL writes the
return address with its CALL tag, updates PEx full/empty status and latches the
almost-full interrupt at depth 29. A return address altered by the owner takes a
cold path to another declared entry; owner dispatch uses a loop, not host recursion.
A completed return respects the current bound before requiring a new entry.
Empty/overflowed stacks and plain RTS on a loop or interrupt continuation fail
at the instruction boundary, retaining the preceding completed prefix. Admitted
reentry uses RTS (LR), and admitted interrupt return uses RTI. Architectural
stack exceptions and parallel-compute returns remain open.

Classic type 9 adds non-delayed DAG2-indirect and signed six-bit PC-relative
jumps and calls, optionally with supported single-function fixed ALU computation.
Indirect targets use the old I+M value, wrap to 24 bits and leave I unchanged;
I and M independently select their physical DAG quarter banks. Branch predicates
read the old flags. Without ELSE, SISD computes unconditionally while SIMD
computes in lanes satisfying the condition; ELSE computes in active lanes with
the inverse condition (Programming Reference 2.4, 9-35–9-38, 10-14).
Taken transfers clear the compute interlock; a fallthrough retains its parallel
compute history across bounds and snapshots. A refused full-stack parallel CALL
commits neither the computation nor the call.

Cross-unit RTS dispatch recognizes prepared CALL continuations without requiring
them to be public entries. Ordinary indirect jumps and calls still require
declared entries. Resolving a return exactly at the run bound preserves the
caller root for later execution and cross-Core snapshots; unknown targets retain
their completed prefix. `test/isa/indirect.cpp` checks both numeric identities,
all I8–I15/M8–M15 selector fields, independent DAG quarters, displacement extremes
and wrap, EQ lane combinations with/without ELSE, complete call/return prefixes,
cycle bounds, cross-Core snapshots, interlocks and refusal before parallel effects.
`test/core/native.cpp` also exercises a cached, consumer-linked indirect call.
System-register transfers, other parallel compute classes and
architectural stack exceptions remain open. This is
ISA evidence; the K4 benchmark below exercises nested direct/indirect calls.

Classic delayed jumps, calls and plain RTS latch the destination and commit
the stack effect at the transfer. A taken CALL records PC+3. Both declared
slots execute before the redirect, with one refill cycle charged at the
second slot. Slots inherit pipeline dependencies from the branch and can
fault at their own PC. A false transfer follows the ordinary sequential
path. Preparation admits adjacent complementary delayed jumps without
parallel computation and rejects other branches in slots. The ISA tests in
`test/isa/delay.cpp` cover instruction and cycle prefixes, cross-Core replay,
absolute, relative and DAG2 calls, 24-bit PC wrap, stack refusal and a slot
memory fault. `test/core/interrupt.cpp` qualifies RTI (DB) for sources without
automatic status stacking.

Long classic DO bodies of at least five words support immediate and admitted
register counts, LCNTR, arithmetic termination and FOREVER. Counted loops
update the current counter when the end word reaches Fetch1. On the final
turn the stack can pop several words ahead of the end. Arithmetic loops
test both lanes in SIMD and account for the trailing pipeline refill.
Counted nesting and complete-turn LLVM grants for eligible counted,
arithmetic and FOREVER bodies are exercised through the public interface. Tests cover zero
count wrap, retained stack cells, exact instruction and cycle prefixes,
cross-Core replay and fault attribution.

Counted classic DO bodies of one through four words retain buffered
instructions after the loop and PC stacks pop. The short-loop fixtures cover
startup and drain cycles, the count thresholds for fetch redirection, retained
stack cells, fetch and decode addresses, and snapshot replay after an early
pop. Eligible steady turns execute in the same bounded LLVM loop as long
counted bodies.

Short condition-terminated DO has separate sample points for one-, two- and
three-word bodies. It preserves the remaining pass or iterations after the stack
pop and charges the two-clock arithmetic refill at the final retirement. The
four-word arithmetic loop uses the regular five-stage fetch schedule. Authored
tests cover old ASTAT sampling, a flag changing on either word of a two-word
loop, SIMD lane agreement, FOREVER, exact bounds and cross-Core replay.
Short arithmetic fetch and decode observations derive from the documented
sample points and five-stage pipeline. They have no hardware trace comparison.
An immediate CALL on any of the last three words of an admitted counted
four-word DO suspends the remaining buffered words. The callee can run its
own DO. On a repeating endpoint CALL, the saved PC is the loop header and
the replacement end fetch samples the count at the return boundary. On
the final endpoint CALL, the saved PC is the word after the loop. `RTS (LR)`
restores a suspended buffer after both slots of a delayed return. The
installed-image tests cover a popped loop frame, a live frame, the
zero-encoded full count, false CALL predicates and snapshot replay. The
combined CALL and refill timing awaits hardware comparison.
Endpoint CALLs in counted two- and three-word bodies resume the remaining
buffer at the loop header or return to the linear successor on the final
pass. In a two-word body the return refill reaches the next counter sample
after the frozen clocks. In a three-word body the penultimate word already
sampled the count. A taken CALL in a single-iteration two-word body fails
at the CALL boundary. A false predicate retires the ordinary
loop tail. When a three-word body terminates with the endpoint CALL in
Address, the saved PC carries the loop-termination tag. Fixtures cover
direct and indirect calls, both return forms, buffered passes after a
stack pop and snapshot replay. These combined pipeline observations await
hardware comparison.
An immediate CALL on the penultimate word of a counted three-word DO
suspends the pending end sample. The return refill samples before the
endpoint executes, after both slots of a delayed RTS. The compact
single-iteration buffer keeps its drain clocks. When the replayed sample
terminates the loop with a following CALL in Address, that CALL carries
the termination tag. A false predicate takes the ordinary sample in
place. Fixtures cover both return forms, the compact drain, consecutive
calls and snapshot replay. This combined timing awaits hardware comparison.
An immediate CALL on the first word of a counted two-, three- or four-word DO
saves the buffered suffix after any startup count sample. The CALL flush
replaces the rest of the entry fill. A single-iteration three-word loop
restores the compact buffer and samples during its second word. Fixtures
cover taken and false predicates, startup termination, both return forms,
and a callee that runs a short DO of its own. Four-word fixtures also
cover the full-count encoding, IF NOT LCE on the resumed suffix and a
precise fault for an ordinary RTS on the suspended buffer. The combined
timing awaits hardware comparison.
Immediate CALLs in two- and three-word arithmetic and FOREVER bodies
retain either the active loop or its buffered exit across `RTS (LR)` and
`RTS (DB, LR)`. A CALL at the two-word endpoint or the three-word header
can defer the arithmetic sample to the fourth return fetch. That sample
sees flags from the callee, including the last slot of a delayed return.
A following CALL in Address carries the termination tag. Tests cover both
sample outcomes, continuation after a tagged CALL, a taken endpoint CALL
that replaces the final refill, and a callee that uses its own short DO.
Single-word bodies exclude CALL. The combined timing awaits hardware
comparison.
Admitted immediate CALLs in the tail of a long DO preserve the sampled loop
decision across `RTS (LR)` and `RTS (DB, LR)`. The PC stack records a separate
tag when the loop terminates during the call's address stage. Tests cover
continued and terminating counted turns, a DAG2 call into a separate prepared
entry, delayed return slots, suspension of an arithmetic refill while the callee
runs its own DO, and a precise fault for an ordinary RTS on a suspended tail.
An interlock that holds the CALL in Decode delays its address stage past
the counter sample. The saved PC has no termination tag in that case.
Installed-image fixtures exercise this timing in short and long bodies.
Qualified classic IF NOT LCE uses the end-fetch decision in long counted
DO tails and in buffered one- through four-word bodies. At a counted
header reached from its DO, the predicate observes the newly loaded
count with a one-cycle compute or two-cycle branch interlock.
A backedge to the same header follows the normal sample history.
The tests exercise immediate, register and LCNTR setup, SIMD compute,
both header CALL predicate outcomes, and snapshot replay.
The startup timing is derived from the programming reference's
conditional-branch stalls and loop timing tables.
Buffered passes retain sampled decisions after the loop stacks pop and
across CALL and RTS (LR). A held startup fetch ages through its entry
bubble. The tests exercise the separate branch and Execute latencies,
a DAG2 interlock and complete-turn grants for nonterminating counted passes.
An admitted long counted parent tracks the sample clock during a short
child's drain or a long arithmetic child's refill. A counted child preserves
its own observation while the parent samples. Installed-image fixtures cover
simultaneous pops, all four endpoint gaps for long counted children and
arithmetic children of one to three words, parent samples preceding child
setup, and parent outcomes at successive refill clocks. The long counted
child is also exercised across immediate and delayed `RTS (LR)`.
Three- and four-word counted outer loops keep their own expiry observation
while a short child runs or drains. A counted child has an independent
observation. Tests exercise both outcomes in the overlapping buffers,
a parent sample on the last clock of a child refill, and preservation of
the parent's sampled expiry across both return forms after the child drains.
Explicit loop-resource changes, other overlapping configurations, predicates
in the middle of long bodies and unlooped predicates remain unqualified.
Combined condition timing awaits hardware comparison.
An immediate CALL at the endpoint stores the next loop header or the word
after the loop as its return address. On a terminating arithmetic turn the
taken call replaces the loop refill. A false predicate keeps the normal
loop backedge or exit refill. Installed-image fixtures cover both backedge
and exit returns through the connected and separate prepared entries.
The endpoint CALL timing is derived from the branch and loop pipeline
tables. Comparison with a hardware trace remains open.
An immediate CALL at the instruction whose retirement would sample the
end fetch or arithmetic condition defers that sample to the return refill.
The ordinary RTS form admits a return to the first, still-unsampled end
fetch. A deferred arithmetic condition uses the callee flags, including
the final `RTS (DB, LR)` slot. A false CALL samples on the ordinary loop
path. Installed-image tests cover counter timing, direct and separate
DAG2 callee entries, SIMD lane disagreement, both return slots and
snapshot replay. The sample executes at the prepared return address
before admitting another guest instruction. The combined timing follows
the documented pipeline and awaits hardware comparison.
An in-loop JUMP can reach the last four words before the ordinary end fetch.
The target refill samples a long counted endpoint once or selects the
speculative arithmetic backedge and tests its condition. Delayed jumps
advance that fetch stream at each slot boundary, preserving the sampled
state and fetch address in snapshots. The arithmetic test sees the first
slot's flags if it coincides with the second slot, and the second slot's
flags if it occurs in the refill bubble. Installed-image programs cover
absolute, PC-relative and declared DAG2 targets, both count outcomes, a
delayed one-level abort into a parent tail and an arithmetic child whose
branch refill reaches a counted parent's end. In the fourth- and
fifth-last words of the source loop, plain JUMP needs a static in-loop
target.
This timing is derived from the documented pipeline and awaits hardware
comparison.
Classic JUMP (LA) handles qualified long-loop exits through absolute,
PC-relative and DAG2 targets, with either immediate transfer or two delay
slots. A false predicate leaves the hardware loop's outstanding decision
active. A taken exit uses the active frame or the already-popped counted
tail, preserving the parent frame. Authored tests cover precise refusal for
targets inside the current loop, exits across two levels and exits to the
endpoint of an arithmetic parent.
When an admitted long arithmetic child finishes within four words of the
parent endpoint, the parent's Fetch1 event follows the child's actual drain
and refill clocks. A parent arithmetic sample in the final refill clock sees
the flags written by the child's last instruction. The checked path and the
LLVM grant for full child turns share the transition logic. Installed-image
tests cover countdown timing at all four gaps, snapshot replay, an arithmetic
parent and a child refill overlapping an already-popped counted parent with
a pending CALL. These combined pipeline timings are derived from the manual.
For short children within long loops, Fetch1 can reach a parent endpoint
while buffered child words still execute, including during a compact entry
or exit bubble. The parent keeps any earlier fetch decision across the
short child fill. Tests exercise counted bodies of one through four words,
a short arithmetic child, a late arithmetic parent sample and snapshot
replay with both active and already-popped frames. The overlap timing is
derived from the documented pipeline observations.
Counted three- and four-word outer loops and three-word `DO FOREVER`
can nest a short DO with a distinct earlier endpoint. The outer buffer
survives the child, including when the outer counted frame has already
popped. Tests cover one- and two-word counted children, a one-word
arithmetic child, both child positions in a four-word parent, and a
counted child that admits complete native turns. Fetch and decode
observations across the handoff are derived from the short-loop timing.
When both short buffers are nested in a long DO, their fetch stream still
reaches the long endpoint at the correct point in the drain. Tests exercise
all four counted outer endpoint gaps, another middle pass, a compact
middle loop, and a long arithmetic condition sampled across the handoff.
The authored expectations use the documented fetch and decode timing.
An immediate CALL in an overlapping short child saves the outside fetch
position, sampled conditions and any suspended short parent. `RTS (LR)`
and `RTS (DB, LR)` restore that context. The return can finish a displaced
ancestor sample or resume a short parent after a terminal child CALL.
Fixtures exercise an already sampled ancestor, an unsampled ancestor,
callee-owned nested short loops and the last return slot's arithmetic
flags.
An immediate CALL on either of the final two words of a long arithmetic
child suspends the enclosing tail and the child's pending refill.
`RTS (LR)` and `RTS (DB, LR)` restore the tail. On a returning backedge
the child continues at its header. On a terminating pass the return
refill reaches enclosing end fetches displaced by the CALL. A taken
endpoint CALL replaces the child's own final two refill clocks.
Fixtures exercise all four gaps to a counted parent, its `NOT LCE`
latency and termination tag, and a displaced arithmetic parent sample
after a separate callee runs its own DO and sets SIMD flags in the last
return slot. Consecutive CALLs can independently sample two counted
ancestors.
Immediate CALLs in shared long tails restore sampled conditions and
defer an outstanding fetch or arithmetic test to the return refill.
The terminating pass of a counted child can reach the parent's sample.
A repeating endpoint CALL returns to the child header. Fixtures cover the four
counted parent gaps, a CALL following a sampled parent, false `NOT LCE`
predicates, a prepared edge that bypasses the child setup, and the
arithmetic child and parent tests across separate return entries.
Other branches in an overlapping refill remain unqualified. The combined
timing awaits hardware comparison.

Classic type 5 transfers raw data among R/S and I/M/L/B registers, including
cross-PE data moves and raw40 register swaps, with supported single-function fixed
ALU computation. Both effects read the old operands and predicates; admitted UREG
writes take priority over ALU data while the ALU flags and sticky effects survive.
Complementary data destinations use their own lane predicates; a shared DAG
destination uses PEx only. This follows PRM 2.4 tables 4-39/4-44 and the discussion
on 4-102–4-103; the type-5 example on 9-24 instead describes an AND predicate.
That inconsistency remains unverified on a primary implementation and prevents
claiming full Strict qualification for this case. Immediate type 17 also writes
S0–S15 and all DAG fields; raw transfers do not round in FastFP32.

DAG loads retain two recent physical-register dependency tags across owner
bounds, native transfers and snapshots. Address generation after I/B/L loads of
the same index stalls two cycles, or one with an intervening unrelated cycle;
M dependencies require the same M register (PRM 4-111–4-116). Nominal cycles
include the actual stall and a deadline finishes the entire instruction. Branch
bubbles drain that history; concurrent Decode/Address interlocks use the longest
stall. Acyclic grants reserve the worst case and account actual cycles. Current
fixed-cost loop grants require empty incoming DAG load history and no DAG loads
in the loop; other cases use the same lowering in the cold exact path until
proved cyclic dependency costs are available. Standalone MODIFY/BITREV does not
create a UREG-load tag; its existing consumer-timing qualification gap below
remains open.

`test/isa/register.cpp` compares complete register state for every prefix, both
numeric identities, all DAG fields and register selector fields, independent DAG
quarter/data bank configurations, conditional SISD/SIMD moves/swaps, extension
bytes and old-operand/ALU-priority cases. Memory and indirect-branch fixtures check
zero/one/two intervening instructions, matching and unrelated I/M/L/B loads,
consecutive pending loads, conditional loads, overlapping compute/branch stalls,
cycle deadlines and cross-Core snapshots. Cached consumer-linked IR also loads
B/L/I and consumes them in a circular MODIFY. This is ISA evidence; K4
performance is measured separately in the benchmark layer.

Register-count ROT projects the full signed 32-bit count, rotates the fixed-point
word modulo 32, clears the destination extension byte, updates SZ and clears
SV/SS while preserving other flags and STKY (PRM 2.4, 11-63). It uses the same
conditional lane and writeback machinery as other computations.
`test/isa/rotate.cpp` checks every source bit, zero/full words, large positive and
negative counts including both signed extrema, destination/source/count aliases,
independent data banks, SISD/SIMD predicates, ALUSAT/RND32 preservation and both
numeric identities against an independent bit permutation. A shifter-condition
branch checks its interlock across a cross-Core snapshot. Standalone classic
type-6 immediate ROT uses the same LLVM rotation and writeback with a signed
8-bit immediate count. Its installed fixtures cover all 256 count encodings,
zero/full words and every source bit, source/destination aliases, distinct
physical read/write banks, SISD/SIMD predicates, raw extensions, flags and sticky
preservation under all bank/SIMD/ALUSAT/RND32 selections, both numerical modes
and whole-unit runs against instruction/cycle prefixes with snapshot replay.
Both count forms check
the shifter-condition branch interlock. Nonzero unused/DATAEX fields, other
immediate shifter operations, parallel memory forms and full shifter qualification
remain open. See `encoding.md` for standalone-prefix reference evidence.

Classic type 15 transfers use direct normal-word PM/DM layouts, pre-indexed
32-bit displacements, physical DAG/data banks, explicit R/S registers, SIMD and
read broadcast. Full raw registers and padded storage are checked at every
instruction/cycle prefix by `test/isa/memory.cpp`. `test/core/memory.cpp` checks
precise access/refusal prefixes, rebinding, declared code-storage reads, protected
code-alias writes, ambiguous/truncated layouts and missing implicit SIMD storage.
The core memory suite checks short-word SSE and SIMD stride, PM/DM aliases,
long-word neighbor registers and broadcast, and raw 40-bit transfers. It checks
declared callbacks for access order, refusal, yield, delay-slot continuation and
replay, including 40-bit payloads. The Core control suite checks prepared
handoffs on direct, computed and delayed control flow, rejected installations,
generation rebinding and snapshot replay. The memory suite checks coherent
short and normal stores through PM/DM code aliases, including unchanged bytes
and binding revocation on a later I/O fault. It also checks transfer in a live
delay slot. The native suite checks active counted-loop handoff and rejection of
an incompatible loop plan through a cached, consumer-linked artifact.
It also checks consumer-linked read and write roles through a cached artifact,
including owner binding, yield after retirement, access fault and replay.
Forced long-word encodings remain open. See
[memory.md](memory.md) for the exact storage contract.

Classic type 19 implements immediate MODIFY/BITREV, including distinct source
and destination I registers. The source B/L define circular adjustment before
bit reversal; MODIFY ignores CBUFEN for nonzero L. All sixteen I registers, all
within-DAG destination choices, both physical banks in each DAG quarter,
positive/negative adjustment, wrapping and raw bit reversal are exercised by
`test/isa/dag.cpp` in both numeric identities. Each prefix compares all registers
under instruction and cycle bounds. SIMD does not duplicate the DAG operation.
Canonical I7/I15 updates test the documented sticky/interrupt latches, including
CBUFEN with L zero. Overflow routing when an enhanced I7/I15 operation writes a
different I remains unverified: the implementation follows the source buffer,
and the enhanced matrix deliberately does not claim that overflow case. Immediate
consumption timing after a standalone MODIFY/BITREV has not been observed on a
primary implementation; the current nominal one-cycle operation follows the DAG
operation model. These gaps prevent complete Strict profile qualification.

Changing I values remain SSA across loop grants. Complete normal-word spaces
use the current index directly without access guards. Changing sparse mappings
currently select the cold exact path pending their affine proof. Core fixtures
check moving-index scalar/SIMD copy and checksum loops, exact sparse failure and
cross-Core snapshots. Cached consumer-bound IR preserves circular mode and DAG
bank metadata. The installed SDK also executes an index update and a subsequent
load. These authored controls are not K5 measurements.

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
Nested authored calls check full raw state at every instruction/cycle boundary
in SISD/SIMD and both mode identities, including nonempty initial stacks. A
callee snapshot replays in another Core; changing its return cell exercises the
guarded cold transfer and undeclared-target boundary. Absolute/relative CALL
and RTS predicates reuse independent paired-status records under both numerical
identities. False predicates preserve unsupported stack states; unsupported
empty, overflowed or tagged returns stop without retiring or changing registers.
Reserved branch fields refuse preparation; legal but unimplemented modifiers
refuse explicitly. These are ISA checks, not K4 benchmark results.
Each prepared unit starts at one declared entry and includes its reachable direct
control flow. Distinct entries receive independent units; a saved continuation
retains the entry of its current unit, including after a native tail transfer. Normal entry links directly into that flow; only
cold continuation resumes dispatch on PC. Artifact metadata preserves the entry
identity across loading and binding. Disconnected and overlapping entry paths,
owner PC replacement and cross-Core artifact-bound replay compare complete state.
Simple contiguous backedge loops receive a whole-turn grant and account on exit;
acyclic control flow uses a conservative unit grant. Other cycles use the cold
exact path until their loop plan is implemented. Independent five-turn programs
compare complete state at every instruction and deadline, in both numerical
identities, SISD/SIMD and both low data banks. The optional poisoned-state build
runs the same tests with stale pinned fields guarded. Full PM/DM virtual mappings
and cross-unit flag recipes remain open; no performance result is claimed here.

`test/core/native.cpp` checks cache round trips/refusal, consumer-linked LLVM IR,
precise bounds and undeclared exits, stopped installation, code validation,
memory rebinding and snapshot replay in another Core. The installed CMake
consumer executes its own literal instruction program, snapshots inside a callee
and replays the return. Cached bitcode also executes a call/return after loading
and binding through consumer-linked IR. Cached memory IR is also loaded and
bound with alternate DAG/data banks and broadcast.

Linux x86-64: Clang/LLVM 19.1.7 Release; all 16 CTest checks pass, including an
installed consumer executing native instructions. clang-format and clang-tidy
are clean. macOS ARM64 remains untested. The benchmark layer includes K4:
a table-driven eight-state machine with nested calls, register-count rotation,
complete registers/table/stack output and exact progress in both identities.
K5 checks a full 4096-word copy and running checksum repeated 512 times under
both numerical identities, including guard bytes and precise progress.
See the [current benchmark results](../test/bench/README.md) for inputs,
independent references and unscaled Linux hardware counts. K1–K3, K6 and
goal sanitizer qualification remain open.

## Single-function floating compute

Classic type 2a lowers floating ADD, SUB, their absolute forms, average,
multiply, PASS, negate, ABS, RND, COMP, COPYSIGN, MIN, MAX and CLIP. It also
lowers FLOAT, FIX and TRUNC with and without a scale, SCALB, MANT, LOGB,
FPACK, FUNPACK, RECIPS and RSQRTS. Strict applies the instruction-specific
guest rounding and precision rules. FastFP32 rounds
computational inputs and results to binary32 with the guest rounding direction.
Both identities use the raw register file and leave guest MODE1 visible.

`test/isa/floating.cpp` runs independently authored instruction fields and
14,288 paired expectations, split evenly between Strict and FastFP32,
through installed images. It checks full register state,
arithmetic flags, comparison history, sticky flags, and progress for admitted
blocks and partial bounds in SISD and SIMD, with alternate banks and separate
lane predicates. A raw register move carries an extended midpoint between
physical banks. A floating compare of that copy drives a conditional fixed
PASS across the mode-specific precision boundary, and a later compare preserves
CACC through an overwrite of the ordinary ALU flags. The sequence runs with upward
host rounding and checks the unchanged host rounding mode. The preparation
suite round-trips the Strict artifact and checks cross-mode refusal.

The matrix verifies negative zero and NaN flags, RND32 and explicit RND
classification and output widths, COMP history on unordered inputs, and the
minimum-normal boundary. It distinguishes Strict RND32 truncation from
FastFP32 operand rounding and handles extension-only NaN inputs in each mode.
It verifies the signed invalid result from floating multiplication. The
original operands and guest MODE1 retain their full contents. The observed
Strict boundaries and their reproduction procedure are recorded in
[numeric-reference.md](numeric-reference.md#adsp-21489-simulator-observations).
Classic FLOAT and FLOAT BY preserve signed fixed-point inputs and scale
operands, and produce extended Strict or binary32 FastFP32 results. The
matrix covers integer low bits, ignored raw operand extension, exact zero,
overflow, signed underflow, and extreme scales in both widths and rounding
modes. SCALB covers signed fixed scaling, infinity and NaN classification,
and the midpoint where minimum normal and AUS occur together.
FIX and TRUNC check signed rounding and saturation, with full invalid-result
flags and raw widths. MANT and LOGB check projected mantissas, fixed exponent
outputs, signed-zero and infinity handling, and NaN sentinels. FPACK and
FUNPACK cover short subnormals, inexact packing, clipping and shifter flags.
RECIPS and RSQRTS exercise both endpoints of every observed seed bin,
with signed and exceptional inputs and unchanged inactive lanes. Preparation
tests also load an artifact containing a seed table.
Floating multifunction execution and floating parallel-transfer qualification
remain open.
Floating-workload performance is tracked separately from the fixed K1–K6 identities.

## S2 independent expectations (complete milestone, numerical gaps retained)

`test/support/reference.py` supplies a separate exact integer/rational numeric
model. `generate_expectations.py` checks 311 numeric/conversion/accumulator/shifter
anchors, 48,768 normal-range rounding positions and 27,648 limb-based multiplier
cases and can emit deterministic machine-readable expectations without loading
production code. Fixed ALU records include complete
modeled scalar flags and sticky effects, including fixed 80-bit arithmetic,
saturation, rounding and accumulator transfers. FIX/TRUNC records cover
integer, saturated and invalid results with flags, including post-round range
checks and pre-round AUS. 12,800 signed-quotient comparisons cross-check both
conversion instructions. Floating value-only records remain
separate from new qualified complete ALU, compare, MANT/LOGB and multiply effects.
The latter pass 35,088 independent full-result checks.
Another 130,817 shifter, 15,449 bit FIFO and 65,536 short-float unpack cases use
independent bit routing, bit lists and rational value checks. Scalar shifter
expectations preserve unrelated flags and sticky bits. FIFO records refuse
undefined data/pointer results rather than supplying a guessed complete state.
Another 13,072 multifunction cases check dual-ALU flag unions, disjoint ALU/
multiplier flags, old operands and accumulator effects. Authored paired-lane
records include different old status/sticky bits and independent exceptions.
The installed fixed multifunction fixture uses an independently generated subset
of these numerical expectations. The remaining records do not qualify native
execution.
Another 134,092 FPACK field checks and 896 complete seed-special-case checks
cover exact and inexact short values, exponent clipping, signed zeros, infinities
and invalid seed inputs. The installed fixture adds full FPACK storage and status
checks and compares native seeds with independently observed words at every ROM index.
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

The S2 generation criterion is met. Remaining shifter and FIFO cases,
full conditional execution/bank composition
and parallel memory ordering expectations are still required. See
[numeric-reference.md](numeric-reference.md) for the exact scope, source sections
and gaps. Oracle validation is not installed-image ISA execution coverage. No
complete numerical or profile acceptance follows from these oracle checks.
Native coverage is limited to the separate subset above; benchmarks remain open.
