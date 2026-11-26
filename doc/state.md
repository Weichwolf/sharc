# ADSP-21489 architectural storage

The public `Registers` definition stores physical architectural bits, independently
of the selected computation precision. It is the S1 programmer-visible storage model, not an
execution or reset implementation. Default initialization supplies zeroed inspection
storage; it does not claim the hardware reset state. The reference is the SHARC
Processor Programming Reference 2.4, chapters 2–6 and appendix A, identified in
`reference-manifest.json`.

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
contributing DAG register. Pending interrupts, mode-register writes and
declared code transfers still need execution and snapshot qualification.

For admitted DO forms, snapshots retain the PC-stack ownership of each
hardware loop frame and its fetch decision. They also retain the pending
arithmetic refill and short loop words still buffered after an early stack pop.
A pending counted tail and an arithmetic refill have distinct storage, so
the outer counter may terminate while the inner arithmetic tail is draining.
For a short child, the snapshot also preserves how far Fetch1 has advanced
past its endpoint while buffered child words remain.
A counted DO loads LCNTR once. The current count then evolves on the loop
counter stack while LCNTR continues to hold the setup value.
A qualified CALL saves a suspended terminating tail with its PC-stack entry.
For a delayed `RTS (LR)`, the tail becomes active after the two return slots.
When a prepared DO tail uses IF NOT LCE, the counted tail also carries
the clocks elapsed after its terminal fetch. This observation survives a
stack pop, a return refill and a cross-Core snapshot.
Admitted CALLs in counted two-, three- and four-word loops keep the
buffered instruction count with the PC-stack entry. The callee can use its
own loop buffer. The saved buffer is restored at the prepared return
address. On a repeating endpoint CALL in two- and four-word bodies the
return refill samples the loop count before the header executes. In a
three-word body the penultimate word already sampled the count.
If a three-word CALL occupies that penultimate word, its continuation
instead samples during the return refill. The continuation also preserves
the compact drain state of a one-pass loop.
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
implemented. Instruction pipeline observations beyond the admitted classic
transfers and loop reentry, interrupt eligibility/shadows, timer evolution and
reset values remain open. Straight-line ISA and admitted direct and indirect
transfers update PC, FADDR, DADDR and EMUCLK at owner boundaries.
