# ISA and implementation coverage

This is a target inventory, not an implemented decoder table. Every row is
**unimplemented**. Replace coarse rows with documented encoding/semantic
entries as work starts; record independent fixtures and supported-host
evidence before marking an instruction or mode implemented.

| Area | Required evidence | Status |
|---|---|---|
| Classic 48-bit ISA | Every documented instruction class, condition, legal operand and parallel form | Unimplemented |
| VISA 16/32/48 | All compact encodings, fetch boundaries, PC units and mode transitions | Unimplemented |
| Arithmetic and SIMD | PEx/PEy, fixed point, 32-/40-bit float, rounding, modes, flags and exceptions | Unimplemented |
| Parallel operations | Old operands, memory aliases, register exchanges and ordered partial effects | Unimplemented |
| Address generation and memory | Both DAGs, modulo/reverse addressing, PM/DM and physical view aliases | Unimplemented |
| Sequencer and control | Delayed flow, DO/loop endpoints, LR, stacks, reset and interrupts | Unimplemented |
| Chip-profile boundaries | ADSP-21489 memory/vector parameters and explicit I/O ownership | Unimplemented |
| JIT coherence and hosts | Self-modifying aliases, natural loops, precise exits, x86-64 and ARM64 | Unimplemented |

Overall status: documentation/reference seed only. No native execution,
ISA completeness, host validation, real-time or benchmark claim.
