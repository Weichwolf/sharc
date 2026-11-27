# Independent Strict numerical reference

`test/support/reference.py` is authored independently for expectation generation.
It imports only Python's standard library. Production neither imports it nor
shares numeric implementations with it. It uses arbitrary-precision integers and
`fractions.Fraction`; there is no host float, LLVM evaluator, instruction decoder,
processor execution loop or manufacturer program asset. The reference origin is
the pinned SHARC Processor Programming Reference 2.4 in
`reference-manifest.json`, especially chapters 3 and 11, appendix A and appendix C.

S2 supplies independent compute expectations without production lowering. That
reference-generation milestone is complete at the scope listed here; numerical
qualification and installed-image execution coverage remain separate requirements.
Further reference cases accompany instruction coverage rather than a standalone
reference processor. See issue #2.

| Component | Current scope |
|---|---|
| Raw views | 40-bit register storage; fixed view in bits 39:8; 8-bit exponent and 31-bit fraction for extended float |
| Fixed ALU | ADD/SUB, carry/borrow variants, increment/decrement, negate/absolute, average, pass, logic, min/max/clip, signed/unsigned compare |
| Fixed effects | Raw result with extension cleared, old ASTAT/STKY preservation, AF clearing, carry, saturation/overflow, AS, AOS, CACC history; compare has no data-register write |
| Finite FP rounding | Exact rational nearest ties-to-even and toward-zero, 24-/32-bit significands, overflow to infinity or format-specific maximum finite |
| FP value operations | ADD, SUB, average before rounding, multiply; independent value and exception metadata |
| Qualified FP effects | ADD/SUB and absolute forms, average, negate/absolute/pass, RND, SCALB, FLOAT, MIN/MAX/CLIP/COPYSIGN, compare and floating multiply; complete scalar flags/sticky results within the stated domain |
| MANT/LOGB effects | Raw mantissa with hidden bit, unbiased fixed exponent, RND32 input projection, zero/infinity/saturation results and width-specific invalid sentinels |
| Computational input views | Strict RND32 clears eight extension bits before arithmetic; denormal inputs become signed zeros without a new numeric underflow event |
| Special values | Signed zeros and their sign flags, infinities, NaN formats and signs, unordered COMP, and the minimum-normal rounding boundary for single-function arithmetic |
| Conversion values | FLOAT always returns a 40-bit result, including with RND32 selected; explicit RND consumes the current computational view and returns 32-bit precision |
| FIX/TRUNC effects | Scaled inputs, nearest/floor/toward-zero distinctions, post-round integer range, pre-round AUS, saturation and width-specific invalid sentinels |
| Fixed multiplier | SSI/SUI/USI/UUI and fractional/rounded variants, full 80-bit multiply/MAC/MSUB, separate MR/register destinations and preserved old flags |
| Accumulator operations | Signed/unsigned integer/fraction saturation, explicit nearest rounding, clear, all three limb reads/writes; MR1 writes extend their sign into MR2 |
| Scalar shifter | Logical/arithmetic shifts, rotation, bit clear/set/toggle/test, field deposit/extract with sign extension and deposit OR, leading zero/one counts and normal/extended exponent extraction |
| Bit FIFO | 64-bit raw storage, seven-bit pointer read/write, append, extract and no-update query; defined scalar results and status, explicit refusal of undefined data/pointer results |
| Short float unpack | All 65,536 short encodings, including gradual underflow, signed zero and maximum finite magnitude; exact binary32 results independent of ALU modes |
| Short float pack | Exact and rounded short fields, signed zero, clipping and shifter flags |
| Reciprocal seeds | Every observed RECIPS/RSQRTS ROM bin, exponent reconstruction, signed zero and denormal inputs, infinity and width-specific invalid sentinels |
| Dual ALU | ADD/SUB from common old inputs, OR of the new ALU status bits, sticky union; fixed saturation and qualified 32-/40-bit floating results |
| Multifunction arithmetic | Fixed product/MAC/MSUB with ALU, floating product with ALU/conversions, multiply plus dual ALU; disjoint flag ownership and old 80-bit accumulator |
| SIMD numeric expectations | Authored paired PEx/PEy values with distinct carry/overflow, saturation, floating exceptions and old flag/sticky inputs; no cross-lane flag union |
| Numeric/status conditions | EQ/NE/GT/LT/GE/LE, arithmetic/shifter/BTF predicates and complements; old AF/AV/ALUSAT selection, per-lane compute, branch AND and post-modify OR |
| Conditional numeric effects | Qualified fixed/floating computations from old lane predicates; inactive/SISD lanes and comparison destinations retain all raw storage and old flags |
| Data register writeback | Raw 40-bit physical cells, low/high alternate-bank selection, producer write priority and independent PE destinations; proposals are already admitted and projected |
| Conditional data swaps | Raw40 Rx/Sy exchange proposals from old physical banks; PEx-controlled SISD halves and independently controlled SIMD halves, composed with ALU effects |
| Scale extremes | Signed 32-bit scale factors use exponent bounds before allocating a power of two; extreme shifts cannot request gigabytes of oracle storage |

