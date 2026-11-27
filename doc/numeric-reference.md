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
Another 135,002 shifter cases route individual bits and count textual prefixes;
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

- FIFO operations with a prohibited length, overfull/invalid pointer, or insufficient
  capacity/data. The manual defines SV but leaves the resulting data/pointer
  undefined; no invented full-result expectation is emitted.
- Operations outside the currently implemented numerical functions.

Still required for complete numerical coverage are the undefined FIFO cases
above, parallel memory ordering, other conditional transfer admission and mode/register-bank
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

In 200 authored fixed dual ADD/SUB cases the CCES 2.12.1 ADSP-21489 simulator
matched both output words, ASTAT and STKY in both lanes. The cases use equal
and opposite operands, zero, carry and signed overflow in SISD and SIMD with
and without ALUSAT. They vary all SIMD EQ predicate pairs and let both
destinations overlap the inputs or select distinct registers. Inactive cases
establish the readable ASTAT and STKY baselines, including flag-pin and stack
bits supplied by the simulator.

Another 40 cases cover eight classic packet shapes with fixed dual compute.
The simulator matched both output words, ASTAT, STKY, the two address indices
and the observed PM/DM words. Type 1a uses both memory buses and performs the
compute on each available lane. Types 3a and 4a apply the lane predicates to
compute and to the indexed load or immediate-offset store. Type 5a transfers
the old difference register into the sum destination. The memory load or
register move wins the shared sum destination while the ALU flags remain.
Type 7a updates the index once when either lane is selected. Type 9a applies
the SIMD predicates to its parallel compute, and `ELSE` inverts them. The
installed tests also check the actual branch destination and retirement cost
with the original conditions.

Another 432 probes paired tied destinations with controls using distinct ALU
destinations. They cover fixed and floating dual ADD/SUB and butterflies,
including a separate multiplier destination or all three results sharing one
register. The probes vary three destination registers, both processing modes,
EQ predicates and ordinary or overflow operands. CCES 2.12.1 emitted the tied
compute fields and warned about an ambiguous destination. In all 180 active
lane results the tied destination matched the sum and differed from the
difference. The separate destination and all checked ASTAT and STKY values
matched their expected control effects. The lowering keeps the sum and
retains both ALU flag contributions along with any multiplier effects.
Native fixtures check the adopted behavior against independent numeric results.
Silicon confirmation remains open.

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
allocate excessive oracle storage. Rotations wrap modulo 32. The simulator's
264 single-bit probes match unsigned register positions. Positions above 31,
including raw words with the sign bit set, set SV. Bit modifiers keep the fixed
source word outside that range and clear the destination extension byte. BTST
sets SZ for an outside position and writes no data register. The assembler
limits immediate BTST to 0–31. It accepts off-scale byte positions for BCLR,
BSET and BTGL. Nine additional probes covered `-1`, `32` and `255` for those
modifiers.

In 684 field probes, FDEP/FEXT consumed the unsigned six-bit position and
length from the lowest 12 bits of the fixed count or the type 6a DATA/DATAEX
fields. SV is set when the position plus length exceeds 32, including an
off-scale empty field. Sign extension requires a nonempty field whose sign
bit is in range. OR-deposit combines the completed field with the old
destination before computing SZ.

Another 208 probes checked leading-bit counts and both exponent forms. LEFTZ
and LEFTO return 0 through 32, with SV for 32 and SZ for zero. EXP returns the
signed negative count of redundant sign bits, or +1 for extended extraction
with old AV set. SS uses the old input sign XOR old AV in the extended form.
The destination extension byte is zero. Unrelated status and sticky bits are
preserved.

FIFO deposit preserves the occupied prefix and writes the incoming field at
the old write pointer. It clears all other bits beyond the old pointer, even
for a zero-length deposit. The PRM's OR pseudocode on page 11-87 omits that
clearing step. Extract shifts the complete raw FIFO left. NU preserves storage,
the pointer and SF. Pointer writes preserve raw FIFO bits. They use seven bits,
update SF at 32 and set SV above 64. Reads preserve SF and clear SZ/SV/SS.

