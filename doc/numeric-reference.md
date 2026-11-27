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
| Qualified FP effects | ADD/SUB and absolute forms, average, negate/absolute/pass, RND, SCALB, FLOAT, MIN/MAX/CLIP/COPYSIGN, ordered compare and floating multiply; complete scalar flags/sticky results within the stated domain |
| MANT/LOGB effects | Raw mantissa with hidden bit, unbiased fixed exponent, instruction-specific infinity/zero/saturation results, AF/AS/AV/AI and sticky effects; explicit RND32/sentinel gaps |
| Computational input views | Strict RND32 clears eight extension bits before arithmetic; denormal inputs become signed zeros without a new numeric underflow event |
| Special values | Signed-zero value rules, infinities, 40-bit all-ones NaN results and invalid-operation metadata |
| Conversion values | FLOAT always returns a 40-bit result, including with RND32 selected; explicit RND consumes extension bits and returns 32-bit precision |
| FIX/TRUNC effects | Finite integer outputs and saturated overflow/infinity, scaled inputs, input precision, nearest/floor/toward-zero distinctions, exponent-based AV and pre-rounded AUS; sentinel-result flags explicitly refused |
| Fixed multiplier | SSI/SUI/USI/UUI and fractional/rounded variants, full 80-bit multiply/MAC/MSUB, separate MR/register destinations and preserved old flags |
| Accumulator operations | Signed/unsigned integer/fraction saturation, explicit nearest rounding, clear, all three limb reads/writes; MR1 writes extend their sign into MR2 |
| Scalar shifter | Logical/arithmetic shifts, rotation, bit clear/set/toggle/test, field deposit/extract with sign extension and deposit OR, leading zero/one counts and normal/extended exponent extraction |
| Bit FIFO | 64-bit raw storage, seven-bit pointer read/write, append, extract and no-update query; defined scalar results and status, explicit refusal of undefined data/pointer results |
| Short float unpack | All 65,536 short encodings, including gradual underflow, signed zero and maximum finite magnitude; exact binary32 results independent of ALU modes |
| Short float pack values | Exact finite fields, source-exponent clipping and signed-zero fields; 16-bit values only, inexact rounding and full register/status effects remain unqualified |
| Seed special cases | RECIPS/RSQRTS zeros, flushed denormals, infinities and qualified 40-bit invalid results; RECIPS large-exponent zero; ordinary ROM entries explicitly refused |
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

The generator checks 279 independently authored numeric/conversion/accumulator/shifter
anchors and 48,768 positions between neighboring normal encodings. The latter
span both precisions, both signs, all 254 normal exponents, six fraction patterns, four
quarter-ULP positions and both rounding directions. Expected neighbors are formed
from monotone raw bit patterns; they do not use the reference's normalization or
significand division. Additional checks require explicit refusal of unqualified
cases. Another 27,648 multiplier cases compare direct unbounded arithmetic with
schoolbook 16-bit partial products, signed-input corrections and five-limb carry/
borrow propagation. Their expected overflow uses guard-bit patterns while the
reference checks numeric bounds; their expected underflow uses a magnitude test.
Another 10,972 conversions compare against signed quotient/remainder arithmetic
over both precisions, signs, mode-bit values, instructions and saturation choices,
ten exponents, eight fraction patterns and five scales. The same lattice requires
1,828 explicit refusals for unqualified unsaturated overflow results.
Another 130,817 shifter cases route individual bits and count textual prefixes;
15,449 FIFO cases use bit lists instead of register shifts, covering every legal
pointer/length combination for three data patterns, including no-update queries.
They also check all 128 pointer reads and all low-byte pointer-write patterns
with both input SF states and unrelated upper bits.
All 65,536 short encodings are unpacked and checked by decoding the resulting
binary32 rational and comparing it to the short format's value equation.
Another 29,956 full scalar FP cases use an authored wire-value codebook for exact
dyadic arithmetic, textual compare history and direct mantissa/exponent field
checks. They span both precisions and rounding settings, ALU/multiplier flag
ownership, unary/scaled operations, all normal exponents and eight fraction
patterns. The same checks require 5,132 explicit refusals of unqualified negative
zero or MANT extension cases; those are not accepted results.
Another 13,072 multifunction cases compare the composed reference against
sign-bit overflow identities, independently rounded fixed averages, five-limb
products/accumulations and the exact floating wire-value codebook. Authored
multifunction records map old R0/R4/R8/R12 inputs to overlapping destination
registers, supplying expectations for later installed-image dependency checks.
Paired lane records give independent complete scalar effects for each PE.
Another 133,832 PACK field checks construct IEEE inputs with bit strings from all
65,536 short encodings and exercise exponent clipping; 260 inexact inputs require
explicit refusal. A further 896 seed special cases check both precisions, both
rounding settings where qualified, signs, denormals and old status/sticky bits.
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
row can be accepted. Current installed-image coverage is limited to the fixed
ALU and direct-branch subsets listed in `isa-coverage.md`.