The model requires an explicit `Mode` for floating operations. It is a mathematical
input, not a public Core/backend selector or a change to the Core's fixed lifetime
mode. It implements no FastFP32 approximation. In particular, truncating a Strict
RND32 operand's extension byte differs from rounding a FastFP32 operand.

ASTAT/STKY effects are instruction-specific. A numerical zero is not interchangeable
with a multiplier underflow exception; an ALU instruction may use AZ as an input to
its AUS rule, while another instruction leaves AUS unchanged. Consequently the
floating value records deliberately contain no speculative guest flag values.
Separate qualified effect records contain complete scalar results, including
ASTAT/STKY, or refuse the whole expectation if any required status is unqualified.
Fixed ABS preserves AOS even if it sets AV (11-14). Compare history uses the stated
X-greater-than-Y rule (11-7/8, A-20), rather than the contradictory parenthetical
expression printed beside it. Appendix A's RND32 selection agrees with the target
architecture; the introductory default-mode sentence on 3-37 is inconsistent.

## Reproducible generation

```sh
python3 test/support/generate_expectations.py --output build/local/strict-expectations.json
```

The generator checks 311 independently authored numeric/conversion/accumulator/shifter
anchors and 48,768 positions between neighboring normal encodings. The latter
span both precisions, both signs, all 254 normal exponents, six fraction patterns, four
quarter-ULP positions and both rounding directions. Expected neighbors are formed
from monotone raw bit patterns; they do not use the reference's normalization or
significand division. Additional checks require explicit refusal of unqualified
cases. Another 27,648 multiplier cases compare direct unbounded arithmetic with
schoolbook 16-bit partial products, signed-input corrections and five-limb carry/
borrow propagation. Their expected overflow uses guard-bit patterns while the
reference checks numeric bounds; their expected underflow uses a magnitude test.
Another 12,800 conversions compare against signed quotient/remainder arithmetic
over both precisions, signs, mode-bit values, instructions and saturation choices,
ten exponents, eight fraction patterns and five scales.
Another 133,397 shifter cases route individual bits and count textual prefixes;
15,449 FIFO cases use bit lists instead of register shifts, covering every legal
pointer/length combination for three data patterns, including no-update queries.
They also check all 128 pointer reads and all low-byte pointer-write patterns
with both input SF states and unrelated upper bits.
All 65,536 short encodings are unpacked and checked by decoding the resulting
binary32 rational and comparing it to the short format's value equation.
Another 35,088 full scalar FP cases use an authored wire-value codebook for exact
dyadic arithmetic, textual compare history and direct mantissa/exponent field
checks. They span both precisions and rounding settings, ALU/multiplier flag
ownership, unary/scaled operations, all normal exponents and eight fraction
patterns.
Another 13,072 multifunction cases compare the composed reference against
sign-bit overflow identities, independently rounded fixed averages, five-limb
products/accumulations and the exact floating wire-value codebook. Authored
multifunction records map old R0/R4/R8/R12 inputs to overlapping destination
registers, supplying expectations for later installed-image dependency checks.
Paired lane records give independent complete scalar effects for each PE.
Another 134,092 PACK field checks construct IEEE inputs with bit strings from all
65,536 short encodings and exercise exponent clipping and inexact rounding.
A further 896 seed special cases check both precisions, rounding settings,
signs, denormals and old status/sticky bits.
Another 928 predicate and aggregation checks use literal truth tables, independent
single-flag bit positions and all four true/false lane combinations in SISD/SIMD.
The output contains fixed and qualified FIX/TRUNC result/flag records and floating
value-only or complete qualified effect records, distinguished by record kind. Shifter
records include raw outputs, flags and FIFO effects where relevant. Raw 80-bit
accumulators and 64-bit FIFOs use fixed-width hexadecimal strings so a JSON reader
cannot silently round them.
`pack-value` records contain only `expected.short`, not a raw 40-bit result or
status. They cannot be consumed as complete instruction expectations. `seed`
records do contain complete qualified raw/status/sticky effects.
Output is deterministic and declares `isa_execution_coverage: false`.

