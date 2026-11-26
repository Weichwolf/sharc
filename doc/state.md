# ADSP-21489 architectural storage

The public `Registers` definition stores physical architectural bits, independently
of the selected computation precision. Default initialization supplies zeroed inspection
storage. The reference is the SHARC Processor Programming Reference 2.4,
chapters 2–6 and appendix A, identified in `reference-manifest.json`.

| Public member | Architectural representation |
|---|---|
| `pe[0]`, `pe[1]` | PEx and PEy; each has two physical banks of sixteen raw 40-bit registers |
| `pe[].multiplier[0..1]` | Foreground/background 80-bit MRF/MRB or MSF/MSB, split into unsigned 32/32/16-bit limbs |
| `pe[].bit_fifo`, `bit_fifo_write_pointer` | 64-bit shifter FIFO and raw seven-bit write pointer (normal occupancy 0 through 64) |
| `pe[].astat`, `pe[].stky` | Independent status/sticky bits for both lanes |
| `dag[bank][0..15]` | Primary/alternate I/M/L/B; indices 0–7 are DAG1, 8–15 DAG2; raw 32-bit values |
| `pc`, `faddr`, `daddr` | 24-bit execution, fetch and decode addresses; ISA counts 48-bit words, VISA counts 16-bit parcels |
| `pc_stack`, `pc_stack_pointer` | Thirty 26-bit entries, including CALL/loop/interrupt tags; pointer 0–30, overflow value 31 |
| `loop_address_stack` | Six raw 32-bit LADDR entries: endpoint, termination condition and loop type |
| `loop_counter_stack`, `lcntr` | Six 32-bit counters and the next loop count; top counter supplies CURLCNTR |
| `loop_address_depth`, `loop_counter_depth` | Independent depths, 0 through 6 |
| `status_stack`, `status_depth` | Fifteen MODE1/ASTATx/ASTATy frames and depth 0 through 15 |
| `px` | Raw 64-bit exchange register; PX1 is bits 31:0, PX2 bits 63:32 |
| `mode1`, `mode2`, `mmask`, `flags` | Raw control, interrupt mode mask and core flag interface bits |
| `irptl`, `imask`, `imaskp`, `lirptl` | Core interrupt latch/mask/nesting registers |
| `ustat[0..3]` | USTAT1 through USTAT4 |
| `tperiod`, `tcount` | Raw 32-bit core timer period and count |
| `emuclk` | EMUCLK/EMUCLK2 low/high 32-bit cycle counter halves |

R/F and S/SF names are views of the same bits. Data registers use bits 39:0
of each host integer; a guest 32-bit view occupies bits 39:8. Selection through
MODE1 does not swap the physical bank arrays. Accumulator limbs are raw two's
complement storage, not three independently signed numbers. PCSTK, LADDR and
CURLCNTR are views of the stack tops, not duplicate independently mutable fields.
BFFWRP retains all seven bits: page 11-89 defines a write above 64 as an SV
condition. The storage boundary must not truncate those raw bits to a valid FIFO
occupancy. Instruction effects and undefined FIFO overflow/underflow behavior
still require separate execution qualification.
No host C++ object layout is a file or generated-code ABI.

## Reset

`Core::reset(address, instruction_set)` resets the defined core controls and
empty stack depths according to Table A-18. An empty stack's architectural
top value is a register view. Its inactive cells retain their raw backing.
Data banks, DAG fields and the other registers with unspecified reset values
retain their contents. FLAGS uses the pin-level bit positions in Table A-5.
Timer count and period retain their contents in accordance with Chapter 5's
timer initialization rule. MODE2 disables the timer.

The consumer supplies the restart PC and instruction set. The core initializes
both empty pipeline addresses to that PC, clears pending transfers, faults and
yield requests, and retains the installed image and elapsed progress. Execution
resumes through a declared entry. A VISA restart records its address and mode
in state. VISA execution remains open. A previously claimed terminal host
fault prevents reset under the owner contract.

Classic `IDLE` retires once and stops execution before its successor. In
straight-line code Decode holds the successor and Fetch1 stops two words
ahead, as shown in Table 4-12. Later runs return `sleeping` with zero progress.
A taken delayed branch held by `IDLE` in a slot remains pending and blocks
interrupt release. The consumer accounts for elapsed time outside the run
bound while the core sleeps. A snapshot preserves the sleeping boundary.

## Interrupts

`InterruptRequest::source` is the IVT priority slot in Programming Reference
Table B-2. Its bit in IRPTL/IMASK/IMASKP or LIRPTL follows Tables A-12 and A-13.
The consumer chooses a pending source and supplies its declared vector PC.
`interrupt_ready` checks the installed route, the global and individual masks,
the active service priority, the first ISR instruction, loop interrupt shadows,
and a pending delayed transfer. On entry, `interrupt` clears the source latch,
marks the source active and pushes the tagged return PC. It charges the four nominal vector
refill cycles without retiring a guest instruction. Latches for sources in
service are kept clear at execution boundaries. A sleeping core wakes
through the vector and resumes after IDLE on RTI.

IRQ2 through IRQ0 and the two timer priorities also push both ASTAT registers
and MODE1, then clear the MODE1 bits selected by MMASK. Classic RTI restores
that frame and clears the source's latch and active bit. RTI (DB) is admitted
for sources without automatic status stacking. The PC stack pops and service
ends when RTI retires. The two slots complete before the redirect. A snapshot
captures the nested service frames, the interrupted native continuation and
suspended loop fetch state, including during a delayed return. Emulation events,
internal timer evolution, external stalls, software interrupt control and
simultaneous branch Address and loop-end Fetch1 admission remain profile
coverage items.

