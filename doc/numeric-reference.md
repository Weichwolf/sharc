# Independent Strict numerical reference (S2 in progress)

`test/support/reference.py` is authored independently for expectation generation.
It imports only Python's standard library. Production neither imports it nor
shares numeric implementations with it. It uses arbitrary-precision integers and
`fractions.Fraction`; there is no host float, LLVM evaluator, instruction decoder,
processor execution loop or manufacturer program asset. The reference origin is
the pinned SHARC Processor Programming Reference 2.4 in
`reference-manifest.json`, especially chapters 3 and 11, appendix A and appendix C.

This is an initial S2 increment, not completion of S2 or of Strict execution.

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

The generator checks 52 independently authored numeric/conversion anchors and
48,768 positions between neighboring normal encodings. The latter span both
precisions, both signs, all 254 normal exponents, six fraction patterns, four
quarter-ULP positions and both rounding directions. Expected neighbors are formed
from monotone raw bit patterns; they do not use the reference's normalization or
significand division. Additional checks require explicit refusal of unqualified
cases. The output contains fixed result/flag records and floating **value-only**
records, with the distinction encoded in each record's kind. It is deterministic
and declares `isa_execution_coverage: false`.

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
- Operations outside the currently implemented numerical functions.

Still required: instruction-specific floating flags/sticky effects (including NaN
and signed-zero flags), FIX versus TRUNC and scaled conversions, remaining FP
operations and seed ROM expectations, 80-bit fixed multiplier/accumulator behavior,
shifter/packing expectations, and parallel/SIMD result and status combinations.
The explicit FIX/TRUNC distinction on 11-37 must not be replaced by a shared host
integer cast. No partial flag mask is used to claim a complete instruction result.

Normal-range rounding validation does not prove the unqualified underflow boundary,
seed tables, instruction semantics, compiled native results or a supported-host
performance result. Every execution row in `isa-coverage.md` remains unimplemented.
S3 is still gated on the announced Lead translation-rules update in lead-a1-src#2.