Another 10,112 register-writeback cases cover both PEs, all sixteen logical
registers, the four low/high bank combinations and every producer-priority pair
in both proposal orders. Another 256 cases refuse an unresolved tie at the
winning priority. Authored packets show that a memory-source write can replace
an ALU data result while its new overflow/carry/zero flags remain, and that
different PE files do not compete. These records describe normalized raw writes;
they do not establish transfer widths, predicate admission or memory bus order.

Another 16,384 raw40 swap cases cover all Rx/Sy register pairs, four physical
bank selections, SISD/SIMD, all predicate pairs and unrelated MODE1 bits. Chapter
2-7 and table 4-40 on 4-98 define a single 40-bit exchange: in SISD PEx controls
both halves, even while PEy computation is inactive; in SIMD each predicate
controls its half independently. Both halves read old values, including distinct
extension bytes. An admitted half can duplicate a source without executing the
other half. Two authored ADD/swap packets use old AZ predicates and preserve new
ALU flags when the higher-priority swap overwrites the computed data result.
Other UREG transfer forms, their widths and admission rules remain outside this
swap helper, as do instruction encoding and native execution qualification.

These are checks of the expectation generator, not a new CTest layer and not a
replacement execution path for ISA tests. They do not call a fake Core or execute
an interpreter. Installed-image ISA tests will need complete independent results,
including all lane flags and ordered effects, before any corresponding coverage
row can be accepted. See `isa-coverage.md` for installed-image coverage by
instruction family and execution mode.

## Unqualified numerical cases

`Unqualified` prevents the generator from silently supplying a guessed expectation.
Currently explicit refusals include:

- SZ for a negative BTST position and sign extension of a zero-length field.
- FIFO operations with a prohibited length, overfull/invalid pointer, or insufficient
  capacity/data. The manual defines SV but leaves the resulting data/pointer
  undefined; no invented full-result expectation is emitted.
- Operations outside the currently implemented numerical functions.

Still required for complete numerical coverage: the shifter and FIFO details
above, OR-shift status, parallel memory ordering, other conditional transfer admission and mode/register-bank
composition beyond admitted raw writes. Qualified arithmetic and writeback
composition do not close those architectural gaps.
The explicit FIX/TRUNC distinction on 11-37 must not be replaced by a shared host
integer cast. No partial flag mask is used to claim a complete instruction result.

Installed-image evidence is recorded in `isa-coverage.md`. Conversions, packing
and seed operations are tracked in #24, and combined floating execution in #25.

## ADSP-21489 simulator observations

