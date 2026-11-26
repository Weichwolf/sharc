# Target architecture

This is the implementation target, not a claim of complete execution. Initial
LLVM lowering executes a limited classic ISA subset. Numerical qualification,
complete control flow, remaining memory instructions and the complete profile
remain open; see [isa-coverage.md](isa-coverage.md) for current evidence.

```text
consumer: CodeImage + CompileOptions -> Core::lower -> SemanticImage (LLVM bitcode)
consumer: link device IR, optimize, cache -> Core::bind -> NativeImage
consumer: Core::install on a stopped owner -> run / run_until -> save / restore
```

The interface, lifecycle, verification layers and benchmarks are defined in
[core-contract.md](core-contract.md) and are not repeated here.

## Ownership and public surface

Expose only include/sharc/core.hpp and memory.hpp as implementation proceeds.
Use namespace sharc and CMake target sharc::core. Keep LLVM types and native-code
details private. Core owns architectural state, progress, failure context and
translation lifetime. Consumers own storage, device models, clocks and scheduling.
One owner executes and mutates a Core. Separate cores can execute independently;
cross-core synchronization belongs to the consumer.

Execution, reset and memory mutation must have explicit ownership and reentry
rules. State inspection returns complete architectural values at resumable boundaries;
it must not force state materialization on every guest instruction. Expose useful
fault context and precise guest prefixes at observable Exact exits. A terminal
Direct host fault defines diagnostics and completed-owner progress only; its
partially materialized register backing is not an architectural snapshot.

## Translation and memory

The consumer declares executable code: spans, instruction entries, variants,
interrupt vectors, transitions and memory layouts. The core decodes that declared
code once during `lower`, normalizes encodings into reviewed architectural
operations and keeps the semantic order of old reads, computation and writes.
Nothing is decoded, compiled or looked up on an admitted execution path. Real
device callbacks are external effects. A code or mapping change through any view
revokes the binding; execution continues only after a declared variant is
installed. Record code identities and dependencies by explicit generation, never
by hashing guest memory at run time.

### Lowering rules, measured on cpu12 and dsp56300

