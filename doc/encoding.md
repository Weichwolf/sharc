# ADSP-21489 preparation inventory

The decoder uses the SHARC Processor Programming Reference 2.4, chapters 9–12
(the pinned local reference in `reference-manifest.json`). This inventory covers
preparation classification. It does not certify instruction execution.

Code spans contain numerical 16-bit parcels in most-significant-first fetch
order. ISA consumes three parcels at one PC. VISA consumes one, two or three
parcels and advances the PC by that count. Serialization to physical consumer
storage and boot containers are separate from this representation. A span is a
complete declared fetch range; an instruction may not borrow an undeclared
parcel from another span. Entries and interrupt vectors must name decoded
instruction boundaries. Variants may reuse addresses; spans within a variant
may not overlap, even if their instruction sets differ.

| Instruction family | Classic ISA | VISA | Preparation classification |
|---|---|---|---|
| Parallel DM/PM transfers, optional computation | 1a | 1a/b | Two DAGs, two transfer directions; optional compute marker |
| Conditional/unconditional computation | 2a | 2a/b/c | General, unconditional compact, short compute |
| DAG register-modified transfer | 3a | 3a/b/c | General, no-compute compact, short DM transfer |
| Immediate-modified transfer | 4a | 4a/b | General or compact without compute |
| Register transfer/exchange | 5a | 5a/b | General or compact without compute |
| Immediate shift and optional transfer | 6a | 6a | Dedicated immediate compute field; distinct standalone and memory-transfer prefixes |
| DAG modify and optional computation | 7a | 7a/b | General or compact without compute |
| Absolute/PC-relative jump or call | 8a | 8a | Distinct direct/relative prefixes |
| Indirect/short PC-relative branch and computation | 9a | 9a/b | General or compact without compute |
| Branch, computation and DM transfer | 10a | — | ISA-only; its 1100 prefix collides with VISA short compute |
| RTS/RTI and optional computation | 11a | 11a/c | Return subtype and short return marker |
| Counter-based hardware loop | 12a | 12a | Immediate count or UREG count source |
| Condition-based hardware loop | 13a | 13a | Termination condition and relative endpoint |
| Absolute memory transfer | 14a | 14a | PM/DM, direction, long-word override and UREG |
| Immediate offset plus DAG index transfer | 15a | 15a/b | Full or compact immediate |
| Immediate data to indexed memory | 16a | 16a/b | Full or compact immediate |
| Immediate data to register | 17a | 17a/b | Full or compact immediate |
| System register bit operation | 18a | 18a | Operation and SREG fields |
| Index modify/bit reverse | 19a | 19a | Source/destination index and immediate modifier |
| Stack/cache control | 20a | 20a | Individual loop/status/PC stack and cache bits |
| NOP | 21a | 21a/c | Full or short NOP |
| IDLE/EMUIDLE | 22a | 22a/c | Full or short idle subtype |
| Reserved types | 23/24 | 23/24 | No admitted instruction prefix |
| Compiler frame operations | 25a | 25a/c | CJUMP/RFRAME prefix and short RFRAME marker |

The standalone type-6 prefix is `0x02` in bits 47:40. Programming Reference 2.4,
section 10-11, repeats the with-transfer diagram for its without-transfer form.
The code-compatible ADSP-21160 Instruction Set Reference 2.1, section 2-26/27,
distinguishes these prefixes and the common immediate compute field; it is pinned
in `reference-manifest.json`. ADSP-214xx code compatibility with ADSP-2116x is
stated in Programming Reference 2.4, section 1-10. The previous decoder omitted
this standalone prefix; malformed-span and reserved-prefix fixtures incorrectly
called it reserved. The malformed span now uses `0x03`, and explicit
standalone type-6 fixtures cover recognition and installed execution.

For the classic bit FIFO opcodes in Table 12-11, CCES assembly of authored
programs places the register length of BITDEP and BITEXT in the Y field. BITDEP
reads the data to append from X. `BFFWRP = Rn` also reads the Y field, and
`Rn = BFFWRP` writes the result field. The standalone immediate forms take the
length or pointer from DATA and, for a 12-bit length, DATAEX. The installed
FIFO execution coverage is tracked separately in `isa-coverage.md`.

The 1001 VISA prefix uses additional discriminator bits to separate full-width
immediate stores, compact immediate stores/offset transfers and short transfers.
The optional-compute marker is tested only in the families that define it;
it must not shorten a classic ISA instruction. A compact 2b instruction instead
carries a relocated 23-bit compute field. A short 2c instruction uses a separate
four-bit operation code. These all enter one normalized compute representation.

| Compute family | Decoded fields and static refusal checks |
|---|---|
| Single ALU | Fixed/float opcode, output and input registers; reject opcode holes |
| Single multiplier | Opcode, inputs/output, signedness/format/destination/rounding bits; reject undocumented opcodes and rounded integer forms |
| Register shifter | Opcode, inputs/output; reject holes without confusing its opcode space with ALU |
| Immediate shifter | Dedicated opcode, input/output, immediate and DATAEX; reject reserved bit 22 and opcode holes |
| Parallel add/subtract | Two outputs and two old inputs; distinguish fixed/float from single-ALU opcodes |
| Multiplier plus ALU | Two outputs and four restricted input groups; reject opcode holes |
| Multiplier plus add/subtract | Three outputs and four restricted input groups; fixed/float prefix |
| Accumulator transfer | Direction, data register and six legal MR limb selectors |
| Short compute | Normalize sixteen compact operations to their general ALU/multiplier operations |

The private decoded representation retains raw fields and ISA/VISA identity.
No compute operation is executed by the decoder, and no test imports that
representation. Public preparation tests observe explicit acceptance-for-decoding
followed by unsupported-lowering refusal, or an earlier invalid-encoding refusal.
This proves classification/refusal boundaries, not the numerical or operand
semantics of successfully installed images.

Still open for execution qualification: transfer operand restrictions, DAG source
conflicts, universal-register access rules, reserved control bits, branch/loop
programming restrictions, result collisions, ordered parallel effects, mode
latencies and all numeric results/flags. Until their native paths exist, all such
instructions explicitly refuse lowering. Semantic coverage remains in
`isa-coverage.md`; unresolved or unimplemented execution cases prevent the full
goal from being complete.