Analog Devices CrossCore Embedded Studio 2.12.1 for Windows, SHARC compiler
8.16.1.0, ran 436 focused and 1,344 deterministic scalar probes on the
ADSP-214xx Family Simulator with the ADSP-21489 target. The second set varied
signs and significands across normal exponents and combined selected zeros,
denormals, infinities, NaNs and arithmetic range boundaries. The corrected
reference matched the destinations, ASTATx and writable STKYx in both sets.
Captures covered MODE1 values `0`, `0x8000`, `0x10000` and `0x18000`
(both widths and both rounding directions).

An additional 20 paired SIMD probes matched destinations, ASTAT and arithmetic
sticky bits in both lanes. They checked distinct ADD and multiplier exceptions,
ABS of a negative NaN and negative zero, unordered and ordered COMP in the
same instruction, and a conditional ABS admitted only in PEx. To repeat them,
write the S registers through PX and seed ASTATy and ASTATx separately in SISD,
then enable PEy before the compute. Return to SISD before reading both raw
results through PX and collecting their status words.

| Boundary | Observed single-function behavior |
|---|---|
| RND32 input | Clear the extension byte before classification, including for explicit RND. PASS and COMP see positive infinity for `0x7f80000001` with RND32 set. |
| Invalid output | ALU uses a negative NaN with all fraction bits set. Multiply uses the first NaN operand's sign, or the product sign for zero times infinity. RND and RND32 outputs clear the extension byte. |
| Sign and comparison flags | Negative zero sets AN or MN. Invalid results clear both. ABS sets AS for negative zero and clears AS for a negative NaN. Unordered COMP sets AI/AIS, clears AZ/AN, and shifts a one into CACC. |
| Nearest rounding immediately below minimum normal | Above the midpoint the result is minimum normal without underflow. At the midpoint the multiplier returns minimum normal and sets MU/MUS. The ALU flushes the midpoint to signed zero and sets AZ/AUS. Below the midpoint both units flush to signed zero and signal underflow. |
| Exact ALU zero | ADD/SUB, absolute sum/difference and average set AZ without a new AUS on exact zero. A nonzero underflow sets AUS. The STKY write/read control retained an already set AUS. |

The exact-zero AUS observation differs from the description of AUS as a sticky
copy of AZ in Programming Reference 2.4 sections 11-24–28 and A-22. The
single-function implementation follows the ADSP-21489 observation for this
boundary. AC, AV and AS on compare follow section 11-29. The CACC result
matches a new bit of `!AZ && !AN` for ordered and unordered comparisons.

The directly observed inputs and expected effects used as anchors are in
`test/support/floating_expectations.py`. To reproduce a case, set MODE1 and
wait one instruction, write input words through PX2/PX1, seed the destination
through PX, write ASTATx and STKYx, and wait one instruction before executing
the single compute. Read the destination back through PX2/PX1, then store
ASTATx and STKYx to DM. The status observations used CACC `0xa5` and writable
sticky bits zero except for the AUS control. STKYx read-only stack bits from
the harness were `0x05400000`. PASS with raw `0x3f80001234` and COMP of
greater, equal and less inputs checked raw capture, precision and CACC order.

Build the authored C/assembly probe with `cc21k -proc ADSP-21489 -nwc -g`.
Run it with `CCES_runner --target "ADSP-214xx Family Simulator" --platform
"ADSP-214xx Simulator" --processor ADSP-21489 --core "0,probe.dxe"`. For
a C array of output words named `observations`, add a `--datafile` value of
`0,out,observations.dat,binary:le,observations,<word-count>,1` and start with
an empty output destination. Each tuple holds PX2, PX1, ASTATx and STKYx
as four little-endian words. Reconstruct raw40 as `PX2 << 8 | PX1 >> 24`.
FastFP32 rounds finite inputs first and preserves the class and sign of NaN
inputs, including extension-only payloads. Its expected effects come from the
independent approximation and the qualified operations.

Shifter observation targets include zero-source OR-shift, BTST position -1
and sign-extended empty fields.

## Floating conversion and seed observations

