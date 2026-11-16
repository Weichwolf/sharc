# ADSP-21489 architectural storage

The public `Registers` definition stores physical architectural bits, independently
of the selected computation precision. It is the initial S1 storage model, not an
execution or reset implementation. Default initialization supplies zeroed inspection
storage; it does not claim the hardware reset state. The reference is the SHARC
Processor Programming Reference 2.4, chapters 2–6 and appendix A, identified in
`reference-manifest.json`.

| Public member | Architectural representation |
|---|---|
| `pe[0]`, `pe[1]` | PEx and PEy; each has two physical banks of sixteen raw 40-bit registers |
| `pe[].multiplier[0..1]` | Foreground/background 80-bit MRF/MRB or MSF/MSB, split into unsigned 32/32/16-bit limbs |
| `pe[].bit_fifo`, `bit_fifo_write_pointer` | 64-bit shifter FIFO and occupancy/write pointer, 0 through 64 |
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
No host C++ object layout is a file or generated-code ABI.

`State` adds cumulative instruction/base-cycle progress, instruction set, execution
state and a latched fault. `Snapshot` is an immutable owning copy of the current
state and yield request, tagged with the numerical mode. Restore replaces that
state, including clearing a later fault. It does not replace consumer memory or
bindings. The current preparation-only implementation has no pending instruction,
branch, mode-register write or code transfer; their hidden continuation must be
added to snapshots together with the corresponding execution implementation.
Interrupted/delayed execution snapshots are not qualified yet.

Peripheral interrupt routing (PICR), SRU, DMA and peripheral timers remain consumer
state. TAP emulation controls and physical instruction-cache contents are not
implemented. Delayed register visibility, instruction pipeline observations,
loop reentry, interrupt eligibility/shadows, timer evolution, reset values and all
execution semantics remain open. These are not covered by storage round trips.