Register counts use the low six bits of the fixed word. In 48 immediate
boundary probes, CCES encoded DATAEX in the assembled instruction. The
functional simulator treated immediate lengths 64, 65 and 96 as out of range
but treated `0x801` as length 1. The core uses the full documented 12-bit
immediate and refuses lengths above 32. Silicon behavior for such operands
remains unverified.

The simulator matched 72 append/extract sequences, 22 pointer cases and 113
of 117 defined operand-domain cases. The four differences were in the raw
bits left behind by a 31-bit extract. To expose stored bits outside the logical
prefix, fill the FIFO with `0x812345679abcdef0`, then set BFFWRP to 7. Apply
BITDEP with length zero, or BITEXT (NU) with length zero. Set BFFWRP to 64 and
extract two 32-bit words. The deposit leaves `0x8000000000000000`. The query
preserves the full original bits. No expected raw result is supplied for
prohibited lengths or attempts outside the available capacity or data.

An isolated sweep of all extraction lengths from 0 through 32 confirmed the
returned word, new pointer and occupied FIFO bits in 330 cases. For lengths
of 8 through 31, the simulator can retain shifted copies of the original
low bytes in the unoccupied tail. From `0x812345679abcdef0`, a 31-bit extract
left `0xcd5e6f78006f7800` in the simulator. The reference follows the documented
64-bit left shift, which leaves `0xcd5e6f7800000000`. Silicon behavior of
the unoccupied bits remains unverified.

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

## Absolute transfers and immediate stores

In 48 authored type-14a normal-word probes, the CCES 2.12.1 ADSP-21489
functional simulator loaded and stored through both PM and DM with explicit
R4 or S4. SISD touched one lane. SIMD accessed the addressed word with the
explicit register and the next word with the complementary register. Setting
BDCST1 or BDCST9 had no effect on these absolute transfers.

Another 32 type-16a normal-word probes used I1/M3 or I9/M11. A store from
the last word of a two-word buffer wrote the same immediate to both lane
addresses in SIMD, including the adjacent word past the buffer. CBUFEN
affected the single post-modification of I and left the lane addresses
unchanged. The broadcast bits had no effect on the writes. Programming
Reference 2.4 gives the extended-word placement in section 9-60 and
long-word replication in section 2-13.