The first ADSP-21489 conversion batch contained 432 authored cases for eight
MODE1 configurations. All 48 cases for FLOAT and scaled FLOAT matched the
independent model after distinguishing exact zero from nonzero underflow.
Strict FLOAT retains 40-bit precision with RND32 set. Scaled FLOAT sets
AUS when a nonzero result underflows and preserves it when the input is zero.
Its signed scale includes the full fixed 32-bit range. FastFP32 rounds the
floating result to binary32 using the guest rounding direction.

SCALB matched the independent model in 144 cases across the first batch
and a 336-case follow-up. The scale is a signed 32-bit fixed operand. In
nearest rounding, an exact midpoint below minimum normal returns minimum
normal with AUS set and AZ clear. Scaling an input signed zero or a
denormal consumed as zero generates no AUS.

A separate sweep checked 1,024 RECIPS and RSQRTS lookups in both widths and
rounding modes. It covered the 128 RECIPS indices and both groups of 64
RSQRTS indices. For each index the fraction matched across lower and upper
bin endpoints and different input exponents. The result exponents matched
`-e - 1` for RECIPS and `-floor(e/2) - 1` for RSQRTS, where `e` is the
unbiased source exponent. Seed lowering and installed-image qualification
use these observed values.

The 244-case seed boundary follow-up also distinguishes two RECIPS zero
results. Finite inputs with unbiased exponent 126 or 127 set AUS on signed
zero output. Infinity inputs produce signed zero and preserve AUS.

In total 1,272 non-control conversions and special cases matched the
independent model, including the 288-case FIX/TRUNC and packing follow-up.
The separate 1,024-case ROM sweep supplies the expected words in
`test/support/seed_observations.py`. To reproduce the sweep, run one
RECIPS probe for each of the 128 mantissa indices at unbiased exponent zero,
and one RSQRTS probe for each of the 64 indices at unbiased exponents
minus one and zero. Repeat with each rounding width and direction, varying
the exponent within the same parity and using both endpoints of each bin.
Capture the raw destination and both status words through the procedure above.

An additional 72 paired SIMD probes checked FIX, scaled TRUNC, MANT, LOGB,
FPACK, FUNPACK, RECIPS and RSQRTS with independent inputs and flags in the
two lanes. The conditional FPACK probes preserved the masked lane's raw
destination and status. All outputs and flags matched the independent model.

## Primary observation tools

The CCES 2.12.1 Installation Guide (June 2024,
sections 6–7) lists classic SHARC simulation/debugging under Windows. Its optional
Linux functional tools support SHARC+ families and exclude earlier SHARCs such
as ADSP-21489. Results from a different SHARC+ profile cannot settle these gaps.
The guide is pinned in `reference-manifest.json` for tool availability only;
it supplies no numerical expected values. Core builds and tests remain offline
and independent of ADI software.

## Qualified floating effects

Chapters 11-23 through 11-57 give separate flag ownership and sticky predicates.
Every floating ALU operation sets AF, including conversions with fixed outputs.
Floating multiplication preserves all ALU bits and updates only MN/MV/MU/MI;
exact zero is not a new multiplier underflow event. Its overflow/underflow sticky
bits are MVS/MUS, while MOS remains unchanged.

ADD/SUB, their absolute forms and average set AUS for nonzero underflow.
Negate, absolute, pass, RND, selection operations and compare leave AUS
unchanged. Average clears AV and preserves AVS. SCALB and scaled FLOAT
set AUS on nonzero underflow and AVS on overflow. Unscaled FLOAT preserves
both sticky bits.

Ordered compare treats both zeros as equal and orders infinities normally. It
shifts CACC independently of the numeric result, preserves other non-ALU bits,
sets AF and writes no data register. MANT returns a fixed 1.31 field with AN
cleared and AS from the sign of a valid input. Infinity yields the hidden bit
and AV. A NaN yields the width-specific floating sentinel and AI with AS clear.
LOGB returns a fixed
unbiased exponent for a normal input; its zero exponent sets AZ without changing
AUS. Zero/denormal and infinity inputs use its separately specified saturated
fixed limits or floating infinities. None of these scalar oracle checks establish
SIMD lane composition, instruction ordering or native execution coverage.