## Unqualified numerical cases

`Unqualified` prevents the generator from silently supplying a guessed expectation.
Currently explicit refusals include:

- Nearest rounding of a nonzero value in the one-ULP interval immediately below
  the smallest normal. The manual says to flush a post-rounded denormal, but the
  current analysis has not established the rounding boundary there. Ordinary IEEE
  gradual underflow is not assumed. Toward-zero underflow and values farther below
  the boundary already have exact signed-zero expectations.
- The extension byte of a canonical NaN in a RND32 result, and RND32 classification
  when the only NaN payload bits are in that byte. Forty-bit NaN values are modeled;
  these 32-bit details require further evidence.
- Complete FIX/TRUNC flags when the result is the all-ones floating sentinel
  (NaN input, or unsaturated infinity/overflow). The sentinel value is documented;
  its AN effect is not established by the fixed-point result wording. Saturated
  infinities and finite integer results already have complete scalar expectations.
- Floating AN/MN for negative-zero or NaN results. Instructions whose AN is
  explicitly cleared, such as absolute sum/difference, already have complete
  expectations where the other fields are qualified.
- Unordered floating compare, LOGB's NaN sentinel, and MANT with RND32 selected
  and nonzero extension input. MANT's exact-output wording and the general input
  precision rule need stronger evidence to resolve that input boundary.
- Inexact FPACK rounding, its complete upper result storage and status effects;
  ordinary RECIPS/RSQRTS ROM entries and RECIPS AN for a negative NaN input.
- SZ for a negative BTST position and sign extension of a zero-length field.
- FIFO operations with a prohibited length, overfull/invalid pointer, or insufficient
  capacity/data. The manual defines SV but leaves the resulting data/pointer
  undefined; no invented full-result expectation is emitted.
- Operations outside the currently implemented numerical functions.

Still required for complete numerical coverage: the floating, conversion, packing and seed details
above, OR-shift status, and
parallel memory ordering, other conditional transfer admission and mode/register-bank
composition beyond admitted raw writes. Qualified arithmetic and writeback
composition do not close those architectural gaps.
The explicit FIX/TRUNC distinction on 11-37 must not be replaced by a shared host
integer cast. No partial flag mask is used to claim a complete instruction result.

Normal-range rounding validation does not prove the unqualified underflow boundary,
seed tables, instruction semantics, compiled native results or a supported-host
performance result. The separately qualified initial native subset is recorded in
`isa-coverage.md`; it does not complete this reference.
The remaining single-function arithmetic cases are tracked in #11.
Conversions, packing, and seed operations are tracked in #24.

## Unverified against a primary source

The following probes remain open. The independent installed-image matrix omits
cases where the reference declines to generate the full outcome. The single-
function lowering currently makes these choices for the open cases:

- For RND32, classify a NaN using the raw 40-bit operand before masking its
  extension and write the all-ones 40-bit sentinel. Appendix A defines RND32
  precision, while sections 3-37–38 describe the general boundary and NaN result.
- Compute AN and MN as ordered negativity, which clears them for negative zero
  and NaN. For unordered COMP, set AI/AIS, clear AZ and AN, and shift a zero
  into CACC. Sections 11-23–33 and 11-57 describe these flags, including
  AI on NaN input. The CACC parenthesis in COMP contradicts its greater-than
  wording, and the unordered comparison flags need direct evidence.
- Round on the subnormal grid at the minimum-normal threshold, then flush
  any remaining subnormal to signed zero. The post-rounded rule in sections
  11-23–27 and Appendix A leaves this transition unqualified.
- Explicit RND consumes the raw extension in Strict. FastFP32 reduces the
  operand first, using the guest rounding direction. RND's flags then describe
  the operation on that reduced operand, including when the operand rounded
  to infinity.