These rules come from whole-instrument measurements of the Blofeld emulator
(Weichwolf/blofeld-src issues #32, #34, #35). Start with them.

1. **Lower every instruction directly to LLVM IR.** Architectural registers are
   SSA values inside a translation unit. Pass the active hot register set as
   arguments between native units under one `preserve_none` signature and use
   `musttail` for chaining. Load and spill that set only at the owner boundary.
   Do not clone an interpreter step per instruction and rely on the optimizer to dissolve it: dsp56300 did, and it cost 54 to 62 host
   instructions per guest instruction.
2. **One lowering path for the whole instruction set.** No fast subset beside a
   slow general path. In dsp56300 the fast path covered two thirds of the
   executed instructions and the remaining third carried most of the cost,
   including the hottest loops.
3. **Admit a grant before executing it.** Prove enough budget for an acyclic
   unit or select the number of complete loop turns before entering the loop.
   Account a loop grant at its exit. The admitted path has no instruction or
   turn budget checks. A failed grant uses the same instruction lowering in a
   cold exact tail, where instruction checks finish the requested bound.
4. **Compute status bits where they can be observed.** Keep flag recipes until
   a reader or a complete owner exit needs them. Analyze flag liveness across
   static unit edges and admit the successor before dropping dead flag recipes.
   Sticky flags accumulate even when ordinary flags are dead. In the dsp56300
   per-instruction path 33 of 39 host instructions of an ADD were status work.
5. **One processor or numeric mode per prepared unit.** Never emit all modes side
   by side at each site and select at run time. dsp56300 produced 27.8 MB of
   native text that way, and on x86 half of the pipeline slots were lost to
   instruction delivery.
6. **Mark exits cold.** Fault, refusal, bound and unprepared paths carry branch
   weights and stay out of line, so the executed code is dense.
7. **State lives in registers, not in core memory.** Guest RAM is consumer
   storage reached through declared layouts. Use consumer-lent virtual address
   spaces for PM and DM and lower word views over their physical backing.
   Host protection provides the eventual cold I/O boundary; qualify plain
   faulting loads and stores before adding that handler.
8. **Cacheable preparation and complete save and restore from the first usable
   Core.** Both are contract requirements and both were expensive to retrofit.
9. **Idle loops are data.** A firmware loop pass that changes nothing can be
   retired arithmetically up to the run bound; keep the lowering able to prove a
   pass is a fixed point (blofeld-src #35).

A unit has one declared entry into connected control flow. Known calls and
returns, loop setup/body/end and both conditional successors belong in that
flow; bounds, events and indirect transfers leave through cold exits. Counted
loops with a proved fixed iteration cost should obtain a whole-iteration grant
from the remaining bound and account at the exit. Guard returns against an
altered architectural return address. These requirements also apply as calls
and hardware loops are added to the initial native subset.

The current native ABI passes R0–R5 and their paired raw values, both ASTAT/STKY
pairs, execution bounds and progress as arguments. Other data registers and the
PC-stack depth remain SSA within a connected unit and are materialized at unit
exits. CALL and RTS access individual cells in the owner’s PC stack. Taken
returns end the current grant and admit the checked continuation. Indirect
transfers use a prepared PC/MODE1 dispatcher. Returns may
also select prepared CALL continuations; ordinary indirect transfers admit only
declared entries. The selected caller root survives an exact return bound and
its snapshot. A shared reload and spill handle the pinned set; an explicit
`SHARC_POISON_NATIVE_STATE` build verifies that its old Core fields stay untouched
while native code runs. This build selection changes prepared-code identity.
Used I/M/L/B fields remain SSA within each unit; unused DAG PHIs are omitted.
A packed 32-bit pipeline argument carries the preceding-compute interlock and
two physical DAG-load dependency tags under the existing native signature.
Owner exits and snapshots preserve the same history. Fixed-cost loop admission
proves empty incoming load history and propagates that fact into its body;
acyclic grants reserve possible stalls and account the actual instruction costs.
Complete normal-word PM/DM views support separate lent read/write reservations,
optional mirrored tails, explicit Direct preparation and a
terminal host-fault adapter; see [memory.md](memory.md). Cross-unit flag recipes,
remaining memory views and runtime data callbacks remain implementation work. See private issues
blofeld-src #36/#37 and lead-a1-src #2 for coordination.

Hoist checks only with a valid alias or permission proof. Keep the host stack
bounded across guest loops. Host SIMD is used only where guest dependencies and
the numerical contract are proved and the benefit is measured.

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

Both floating contracts below remain implementation requirements. Integer
execution already separates their prepared identities. Consumers explicitly
choose a mode when creating a Core; the selection stays
fixed for that Core's lifetime. Strict is the default. Include the mode in the prepared-code and cache identities so native code cannot
cross numerical contracts. Keep the existing minimal public-header shape; do not add a public
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

Follow sections 7 and 8 of the contract. Three test layers only: `test/isa`
(independently authored programs with independently derived expectations,
organized by operation class, addressing mode and processor or numeric mode),
`test/core` (the public interface and lifecycle: run bounds, faults, interrupts,
code change, transitions, save and restore, artifact load) and `test/bench`
(kernels K1 to K6, which also check complete output). Every test drives the
public interface with declared, installed images. There is no single-instruction
capture entry point and no test of internal structures; cpu12 and dsp56300 had
both and had to park 291 test files when those entry points were removed.

`test/support/isa.hpp` is an independently authored fixture builder with explicit
instruction fields, not a wrapper around the production decoder. Include encoding
collisions, rejected forms and precise partial exits. An instruction form that
is not lowered refuses preparation with identity, address and reason; that
refusal is itself tested.

Write the benchmark kernels early, K6 (a kernel under a run bound every 256 base
cycles with periodic interrupts) first. Report host instructions and host cycles
per guest instruction for each kernel with every increment that touches lowering.
No real-time, cycle-accuracy or cross-host claim follows from configuration alone.

The first implementation increment adds real build targets, the public headers
of the contract and independent fixtures together; no empty library and no fake
passing test.