## Parallel arithmetic and lane expectations

Chapter 3-33 specifies that both dual-ALU results use the corresponding scalar
semantics and that their new ALU flags are ORed. The model preserves old unrelated
status bits and unions sticky results. A nonzero sum can therefore coexist with
AZ from a zero difference, or AV and AUS can be set by different floating results.

Chapters 3-35/36 and 11-92/93 define the multifunction input groups and forms.
The numerical functions take all old operands explicitly, never feed a new
product into the ALU, and combine only the ALU's new flags with the multiplier's
new flags. ORing whole scalar ASTAT results would incorrectly reintroduce stale
bits from the other unit. Fixed multifunction supports the listed SSF/SSFR forms
and foreground accumulator destinations; floating multiplication remains a
separate operation rather than a floating MAC. Register-destination products
preserve the old accumulator, while MRF destinations return its updated 80 bits.

The installed floating packet tests use legal source groups with outputs that
replace old sources in both groups. The reference supplies result tuples and
destination maps. It computes separate ASTAT and STKY expectations for the two
lanes and preserves the complete inactive state. FastFP32 narrows each
computational floating operand to binary32 under guest rounding, then applies
the scalar operations at binary32 precision. The fixed operands of scaled
FLOAT and the signed scales of FLOAT and FIX retain their raw fixed widths.
Unqualified scalar results refuse the entire dual or multifunction expectation.

The CCES 2.12.1 ADSP-21489 simulator matched 160 authored scalar and 144 paired
SIMD dual-ALU and multifunction cases. The probes cover the ten forms in PRM
chapters 11-92/93 and 12-13–18, with extended and RND32 precision, nearest and
truncating rounding, and ALUSAT for scaled FIX. The SIMD probes also check both
directions of lane-local `IF EQ` admission. Capture raw destination words and
both lane status registers using the procedure above.

Another 96 paired cases share the product and ALU destination for each of the
eight binary multifunction forms. The simulator returned the ALU value with
both operations' flags and sticky effects in every admitted lane.

The writeback follow-up used different product, sum and difference results and
repeated the collisions with overflow, negative zero and an exact zero
difference in 48 scalar cases. A shared binary multifunction destination takes
the ALU result.
For the floating butterfly the sum wins over the product, and the product wins
over the difference. The losing operation still contributes status and sticky
effects. CCES assembler warning `ea2010` classifies a shared sum/difference
destination as ambiguous. The core rejects that case pending a specified
priority.

Another 12 paired simulator probes combine the butterfly with a type 5a raw
register move. They confirm that the move reads the old subtraction destination,
wins the shared product destination, and follows the separate lane predicates.

PRM 2.4, pages 2-6 and 2-7, gives DAG1 priority for competing register writes
and illustrates a type 1a packet with compute and two loads into one register.
The native fixtures follow its DM-first register priority. Memory arbitration
is separate. Pages 7-20 and 7-21 put a contended PM access in the next bus cycle.

Four floating SIMD probes with one shared memory block confirmed the old
operands, both rounding widths, the two lane status values and the DAG updates.
They returned the PM load in the common destination. A further 12 probes tested
fixed and floating compute in SISD and SIMD with the DM/PM address pairs
`0xc4000`/`0xc4002`, `0xc4000`/`0xe4000` and `0xe4000`/`0xc4000`. Load distinct
word pairs into the memory locations, then issue `F6 = F4 + F1` (or fixed ADD)
with `R4 = DM(I2,M3)` and `R4 = PM(I10,M11)` in the same instruction. Capture
both lanes after a NOP. The functional simulator selected PM in all 12 cases,
including the separate blocks. Register writeback on silicon remains unverified.

## Numeric conditions and predicated lanes