| Boundary awaiting observation | Authored probe |
|---|---|
| NaN result extension and extension-only payload under RND32 | PASS raw40 `0x7fc0000000` and `0x7f80000001`, RND32 clear/set |
| AN/MN for NaN/negative zero and conversion sentinels | PASS negative zero; zero times -1; NaN ADD/MUL; unsaturated FIX infinity; LOGB NaN |
| Unordered COMP | Compare ordinary and extension-only NaNs against finite values in both source positions, with nonzero CACC and both precisions |
| Rounding just below minimum normal | Multiply `0x0080000000` by `0x3f7fffffff` in 40-bit mode and `0x3f7fffff00` in 32-bit mode |
| FPACK inexact result, upper storage and flags | Pack half-way inputs into a nonzero destination; vary TRUNC, RND32 and SSE |
| MANT with nonzero extension under RND32 | MANT raw40 `0x3f80000001`, RND32 clear/set |
| Ordinary reciprocal and reciprocal-square-root seed entries | Capture every input mantissa bin and both square-root exponent parities; retain exact raw40 outputs |
| Remaining shifter status boundaries | Zero-source OR-shift into nonzero destination; BTST position -1; sign-extended empty fields |

## Primary observation tools

The remaining exceptional and seed details require primary observations or
further primary specification. The CCES 2.12.1 Installation Guide (June 2024,
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

ADD/SUB, their absolute forms, average and SCALB set AUS whenever AZ is set,
including an exact zero. Negate, absolute, pass, RND, selection operations and
compare leave AUS unchanged. Average clears AV and preserves AVS. Scaled FLOAT
updates AUS/AVS, including when its explicit scale is zero; unscaled FLOAT
preserves them. The value records still retain signed-zero encodings even when
the corresponding full status record refuses an unqualified AN/MN.

Ordered compare treats both zeros as equal and orders infinities normally. It
shifts CACC independently of the numeric result, preserves other non-ALU bits,
sets AF and writes no data register. MANT returns a fixed 1.31 field with AN
cleared, AS from the input sign, and an all-ones **fixed field** for infinity/NaN.
This differs from the all-40-bit-ones floating NaN sentinel. LOGB returns a fixed
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

The aliasing fixtures use legal source groups with outputs that replace old
sources in both groups. Their result tuples and destination maps are expectation
data for future ISA fixtures; no register-file executor or instruction decoder
was added to the reference. Numeric lane records keep ASTATx/STKYx separate from
ASTATy/STKYy, including different old sticky bits. They assume two admitted lanes;
SISD inactivity, predicates, banks, parallel memory transfers, duplicated output
priority and architectural interrupts still require installed-image qualification.
Unqualified scalar results refuse the entire dual or multifunction expectation.

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

AV uses the stated biased-exponent-plus-scale threshold of 157, independently
of signed integer range overflow. Consequently exact negative INT_MIN sets AV,
while rounding the greatest representable value below positive 2**31 can overflow
without setting AV. Saturation clamps the rounded integer but leaves this AV rule
intact. AUS describes a nonzero pre-rounded magnitude below one, including when
nearest rounding produces one or negative floor produces minus one. Unrelated
ASTAT bits and old sticky bits survive, AF is set and AC/AS/AI are cleared for the
qualified cases. No sentinel flag expectation is fabricated to fill the gap.

## Shifter and bit FIFO details

Chapter 11-58 through 11-91 supplies the scalar effects. Shift amounts and field
positions/lengths are normalized mathematical inputs here; this reference does
not decode the register/immediate encoding or establish the register amount's
significant-bit selection. Positive logical/arithmetic shifts set SV even if no
nonzero bits are lost. Large magnitudes are bounded before shifting so a signed
32-bit extreme cannot allocate excessive oracle storage. Rotations wrap modulo
32. Bit modifications outside the word preserve the data; their SV uses the
instruction-specific greater-than-31 predicate. BTST writes no data register.

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

## Packing fields and seed special cases

FPACK's source is the IEEE32 field, independently of the raw extension byte.
The description on 11-84 and table C-2 specify clipping to the largest short magnitude above source
biased exponent 135 and a signed-zero field below exponent 110. Within that range
the value model emits only exactly representable short fields; it refuses any
discarded nonzero bit instead of guessing the rounding rule. Source exponent 255
uses the table's clipping branch for the value field; no floating exception or
complete shifter-status expectation is inferred from it. The exact-input checks
do not assume a rule for upper destination bits or prove inexact packing.

Chapters 11-41 through 11-44 specify seed exceptions separately from ordinary
ROM lookup. Both operations map computational signed zero, including a flushed
denormal, to signed infinity with AV/AVS. RECIPS maps large finite exponents and
infinities to signed zero and updates AZ/AUS, with AN from its input. RSQRTS maps
positive infinity to positive zero with AZ but preserves AUS; negative nonzero
inputs and NaNs return the qualified all-ones 40-bit NaN with AI/AIS and AN clear.
Its AN rule explicitly names negative-zero input, so those seed cases can be
qualified without guessing the general ALU AN rule for a negative-zero result.
RND32 canonical NaNs and every ordinary ROM seed remain refused. No mathematical
reciprocal or square-root approximation substitutes for a hardware seed.

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