Eight additional immediate-store probes used short and long SRAM addresses
for both buses and both PE modes. The short view used `0x32ab` from immediate
`0x765432ab`, adding a second store at short address +2 in SIMD. The long
view contained two copies of the full immediate in either mode. The probes
read the resulting words through their normal-word aliases in the
[ADSP-21489 memory map](https://www.analog.com/media/en/technical-documentation/data-sheets/ADSP-21483_21486_21487_21488_21489.pdf).

Eight separate aligned forced-long probes loaded or stored R5 through a
normal-word address using the `(LW)` qualifier. The addressed word used R5
and the next word used R4 in SISD and SIMD.

In a further 32 forced-long probes at an odd normal-word address, the CCES
simulator rounded the address down to the preceding even word. It used the
explicit register for that even word and the same-PE neighbor for the odd
word. Both buses and explicit R/S registers matched in SISD and SIMD. With
MODE2.U64MAE set it also raised STKYx.U64MA and IRPTL.IICDI with IICDI masked.
The same two words transferred with monitoring clear. Programming Reference
2.4, section 7-25 and Figure 7-9, describe an unaligned access across two
successive rows. The data placement and timing of the odd-address form remain
unverified on silicon. The lowered form uses the specified address and the
next word in that order, with the observed monitor-dependent latches.

The 32 indexed probes used `R5 = DM(2,I1)(LW)` or the PM counterpart. For
normal-word and long-word addresses, both registers in the pair reached the
other PE when broadcast was enabled, including in SISD. Without broadcast
the other PE was unchanged. The simulator also interpreted `(LW)` at a short
address as a long-word row access. The core follows section 6-8, which applies
the qualifier to normal-word accesses, and retains the short view width and
lane stride.

## Predicated single-bus transfers

The CCES 2.12.1 ADSP-21489 functional simulator ran 184 authored normal-word
packets with `IF EQ`, across PM and DM, SISD and SIMD, independent PEx/PEy
flags, and selected broadcast reads. All 144 type-3 and type-4 results matched
independent lane selection for both compute and transfer. A post-modified
index advanced once if either lane was selected. Pre-modified accesses used
the offset and preserved the index.

In type 6, the simulator selected the immediate rotation result independently
in each PE. With exactly one lane selected in SIMD, it still loaded or stored
both lanes, including both broadcast destinations. This accounts for 12 of
40 type-6 probes. The index still advanced once if either lane was selected.
Programming Reference 2.4, section 9-26, specifies independent selection for
the entire type-6 instruction. The discrepancy remains unverified on silicon.

Another 32 type-3 probes used an explicit S4 read or store. Across both buses
in SISD and SIMD, the PEx condition controlled the explicit S4 transfer and
the PEy condition controlled the complementary R4 transfer. The compute
result still used each PE's condition and data file. Sixteen type-3 and
type-4 circular-address probes used I = B + 1, L = 2 and an offset of +2.
Pre-indexing accessed B + 3 and left I unchanged with CBUFEN clear or set.
Post-modification accessed the old I, then left I at B + 3 with CBUFEN clear
or B + 1 with CBUFEN set. The observations agreed on PM and DM.

Another 24 SISD broadcast probes separated the two stored EQ flags. Type 3
required PEx EQ for the read and both EQ flags for the complementary load.
Type 4 admitted a complementary load and PEy compute with PEy EQ alone.
Type 6 admitted PEy compute and broadcast to both data files when either
EQ flag was true. The core follows the documented broadcast examples in
sections 9-16, 9-21 and 9-27: the admitted PEx read loads both files in SISD
and compute runs on PEx. The differing simulator cases are unverified on silicon.

Eighty more type-3a probes used a natural long-word address or forced `(LW)`
at an aligned normal-word address, with independently initialized register
pairs on both PEs. A load updated the explicit pair only when PEx was
selected. A store used the explicit pair when either predicate was true,
including a PEy-only selection. A post-modifying packet advanced I once
when either predicate was true, including a load with only PEy selected and
no register writeback. This agrees with the post-modify predicate rule on
page 4-103 and the explicit long-word data pair on pages 6-8 to 6-9.

## DAG registers as memory operands

The CCES 2.12.1 ADSP-21489 functional simulator matched 164 normal-word and 32
short-word observations for classic types 3a, 14a and 15a. The probes use both
buses, both PE modes, all four DAG register classes for normal words, and
opposite DAGs for indexed forms. A load takes the explicit word when either
predicate is true in SIMD, including when only PEy is selected. A normal- or
short-word store sources both selected words from the same DAG register, with
the usual one- or two-address stride. Read broadcast does not change the DAG
destination. Type 3a post-modifies its address index once when either SIMD
predicate is true. Programming Reference 2.4, section 4-106, specifies the
OR predicate for a memory-to-DAG move.

Another 112 probes used long-word addresses or `(LW)` at aligned normal-word
addresses. They initialized the neighboring register and upper memory word
independently. Every admitted load updated both neighboring DAG registers and
every admitted store wrote both distinct words. With only PEy selected, the
type-3a transfer still used the whole pair, with the explicit register in the
low word. Sections 6-7 through 6-10 and Table 6-1 specify the neighboring DAG
register, including when the explicit register is odd. The installed-image
fixtures exercise both orders and check the complete register and memory state.

An 81-case DM timing probe bracketed the load and a subsequent `MODIFY` with
zero, one or two intervening NOPs. The NOP control took three, four and five
clocks. A normal-word load followed by use of the explicit I register took
five clocks at each spacing. An unrelated I register matched the control.
A natural long-word load wrote both registers and interlocked only the
explicit register. Forced `(LW)` interlocked both registers, including when
the explicitly named register was odd. The forced form still interlocked
both registers with a false predicate, while preserving their old values.
A separate 81-case PM timing probe, using a dependent DM read in place of
`MODIFY`, showed the same relative dependency pattern and an extra clock on
the unrelated-index control for a PM load. A further 54 DM probes confirmed
that a false type-3a predicate reserves the explicitly named DAG register
even without `(LW)`. Short-word loads use MODE1.SSE to select zero or sign
extension and interlock the explicit register. At a short address with
`(LW)` the simulator loaded both neighboring registers and interlocked both,
the same width discrepancy as the indexed data-register probes. Section 6-8
limits the override to normal-word addressing. These are functional-simulator
observations. Silicon timing remains unverified.

## User status and bus exchange registers as memory operands

The CCES 2.12.1 ADSP-21489 functional simulator ran 282 authored DM probes
with USTAT1–3, PX1, PX2 and combined PX. The absolute, immediate-indexed
and register-post-modified forms used independently initialized register pairs
and memory words. The address index matched the expected old index or single
post-modification in every case. In SIMD normal-word transfers, the PEx
predicate selects the named USTAT or PX half at the explicit address and
the PEy predicate selects its peer at the next address. The short-word
post-modified cases used the same predicates with a stride of two.

Natural and forced long-word indexed transfers used the named USTAT or PX
half in the low word and its peer in the high word. In Type 3a, a load
required PEx and a store used the whole pair when either PE was selected.
All tested absolute long-word transfers of a single USTAT or PX half behaved
differently: stores duplicated the named register and loads put the high
word in only that register. This held in SISD and SIMD at natural and forced
long-word addresses. Programming Reference 2.4, section 2-13, describes a
long-word USTAT store with both peer registers and a SISD load of only the
named register. The absolute-versus-indexed data placement still requires
independent confirmation.

Combined PX moved its low and high halves as one long word for both kinds
of long-word addressing. In eight SIMD short-address post-modify probes,
the low, middle and high 16-bit cells at successive short addresses mapped
to PX bits 31:16, 47:32 and 63:48. Loads cleared bits 15:0 and required PEx.
Either lane admitted a store of all three cells. The fourth cell retained
its original value, and either active lane triggered one post-modification.
The short-address probes used DM and broadcast disabled.
The normal-word broadcast probes for USTAT1 and PX1 followed independent
lane selection in SIMD and required both stored EQ flags to update the peer
in SISD, the same simulator behavior seen for the Type 3a data-register
broadcast probes. The lowering uses the documented SISD broadcast rule and
the documented two-word store for the paired absolute form.

A further 240 combined-PX probes used both buses, absolute, immediate-indexed
and register pre-/post-modified forms, and distinct data at the two- and
three-column aliases. The 144 unqualified short-address samples matched
three successive short cells at even and odd addresses. The 88 normal-address
samples used the three-column layout regardless of the tested SYSCTL.IMDW
setting. In SIMD without broadcast, only PEx admitted a read. Broadcast
permitted either selected lane to load the same 48 bits. Either lane admitted
a store, and a post-modified index advanced once. In eight further probes,
the simulator used four short cells with `(LW)`, the discrepancy also seen
with short-address data and DAG register moves. The core applies the override
only to normal-word accesses, following Programming Reference 2.4, section
6-8. The functional simulator results establish the observed data placement.
Silicon timing remains open.

A further 48 indexed long-word broadcast probes covered USTAT1, PX1 and
combined PX at natural and forced addresses. With only PEx selected, each
named register received the low word and its peer received the high word.
With the implicit transfer selected, the order was reversed, including
immediate-indexed and SISD broadcast reads. The complete pair came from the
same address. The installed memory fixtures check that order and the original
single address update.

In a separate 194-case register probe, 110 cases matched the expectations
for USTAT and PX-half moves, combined-PX moves to and from the raw40 data
files, and immediate writes. SIMD paired writes use the corresponding
source and condition for each lane. Combined PX supplies bits 63:24 to a
data register, and writes from an explicit data register clear bits 23:0.
An immediate write to combined PX duplicates the immediate in both halves.
The other 84 cases moved between combined PX and USTAT, a PX half or a
DAG register. The assembled words use the ordinary type-5a combined PX
operand in both directions. CCES placed the source in the high half and
zeroed the low half for `PX = USTAT1` and `PX = I12`. The simulator also
read PX2 for the reverse moves and performed an implicit USTAT write if
only PEy was selected. The core follows Programming Reference 2.4, pages
2-16–2-17 and 4-101–4-103, for combined PX with USTAT or DAG registers.
A write to PX places the named USTAT in PX1 and its complement in PX2,
or duplicates the DAG word. The reverse transfer copies PX1 to the named
destination only. PEx gates the whole transfer. These choices are covered
by installed-image tests. Silicon confirmation remains open.

A further 320 CCES 2.12.1 probes isolated moves between combined PX and PX1
or PX2, and a combined-PX self-move. Each opcode was observed in the
assembled type-5a instruction with and without parallel ADD. The probes
used four distinct bit patterns, SISD and SIMD, and all old EQ predicate
pairs. In transfers to a half, the old PX2 word is the source for each
selected destination. For example, `PX2 = PX` in SIMD copies old PX2 to
PX1 when PEy is selected, including when only PEy is selected. A move to
combined PX puts the explicitly named half in PX2 and clears PX1 when PEx
is selected. `PX = PX` preserves all 64 bits and retires the parallel compute
in its selected lanes. The independent checks matched both PX halves, the
ALU destinations, and ASTAT and STKY in every case. The lowering reuses the
same raw register path for the half transfers and preserves PX through the
self-move. These projections have simulator and installed-image evidence.

## USTAT bit operations

The CCES 2.12.1 ADSP-21489 functional simulator matched 160 authored classic
type-18a probes covering every USTAT selector, SET, CLR, TGL, TST and XOR,
mask values including zero and all ones, and SISD and SIMD. The probes checked
all four USTATs, ASTATx/y and an immediate TF-dependent register transfer.
TST sets BTF when the mask is a subset of the old word. XOR sets BTF when
the whole word matches. Both clear BTF on a failed test. A mutation keeps
the old flags. In SIMD, the explicit USTAT supplies the PEx operation and
its paired register supplies PEy, including with USTAT2 or USTAT4 named.

An initial 54 EMUCLK probes and a 192-case branch matrix included NOP
controls, zero or one intervening NOP and both processing modes. An adjacent
TF or NOT TF jump gained one cycle after TST or XOR in both the taken and
fall-through cases. EQ and NE matched their controls, as did unconditional
jumps and jumps following SET, CLR or TGL. The lowered pipeline carries that
BTF dependency through the owner boundary and uses it when accounting for
a native loop. Silicon timing remains open.

## Register MODIFY and DAG interlocks

In 64 authored type-7a probes, the CCES 2.12.1 ADSP-21489 functional simulator
used source I15/B15/L15 and M14. The program varied both lane predicates, SISD
and SIMD, CBUFEN, wrap direction and the choice of I15 or I13 as the
destination. Either active lane triggered one shared update. Only selected
lanes produced an ALU result. Both destinations raised CB15S in STKYx and
CB15I in IRPTL on source-buffer wrap with CBUFEN clear or set. The simulator
also returned bit 18 when reading STKYy in those 40 cases. Programming
Reference 2.4, Table A-7, defines that bit for STKYx only. The core preserves
the corresponding STKYy bit. Silicon behavior for the STKYy read remains
unverified.

An EMUCLK bracket covered one operation followed by a dependent memory load
with zero, one or two intervening NOPs. The NOP control took three, four and
five clocks. An explicit I2 load took five clocks at each spacing. Plain
and enhanced register `MODIFY` matched the NOP timing, and the memory load
used the resulting I2 at each spacing. A separate bracket around an explicit
I5, M1, B5 or L5 load followed by enhanced `MODIFY` showed two, one and zero
extra clocks. This qualifies simulator timing for register `MODIFY`; type 19
and silicon timing remain separate.

## Control-register read operands

The CCES 2.12.1 ADSP-21489 functional simulator ran 208 authored register
probes. They combined 13 system registers with independent lane predicates,
R/S destinations and parallel ADD, or with DAG and combined-PX destinations.
ASTAT and STKY selected the named register for the explicit result and its
counterpart for the implicit result. A parallel ADD changed ASTAT after the
transfer had sampled the old status. Only the explicit predicate enabled the
combined-PX transfer. The simulator placed the source in PX2 and cleared PX1.
The core repeats the named source in both halves, following the documented
system-register exception and its I0 and PC examples in Programming Reference
2.4, pp. 2-16–2-17. Silicon confirmation remains open for these sources.

Another 240 authored store probes covered absolute DM, post-modifying DM/PM
and immediate-indexed PM. Normal and short stores followed the selected lanes.
Indexed wide stores paired ASTAT and STKY, while absolute wide stores repeated
the named register in the simulator. For unpaired sources the simulator
repeated the word in absolute wide stores and used the next UREG selector for
the high word of indexed wide stores. The core uses the documented unpaired
replication rule, including LCNTR (p. 2-13), and uses the complementary status
pair in both addressing forms (pp. 4-95–4-96 and 10-31–10-32). Silicon
confirmation of the differing wide stores remains open.

## Control-register bit tests

In 576 authored `BIT TST` and `BIT XOR` probes, the CCES 2.12.1 ADSP-21489
functional simulator updated PEx BTF in SISD and both BTF values in SIMD.
Each ASTAT or STKY test used the named word in PEx and its counterpart in PEy.
MODE1, MODE2, MMASK, FLAGS, IRPTL, IMASK, IMASKP and LIRPTL produced the same
BTF result on both lanes. Probes varied the old BTF values and covered an
adjacent transfer conditional on TF. The flags and transfer results matched
the per-lane operands read before the bit test. All other ASTAT bits retained
their prior values.

## Loop preload writes

In 134 authored probes, the CCES 2.12.1 ADSP-21489 functional simulator
checked register moves, immediate writes and memory loads into LCNTR.
The explicit lane predicate controlled the register write. Parallel ADD
preserved the old source for the move. A post-modifying load updated its
index once when either predicate was true and kept LCNTR unchanged when
only PEy was selected. Absolute long-word loads took the upper word.
Indexed long-word loads took the lower word. Both results held for native
long views and for the normal-word `(LW)` override.

The simulator took PX2 for `LCNTR = PX`. Programming Reference 2.4,
p. 2-16, explicitly specifies PX1, which the core uses. Silicon confirmation
remains open. In the timing probes a type 13a `DO ... UNTIL LCE` immediately
used the new value after an immediate write, register transfer, absolute DM
load or post-modifying DM load. The clock bracket around the write and three
five-word loop turns measured 18 clocks in every case. Adding one or two
NOPs before the DO added one or two clocks, consistent with Table A-10's
zero read and effect latency for LCNTR.

## Explicit arithmetic status writes

The CCES 2.12.1 ADSP-21489 functional simulator ran 272 register, immediate,
bit-operation and predicate probes, 108 focused interlock probes and 64
memory load probes. An explicit ASTAT write is visible in raw readback at
the next instruction. An adjacent predicate still uses the preceding flags
when decode does not stall. With one intervening NOP, a conditional consumer
uses the new flags and adds one clock. With two NOPs it uses the new flags
without an extra clock. A masked writer produced no ASTAT interlock. In a
parallel ADD packet, an adjacent non-branch predicate saw the compute flags
before the explicit write, and an adjacent conditional branch added two
clocks and used the new flags. The Core retains the pending predicate sample
across bounds and snapshots.

Another 48 probes checked unconditional ADD and ADDC with zero, one or two
NOPs after an explicit write. The carry operand was available to ADDC at
the next instruction, and neither operation added clocks.

An absolute long load used the upper word and wrote the named ASTAT only.
An indexed long load placed the low word in the named ASTAT and the high word
in its peer. Both cases covered native long views and `(LW)` in normal views.
The simulator accepted attempts to write SF and returned asymmetric reserved
bits for ASTATx and ASTATy. The Core retains the FIFO-derived SF and masks the
reserved bits consistently on both lanes, following Programming Reference
2.4, Table A-6. The differing write masks and conditional-interlock timing
await silicon confirmation.

## Explicit sticky status writes

The CCES 2.12.1 ADSP-21489 functional simulator ran 88 register, immediate
and bit-operation probes and another 168 memory and timing probes. Register
transfers and bit operations kept the explicit and complementary lanes
independent. A parallel ADD left its ASTAT and data result intact, and the
explicit STKY transfer replaced the computed sticky bits of its physical
destination. Direct reads and `BIT TST` saw a completed STKY write or
arithmetic overflow at the next instruction without an extra clock.

Absolute long loads selected the high word and the named register. Indexed
long loads selected both words when the explicit predicate was true. A
PEy-only post-modifying long load left both STKY registers unchanged and
updated the shared index. This held for native long views and the `(LW)`
override in normal views.

Programming Reference 2.4, Table A-7, assigns the arithmetic sticky bits to
both lanes and the DAG, memory and stack fields to STKYx. The core preserves
the read-only stack fields and masks reserved bits. CCES exposed stack bits
in both registers and accepted writes to reserved fields and to PEy fields
that the manual assigns to PEx. For the defined writable fields the core uses the observed bit
replacement behavior, including zero to clear and one to set. The differing
mask and software set of arithmetic sticky bits await silicon confirmation.

## Emulation clock reads

In 18 authored probes the CCES 2.12.1 ADSP-21489 functional simulator sampled
EMUCLK at the execute cycle. Consecutive register reads differed by one.
An indexed store sampled after two address interlocks when its I register
was loaded by the preceding instruction. A register transfer paired with
ADD took the same sample as a plain transfer. In a three-turn, one-word DO,
the last body read and first successor read measured offsets five and nine
from the preceding clock read. Native fixtures also seed the counter near a
low-word rollover and verify EMUCLK2 and replay across a stopped boundary.

The core uses its emulation clock backing plus retired cycles from the
current native call and the sampled instruction's stalls. It keeps each
clock observation at an exact execution boundary so a batched loop or
straight-line grant cannot omit cycles from the sample. Programming
Reference 2.4, pp. 8-14 and A-31, assigns writes to emulation space.

## Sequencer register reads

The CCES 2.12.1 ADSP-21489 functional simulator ran 113 authored probes.
PC was the executing word address in linear code, either delay slot, the
target of a jump, a PM store and a register transfer paired with ADD.
PCSTKP reflected the pushed CALL frame in both delay slots and the popped
frame after RTS. The simulator returned the low 24 bits for PCSTK and
`0x00ffffff` when empty. The core uses the complete 26-bit entry and
`0x03ffffff` when empty, following Programming Reference 2.4, pp. 4-13
and A-10. Silicon readback of the tag bits remains unverified.

In the final turn of a three-turn counted DO, the first word of a five-word
body could read the remaining count of one and the loop address word.
The second word saw an empty loop stack. Both sampled positions in a
seven-word body saw the active frame. LADDR and CURLCNTR return
`0xffffffff` when empty (pp. 4-48–4-49). Native tests also check unpaired
stores and a retained stack word from an owner-provided stack.

FADDR and DADDR remain open for guest reads. In the simulator, ordinary
register transfers saw PC+4 and PC+2 in linear code. Another 36 probes
compared absolute and post-modifying DM and PM stores in linear code and
both slots of a taken delayed branch. Linear DM stores matched the register
transfers. Linear PM stores saw PC+3 and PC+1. In the delay slots, DM stores
saw Fetch1 at target+1 and target+2, while PM stores saw target and target+1.
Decode redirected in the second slot for DM and remained at the slot's
PC+1 for PM. A preceding load of the addressing DAG register did not change
these samples.

Eight further probes executed the same store site three times in a software
or counted loop. DM offsets remained +4/+2. PM offsets were +3/+1 on the
first turn and +4/+2 on both later turns. The PM loop took one extra clock
across all three turns. These observations are consistent with the initial
PM instruction-fetch cache miss in Programming Reference 2.4, p. 4-80.

### Internal instruction-fetch conflicts

Programming Reference 2.4, pp. 4-80–4-83, and ADSP-214xx Hardware
Reference 1.1, pp. 4-50–4-51, describe a two-way cache with 16 sets
indexed by the low four bits of the instruction fetch address. An internal
instruction fetch consults the cache when it conflicts with a PM data
access. A miss delays the fetch by one cycle and can install the fetched
instruction. External instruction fetches use the same cache without a PM
data conflict.

In 38 additional functional simulator visits, each called routine measured
`EMUCLK` before and five instructions after `PM(I9,M9)=FADDR`.
A hit stored PC+4 and elapsed six cycles. A miss stored PC+3 and elapsed
seven cycles. Sites A, B and C were 16 words apart. D was 17 words after C.
Each case started with a flush and the cache enabled and unfrozen. Mode
changes and flushes were separated from the next PM access by NOPs.

| Case | Visits and control changes | Results in visit order |
| --- | --- | --- |
| Replacement | A B A C A B D A | miss miss hit miss hit miss miss hit |
| Disable | A, disable, A B, enable, A B B | miss miss miss hit miss hit |
| Freeze | A, freeze, A B B, thaw, B B | miss hit miss miss miss hit |
| Recency during freeze | A B, freeze, A, thaw, C A | miss miss hit miss hit |
| Invalidate | A A, flush, A A | miss hit miss hit |
| Invalidate while frozen | A, freeze and flush, A A, thaw, A A | miss miss miss miss hit |
| Invalidate while disabled | A, disable and flush, A, enable, A A | miss miss miss hit |

Disabling kept the existing entries and bypassed lookup and insertion.
Freezing suppressed insertion on a miss, while a hit still updated recency.
`FLUSH CACHE` invalidated the entries in either mode. These are simulator
observations for internal instruction fetches.

Two PM stores shared a fetch address in eight visits. The linear store
was at P and the taken delayed jump targeted P+3, with the second store
in its first slot. With a cold cache, whichever store ran first saw
FADDR=P+3 and took an extra cycle. The other store then saw FADDR=P+4
with no extra cycle. Cache identity therefore follows the conflicting
instruction fetch across paths and is independent of the store's PC or
data address.

In 48 additional samples, a register transfer read FADDR or DADDR one to
four instructions before an immediate or delayed jump. The samples kept
their linear PC+4 and PC+2 offsets for unconditional, taken conditional
and untaken conditional jumps.

In eight predicated PM load visits and four predicated PM store visits,
the first visit stalled even when the predicate was false. The false
load preserved the destination register. The false store preserved RAM
and the post-modified index. A following true visit to either site hit.
Address-stage cache arbitration and execute-stage memory effects must
be accounted for separately.

In four clock-store visits, three consecutive instructions read EMUCLK,
stored EMUCLK to PM or DM, and read EMUCLK again. The store's clock was
always one greater than the first read. The last read was two greater on
the cold PM visit and one greater on the warm PM and both DM visits. The
miss stretched the execute interval to the successor. An installed-image
comparison at `517befa` gave intervals of one cycle for both buses and
retired the three-instruction sequence in three base cycles for both.

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