The formulae in table 4-37 on 4-93 include overbars that are absent from ordinary
PDF text extraction. They were inspected in the pinned PDF. Fixed relations use
AN XOR (AV AND NOT ALUSAT); floating LT excludes AZ, while GT excludes either AN
or AZ. GE and LE use the specified complementary predicates. Tests enumerate AF,
ALUSAT, AV, AN and AZ against separately authored literal tables, including raw
status combinations that need not result from the preceding numeric operation.
Those combinations do not establish how arithmetic generates NaN/negative-zero AN.

Table 4-39 supplies independent compute predicates per lane, sequencer branch
AND and post-modify admission OR. In SISD the implicit PEy computation is inactive
and only PEx controls the aggregate predicates. These are boolean expectations,
not execution of branches, memory operations or DAG modification. The predicated
numeric helper observes old AF/status, computes admitted lanes only and returns
final raw destination/status/sticky values. A comparison has no destination write;
an inactive lane preserves the complete extension byte and never invokes an
unqualified scalar computation. External flag-pin, loop-counter and bus-master
conditions, ELSE/control-flow effects, register banks, pipeline latency and
instruction-level exits remain outside this numerical helper's scope.

## FIX/TRUNC details

Chapter 11-37 explicitly gives FIX with MODE1.TRUNC set a rounding direction of
minus infinity, while the TRUNC instruction always rounds toward zero. The model
uses this instruction-specific rule despite the general toward-zero description
on 3-38. With MODE1.TRUNC clear, FIX rounds nearest ties-to-even. Scaling changes
the exponent before conversion; the model bounds extreme signed 32-bit scales
before allocating powers of two. Normal inputs use the computational precision
rule on 3-37; denormal inputs flush before conversion and do not set AUS.

The ADSP-21489 simulator sets AV/AVS when the rounded result exceeds signed
32-bit range or the input is infinite. Exact negative INT_MIN leaves AV clear,
and rounding a value just below positive 2**31 can set AV. The implementation
follows these observations at the range limits described on 11-38. Saturation clamps
the result. An unsaturated overflow or infinity sets AI/AIS and writes the
all-ones floating sentinel at the current result width. A NaN sets AI/AIS with
AV clear. Both invalid cases clear AN. AUS describes a nonzero pre-rounded
magnitude below one, including when nearest rounding produces one or negative
floor produces minus one. Unrelated ASTAT bits and old sticky bits survive.

## Shifter and bit FIFO details

Chapter 11-58 through 11-91 supplies the scalar effects. LSHIFT and ASHIFT take
the whole signed 32-bit fixed field of a register count or a signed eight-bit
immediate. Positive LSHIFT sets SV even when the source is zero. For positive
ASHIFT, the ADSP-21489 simulator sets SV when the mathematical result exceeds
the signed 32-bit range. OR-ASHIFT also sets SV for any left count of 32 or
more. This differs from the unconditional positive-count ASHIFT rule listed
on pages 11-61 and 11-62. OR forms derive SZ from the combined result and read
the old destination. The simulator matched these results and flags in 360
mixed shift probes and 528 arithmetic follow-ups. To repeat the boundary probes,
shift zero, `0x40000000`, `0x80000000` and `0xffffffff` by `-32`, `0`, `1`, `31`
and `32` in each form. For the OR forms repeat with zero and nonzero old
destinations. Capture the raw destination, ASTAT and STKY. Probe full register
counts with `-2147483648`, `256` and `2147483647`.

Large magnitudes are bounded before shifting so a signed 32-bit extreme cannot
allocate excessive oracle storage. Rotations wrap modulo 32. Bit modifications
outside the word preserve the data. Their SV uses the instruction-specific
greater-than-31 predicate. BTST writes no data register.

FDEP/FEXT use unsigned six-bit position/length inputs. Cross-boundary fields set
SV even when discarded bits are zero. Sign extension applies only if the field's
sign bit is in range; OR-deposit combines the completed field with the old
destination. Leading-bit counts return 0 through 32, with SV for 32 and SZ for
zero. EXP returns the signed negative count of redundant sign bits, or +1 for
extended extraction with old AV set; SS uses the old input sign XOR old AV in the
extended form. Shifter outputs clear the extension byte, preserve other status
bits and leave all sticky bits unchanged.