Short counted DO setup inhibits service on the next clock for one-, two-, and
four-word bodies. For expiry in counted bodies shorter than four words and for
arithmetic termination, the loop sample and the three following clocks inhibit
service. The hidden deadline uses nominal base cycles and survives a snapshot.
Native code records it at a terminating sample, including samples reached during
a whole-turn grant or a branch or child-loop refill. Variants without declared
interrupt vectors omit this tracking.

## Continuations

`State` adds cumulative instruction/base-cycle progress, instruction set, execution
state and a latched fault. `Snapshot` is an immutable owning copy of the current
state, yield request, prepared continuation identity/entry/address and preceding-compute
history, tagged with the numerical mode. That history preserves the conditional
branch interlock across run bounds and snapshots. Non-delayed CALL/RTS also retain
every PC-stack cell, including popped entries; changing a return cell while
stopped is observed by the guarded native return. Restore replaces that
state, including clearing a later fault. It does not replace consumer memory or
bindings. `set_registers` preserves hidden continuation when PC is unchanged;
replacing PC starts a fresh declared entry and clears its pipeline history.
Snapshots retain the preceding-compute interlock, two recent physical DAG
register-load tags and a taken classic delayed transfer's destination and slot
phase. Cross-Core replay preserves the destination even when a slot changes a
contributing DAG register. Mode-register writes and declared code transfers
still need execution and snapshot qualification.

For admitted DO forms, snapshots retain the PC-stack ownership of each
hardware loop frame and its fetch decision. They also retain the pending
arithmetic refill and short loop words still buffered after an early stack pop.
A pending counted tail and an arithmetic refill have distinct storage, so
the outer counter may terminate while the inner arithmetic tail is draining.
For a short child, the snapshot also preserves how far Fetch1 has advanced
past its endpoint while buffered child words remain.
A short outer DO keeps its remaining body words and descriptor while the
inner DO owns the short fetch buffer. The child's drain can see addresses
from the next outer pass. The child endpoint restores the outer suffix
after the inner frame has popped, even when the outer stack frame popped
earlier. Exact bounds and snapshots retain both buffers during the handoff.
If a long DO also encloses both short loops, it sees the fetch stream
after applying the middle loop's redirects. The handoff carries fetch
progress forward relative to the middle endpoint and preserves any outer
decision already reached during the inner drain.
A counted DO loads LCNTR once. The current count then evolves on the loop
counter stack while LCNTR continues to hold the setup value.
A qualified CALL saves a suspended terminating tail with its PC-stack entry.
For a delayed `RTS (LR)`, the tail becomes active after the two return slots.
An overlapping short CALL also saves the ancestor fetch position and any
suspended short parent with that PC-stack entry. The callee uses its own
buffers and observations. The prepared return restores the caller's state
and processes the surviving fetches, including after a terminal child CALL.
When a prepared DO tail uses IF NOT LCE, the counted tail also carries
the clocks elapsed after its terminal fetch. This observation survives a
stack pop, a return refill and a cross-Core snapshot. A short CALL saves
the expiration observation with the remaining buffered words. The
ordinary short body clears it when its last buffered word retires.
Overlapping counted tails have independent observations, including two long
tails. Preparation assigns distinct storage to overlapping long tails. The
saved CALL payload identifies the storage to restore after a return. A parent
sample reached during a short drain or long arithmetic refill records the
clocks from that fetch to the next retirement boundary.
A short counted parent and child also keep separate observations. When the
child's final clock reaches the parent's sample, the restored outer suffix
starts with the fresh observation. Later CALL reentry restores the observation
to the slot of the short loop that owns the continuation.
Admitted CALLs in counted two-, three- and four-word loops keep the
buffered instruction count with the PC-stack entry. The callee can use its
own loop buffer. The saved buffer is restored at the prepared return
address. On a repeating endpoint CALL in two- and four-word bodies the
return refill samples the loop count before the header executes. In a
three-word body the penultimate word already sampled the count.
If a three-word CALL occupies that penultimate word, its continuation
instead samples during the return refill. The continuation also preserves
the compact drain state of a one-pass loop. For a CALL at the first word of
an admitted two-, three- or four-word body, the startup fetch can sample before
the CALL saves the suffix. The compact three-word suffix keeps its sample
pending for the second word.
An arithmetic or FOREVER CALL records an unbounded pass or the bounded
buffer still to drain. If the branch replaces an arithmetic condition fetch,
the prepared return performs that sample after the frozen clocks, using the
flags after the return slots. It restores the fetched suffix and any tag for
a CALL in Address on the sample clock.
A CALL occupying an admitted long-loop endpoint consumes the loop decision
and pushes the selected successor, including the loop header on a repeat.
When a CALL redirects before a pending long-loop sample reaches Fetch1,
the snapshot retains that event until the return refill. A delayed return
preserves it across both slots and applies it at the redirect.
An admitted `JUMP (LA)` removes the loop frame at the jump. A counted tail
that has already popped identifies the completed frame until an abort or the
original endpoint retires. With DB, the two slots execute after the stack effect.

Peripheral interrupt routing (PICR), SRU, DMA and peripheral timers remain consumer
state. TAP emulation controls and physical instruction-cache contents are not
implemented. Straight-line ISA and admitted classic transfers, loop reentry and
interrupts update PC, FADDR, DADDR and EMUCLK at owner boundaries.
