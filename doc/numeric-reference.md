# Independent Strict numerical reference (S2 in progress)

`test/support/reference.py` is authored independently for expectation generation.
It imports only Python's standard library. Production neither imports it nor
shares numeric implementations with it. It uses arbitrary-precision integers and
`fractions.Fraction`; there is no host float, LLVM evaluator, instruction decoder,
processor execution loop or manufacturer program asset. The reference origin is
the pinned SHARC Processor Programming Reference 2.4 in
`reference-manifest.json`, especially chapters 3 and 11, appendix A and appendix C.

S2 remains in progress; this does not establish Strict execution coverage.

| Component | Current scope |
|---|---|
| Raw views | 40-bit register storage; fixed view in bits 39:8; 8-bit exponent and 31-bit fraction for extended float |
| Fixed ALU | ADD/SUB, carry/borrow variants, increment/decrement, negate/absolute, average, pass, logic, min/max/clip, signed/unsigned compare |
| Fixed effects | Raw result with extension cleared, old ASTAT/STKY preservation, AF clearing, carry, saturation/overflow, AS, AOS, CACC history; compare has no data-register write |
| Finite FP rounding | Exact rational nearest ties-to-even and toward-zero, 24-/32-bit significands, overflow to infinity or format-specific maximum finite |
| FP value operations | ADD, SUB, average before rounding, multiply; independent value and exception metadata, without instruction-specific ASTAT/STKY expectations |
| Computational input views | Strict RND32 clears eight extension bits before arithmetic; denormal inputs become signed zeros without a new numeric underflow event |
| Special values | Signed-zero value rules, infinities, 40-bit all-ones NaN results and invalid-operation metadata |
| Conversion values | FLOAT always returns a 40-bit result, including with RND32 selected; explicit RND consumes extension bits and returns 32-bit precision |
| FIX/TRUNC effects | Finite integer outputs and saturated overflow/infinity, scaled inputs, input precision, nearest/floor/toward-zero distinctions, exponent-based AV and pre-rounded AUS; sentinel-result flags explicitly refused |
| Fixed multiplier | SSI/SUI/USI/UUI and fractional/rounded variants, full 80-bit multiply/MAC/MSUB, separate MR/register destinations and preserved old flags |
| Accumulator operations | Signed/unsigned integer/fraction saturation, explicit nearest rounding, clear, all three limb reads/writes; MR1 writes extend their sign into MR2 |
| Scale extremes | Signed 32-bit scale factors use exponent bounds before allocating a power of two; extreme shifts cannot request gigabytes of oracle storage |

The model requires an explicit `Mode` for floating operations. It is a mathematical
input, not a public Core/backend selector or a change to the Core's fixed lifetime
mode. It implements no FastFP32 approximation. In particular, truncating a Strict
RND32 operand's extension byte differs from rounding a FastFP32 operand.

ASTAT/STKY effects are instruction-specific. A numerical zero is not interchangeable
with a multiplier underflow exception; an ALU instruction may use AZ as an input to
its AUS rule, while another instruction leaves AUS unchanged. Consequently the
floating value records deliberately contain no speculative guest flag values.
Fixed ABS preserves AOS even if it sets AV (11-14). Compare history uses the stated
X-greater-than-Y rule (11-7/8, A-20), rather than the contradictory parenthetical
expression printed beside it. Appendix A's RND32 selection agrees with the target
architecture; the introductory default-mode sentence on 3-37 is inconsistent.

## Reproducible generation

```sh
python3 test/support/generate_expectations.py --output build/local/strict-expectations.json
```

The generator checks 118 independently authored numeric/conversion/accumulator
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
The output contains fixed and qualified FIX/TRUNC result/flag records and floating
**value-only** records, with the distinction encoded in each record's kind. Raw 80-bit accumulator values
use fixed-width hexadecimal strings so a JSON reader cannot silently round them.
Output is deterministic and declares `isa_execution_coverage: false`.

These are checks of the expectation generator, not a new CTest layer and not a
replacement execution path for ISA tests. They do not call a fake Core or execute
an interpreter. Installed-image ISA tests will need complete independent results,
including all lane flags and ordered effects, before any corresponding coverage
row can be accepted. Existing Core tests still check preparation/refusal only.

## Unqualified cases and remaining S2 work

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
- Operations outside the currently implemented numerical functions.

Still required: instruction-specific floating flags/sticky effects (including NaN
and signed-zero flags), sentinel conversion flags, remaining FP
operations and seed ROM expectations, shifter/packing expectations, and
parallel/SIMD result and status combinations.
The explicit FIX/TRUNC distinction on 11-37 must not be replaced by a shared host
integer cast. No partial flag mask is used to claim a complete instruction result.

Normal-range rounding validation does not prove the unqualified underflow boundary,
seed tables, instruction semantics, compiled native results or a supported-host
performance result. Every execution row in `isa-coverage.md` remains unimplemented.
S2 still precedes native lowering; coordination is tracked in issue #2.

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