FIFO deposit ORs into the old raw tail, rather than replacing it. Extract aligns
the remaining bits to the top of the 64-bit FIFO; NU preserves the FIFO, pointer
and SF. Pointer reads preserve SF and clear SZ/SV/SS. Pointer writes use seven
bits, update SF at 32 and set SV above 64 without truncating to six bits. The
lattice starts with SF consistent with the old pointer, including for NU.
FUNPACK uses all four exponent bits as finite short-format exponents, supports
the short format's gradual underflow, and clears SZ/SV/SS even for signed zero.
It ignores unrelated upper input bits and clears the result's extension byte.
The exact format equations and independently authored anchors follow appendix C.

## Packing and seed effects

Strict FPACK consumes the IEEE32 field. FastFP32 applies its computational
input rounding first. The description on 11-84 and table C-2 specify clipping
to the largest short magnitude above biased exponent 135 and signed zero
below exponent 110. Within that range the shifter rounds ties up in magnitude,
independently of MODE1.TRUNC. A rounding carry past the largest short value
sets SV and clips, as does source exponent 255. The 16-bit result is
right-justified in the fixed field and all other destination bits are cleared.
Both FPACK and FUNPACK clear SZ and SS and preserve non-shifter flags and STKY.

Chapters 11-41 through 11-44 specify seed exceptions separately from ordinary
ROM lookup. Both operations map computational signed zero, including a flushed
denormal, to signed infinity with AV/AVS. RECIPS maps large finite exponents
to signed zero with AZ/AUS and infinities to signed zero with AZ. RSQRTS maps
positive infinity to positive zero with AZ and preserves AUS. Negative nonzero
RSQRTS inputs and either operation's NaNs produce an all-ones floating sentinel
with AI/AIS and AN clear. The result width controls the sentinel extension byte.
Ordinary seeds use the observed ROM fraction and instruction-specific exponent.

## Fixed multiplier details

The raw accumulator has 80 bits. Inputs use the upper 32 bits of each data
register; only signed/signed fractional multiplication removes a redundant sign
by shifting the product left once (3-14). MAC and MSUB keep all guard/low limbs
and wrap at 80 bits. The modeled MN sign is the full multiplier result's bit 79,
not the sign of a separately selected 32-bit register projection. Register
output selects MR0 for integer format and MR1 for fractional format and leaves
the old accumulator untouched.

Overflow follows appendix A-18 and the saturation limits of table 3-4: signed
integer/fraction limits are 32/64 bits, with 49/17 sign-check bits in the 80-bit
storage; unsigned limits use 48/16 guard bits. Chapter 11's 33/32 fractional
counts conflict with both the appendix and the stated format/saturation layout.
The reference uses the latter consistent definitions and records this discrepancy
rather than silently testing the contradictory counts.

A requested fractional R operation rounds to nearest ties-to-even at bit 32 and
clears the discarded part of the computed result (3-16, 11-54). Without R,
selecting MR1 simply discards MR0, including for a negative result; it is not a
signed host conversion toward zero. A register destination never writes the
rounded computed value back into MR. SAT uses all six limits in table 3-4.
Explicit integer RND modifiers are rejected because the encoding only provides
SF and UF (table 3-6).

Fixed multiplier instructions update MN/MV/MU/MI and preserve ALU flags, including
AF. Fixed overflow can set MOS, whereas fixed underflow does not set MUS under
the instruction-specific tables 3-5 and 11-50 through 11-56. This differs from
the generic underflow wording in appendix A-19. SAT, clear and accumulator limb
transfers preserve sticky bits; SAT clears MV, and clear/limb transfers clear all
four multiplier status bits. These remain oracle expectations to be checked by
installed-image ISA programs when native lowering exists.
