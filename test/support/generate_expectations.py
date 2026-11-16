# SPDX-License-Identifier: GPL-3.0-only
"""Validate the independent arithmetic oracle and emit authored numeric cases.

This is expectation generation, not an ISA execution test or a fourth CTest layer.
Installed-image ISA tests must eventually consume and check complete expectations.
"""

import argparse
from dataclasses import asdict
from fractions import Fraction
import json
from pathlib import Path

import condition_expectations
import conversion_expectations
import floating_expectations
import multiplier_expectations
import packing_expectations
import parallel_expectations
import shifter_expectations
import writeback_expectations

from reference import (
    FloatingResult, IntegerResult, Mode, Unqualified, fixed, fixed_from_float,
    float_from_fixed, float_round, floating, quantize,
)


# Explicit constants and expected flags are authored independently of the model's
# classification/packing code. No production files or manufacturer programs enter.
FIXED = [
    ("add signed overflow", "add", 0x7FFFFFFF00, 0x0000000100, {},
     IntegerResult(0x8000000000, 0x06, 0x04)),
    ("add saturation", "add", 0x7FFFFFFF00, 0x0000000100, {"saturate": True},
     IntegerResult(0x7FFFFFFF00, 0x02, 0x04)),
    ("add carry and zero", "add", 0xFFFFFFFF00, 0x0000000100, {},
     IntegerResult(0x0000000000, 0x09, 0x00)),
    ("subtract borrow", "sub", 0x0000000000, 0x0000000100, {},
     IntegerResult(0xFFFFFFFF00, 0x04, 0x00)),
    ("subtract no borrow", "sub", 0x0000000100, 0x0000000100, {},
     IntegerResult(0x0000000000, 0x09, 0x00)),
    ("subtract signed overflow", "sub", 0x8000000000, 0x0000000100, {},
     IntegerResult(0x7FFFFFFF00, 0x0A, 0x04)),
    ("add input carry", "adc", 0xFFFFFFFF00, 0x0000000000, {"astat": 0x08},
     IntegerResult(0x0000000000, 0x09, 0x00)),
    ("subtract input borrow", "sbc", 0x0000000000, 0x0000000000, {},
     IntegerResult(0xFFFFFFFF00, 0x04, 0x00)),
    ("add carry only", "addc", 0x7FFFFFFF00, 0, {"astat": 0x08},
     IntegerResult(0x8000000000, 0x06, 0x04)),
    ("subtract borrow only", "subc", 0x0000000000, 0, {},
     IntegerResult(0xFFFFFFFF00, 0x04, 0x00)),
    ("increment maximum", "inc", 0x7FFFFFFF00, 0, {},
     IntegerResult(0x8000000000, 0x06, 0x04)),
    ("decrement minimum saturated", "dec", 0x8000000000, 0, {"saturate": True},
     IntegerResult(0x8000000000, 0x0E, 0x04)),
    ("negate zero carry", "neg", 0x0000000000, 0, {},
     IntegerResult(0x0000000000, 0x09, 0x00)),
    ("absolute minimum leaves AOS unchanged", "abs", 0x8000000000, 0, {},
     IntegerResult(0x8000000000, 0x16, 0x00)),
    ("absolute minimum saturated", "abs", 0x8000000000, 0, {"saturate": True},
     IntegerResult(0x7FFFFFFF00, 0x12, 0x00)),
    ("pass clears extension and only ALU flags", "pass", 0x00000001FF, 0,
     {"astat": 0xFFFFFFFF, "stky": 0x55}, IntegerResult(0x0000000100, 0xFFFFFBC0, 0x55)),
    ("AND", "and", 0x8000000300, 0x8000000200, {},
     IntegerResult(0x8000000200, 0x04, 0x00)),
    ("OR", "or", 0x0000000100, 0x0000000200, {},
     IntegerResult(0x0000000300, 0x00, 0x00)),
    ("XOR", "xor", 0x8000000100, 0x8000000300, {},
     IntegerResult(0x0000000200, 0x00, 0x00)),
    ("NOT", "not", 0x0000000000, 0, {}, IntegerResult(0xFFFFFFFF00, 0x04, 0x00)),
    ("signed MIN", "min", 0xFFFFFFFF00, 0x0000000100, {},
     IntegerResult(0xFFFFFFFF00, 0x04, 0x00)),
    ("signed MAX", "max", 0xFFFFFFFF00, 0x0000000100, {},
     IntegerResult(0x0000000100, 0x00, 0x00)),
    ("CLIP negative", "clip", 0xFFFFFFFC00, 0x0000000300, {},
     IntegerResult(0xFFFFFFFD00, 0x04, 0x00)),
    ("average negative tie to even", "avg", 0xFFFFFFFD00, 0, {},
     IntegerResult(0xFFFFFFFE00, 0x04, 0x00)),
    ("average negative toward zero", "avg", 0xFFFFFFFD00, 0, {"truncate": True},
     IntegerResult(0xFFFFFFFF00, 0x04, 0x00)),
    ("compare history", "cmp", 0x0000000200, 0x0000000100, {"astat": 0xA5000000},
     IntegerResult(None, 0xD2000000, 0x00)),
    ("signed compare", "cmp", 0xFFFFFFFF00, 0x0000000100, {},
     IntegerResult(None, 0x04, 0x00)),
    ("unsigned compare", "cmpu", 0xFFFFFFFF00, 0x0000000100, {},
     IntegerResult(None, 0x80000000, 0x00)),
    ("sticky survives a later non-overflow", "pass", 0x0000000100, 0, {"stky": 0x24},
     IntegerResult(0x0000000100, 0x00, 0x24)),
]

FLOATING = [
    ("strict input extension truncation", "add", 0x3F800000FF, 0, Mode(32),
     FloatingResult(0x3F80000000)),
    ("strict extended input retained", "add", 0x3F800000FF, 0, Mode(40),
     FloatingResult(0x3F800000FF)),
    ("single-precision addition", "add", 0x3F80000000, 0x4000000000, Mode(32),
     FloatingResult(0x4040000000)),
    ("extended multiply", "mul", 0x3FC0000000, 0x4000000000, Mode(40),
     FloatingResult(0x4040000000)),
    ("negative zero addition", "add", 0x8000000000, 0x8000000000, Mode(40),
     FloatingResult(0x8000000000)),
    ("opposite signed zeros", "add", 0x8000000000, 0, Mode(32), FloatingResult(0)),
    ("negative zero subtraction", "sub", 0x8000000000, 0, Mode(40),
     FloatingResult(0x8000000000)),
    ("negative zero multiply", "mul", 0x8000000000, 0x3F80000000, Mode(32),
     FloatingResult(0x8000000000)),
    ("denormal input has no underflow exception", "add", 0x0000000100, 0, Mode(32),
     FloatingResult(0)),
    ("negative denormal input flush", "mul", 0x8000000001, 0x3F80000000, Mode(40),
     FloatingResult(0x8000000000)),
    ("post-rounded underflow", "mul", 0x0080000000, 0x3F00000000, Mode(40),
     FloatingResult(0, underflow=True)),
    ("finite overflow", "add", 0x7F7FFFFFFF, 0x7F7FFFFFFF, Mode(40),
     FloatingResult(0x7F80000000, overflow=True)),
    ("truncated overflow", "add", 0x7F7FFFFFFF, 0x7F7FFFFFFF, Mode(40, "zero"),
     FloatingResult(0x7F7FFFFFFF, overflow=True)),
    ("average has no intermediate overflow", "avg", 0x7F7FFFFFFF, 0x7F7FFFFFFF, Mode(40),
     FloatingResult(0x7F7FFFFFFF)),
    ("canonical extended NaN", "add", 0x7F80000001, 0x3F80000000, Mode(40),
     FloatingResult(0xFFFFFFFFFF, invalid=True)),
    ("opposite infinities invalid", "add", 0x7F80000000, 0xFF80000000, Mode(40),
     FloatingResult(0xFFFFFFFFFF, invalid=True)),
    ("zero times infinity invalid", "mul", 0, 0x7F80000000, Mode(40),
     FloatingResult(0xFFFFFFFFFF, invalid=True)),
    ("infinity is not finite overflow", "add", 0x7F80000000, 0x3F80000000, Mode(40),
     FloatingResult(0x7F80000000)),
]


def require(actual, expected, name):
    if actual != expected:
        raise AssertionError(f"{name}: expected {expected}, got {actual}")


def anchored_cases():
    records = []
    for name, operation, left, right, options, expected in FIXED:
        actual = fixed(operation, left, right, **options)
        require(actual, expected, name)
        records.append({"name": name, "kind": "fixed", "operation": operation,
                        "left": left, "right": right, "options": options,
                        "expected": asdict(expected)})
    for name, operation, left, right, mode, expected in FLOATING:
        actual = floating(operation, left, right, mode)
        require(actual, expected, name)
        records.append({"name": name, "kind": "floating-value", "operation": operation,
                        "left": left, "right": right, "mode": asdict(mode),
                        "expected": asdict(expected)})
    conversions = [
        ("FLOAT independent of RND32", "float", 0x0100000100, Mode(32), {},
         FloatingResult(0x4B80000080)),
        ("extreme positive scale", "float", 0x0000000100, Mode(40), {"scale": 0x7FFFFFFF},
         FloatingResult(0x7F80000000, overflow=True)),
        ("extreme negative scale", "float", 0x0000000100, Mode(40), {"scale": -0x80000000},
         FloatingResult(0, underflow=True)),
        ("RND even tie", "round", 0x3F80000080, Mode(40), {}, FloatingResult(0x3F80000000)),
        ("RND odd tie", "round", 0x3F80000180, Mode(40), {}, FloatingResult(0x3F80000200)),
    ]
    for name, operation, raw, mode, options, expected in conversions:
        function = float_from_fixed if operation == "float" else float_round
        require(function(raw, mode, **options), expected, name)
        records.append({"name": name, "kind": "conversion-value", "operation": operation,
                        "left": raw, "mode": asdict(mode), "options": options,
                        "expected": asdict(expected)})
    return records


def rounding_lattice():
    """Check exact rational positions against adjacent raw bit-pattern neighbors.

    Expected neighbors come from monotone positive encodings, not normalization,
    exponent extraction or the oracle's significand division algorithm.
    """
    count = 0
    for width in (32, 40):
        precision = width - 9
        stride = 1 << (40 - width)
        hidden = 1 << precision
        for exponent in range(-126, 128):
            ulp = Fraction(2) ** (exponent - precision)
            for fraction in (0, 1, 2, 3, hidden - 2, hidden - 1):
                raw = ((exponent + 127) << 31) + fraction * stride
                for quarters in range(4):
                    magnitude = (hidden + fraction + Fraction(quarters, 4)) * ulp
                    for rounding in ("nearest", "zero"):
                        advance = rounding == "nearest" and (
                            quarters > 2 or (quarters == 2 and fraction & 1)
                        )
                        expected_raw = raw + stride * int(advance)
                        for negative in (False, True):
                            sign = 0x8000000000 if negative else 0
                            actual = quantize(-magnitude if negative else magnitude,
                                              Mode(width, rounding))
                            require(actual, FloatingResult(expected_raw | sign,
                                                            overflow=expected_raw == 0x7F80000000),
                                    f"rounding width={width} e={exponent} f={fraction} q={quarters}")
                            count += 1
    return count


def explicit_gaps():
    calls = [
        lambda: floating("add", 0x7FC0000000, 0, Mode(32)),
        lambda: floating("add", 0x7F80000001, 0, Mode(32)),
        lambda: fixed("unimplemented", 0),
        lambda: quantize(Fraction(1, 1 << 126) - Fraction(1, 1 << 158), Mode(40)),
        lambda: quantize(Fraction(1, 1 << 126) - Fraction(1, 1 << 150), Mode(32)),
        lambda: fixed_from_float("fix", 0x7FC0000000, Mode(40), saturate=True),
        lambda: fixed_from_float("fix", 0x4EFFFFFFFF, Mode(40)),
        lambda: fixed_from_float("trunc", 0x7F80000000, Mode(40)),
    ]
    for call in calls:
        try:
            call()
        except Unqualified:
            continue
        raise AssertionError("an unqualified expectation was silently generated")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="write qualified numeric cases as JSON")
    arguments = parser.parse_args()
    records = anchored_cases()
    records.extend(multiplier_expectations.anchors())
    records.extend(conversion_expectations.anchors())
    records.extend(shifter_expectations.anchors())
    records.extend(floating_expectations.anchors())
    records.extend(parallel_expectations.anchors())
    records.extend(packing_expectations.anchors())
    records.extend(condition_expectations.anchors())
    records.extend(writeback_expectations.anchors())
    writeback_count, writeback_refusals = writeback_expectations.bank_priority_lattice()
    writeback_expectations.invalid_inputs()
    condition_count = condition_expectations.predicate_lattice() + condition_expectations.lane_lattice()
    condition_expectations.explicit_gaps()
    packing_count, packing_refusals = packing_expectations.packing_lattice()
    seed_count = packing_expectations.seed_lattice()
    parallel_count = parallel_expectations.fixed_lattice() + parallel_expectations.floating_lattice()
    parallel_expectations.explicit_refusals()
    floating_count, floating_refusals = floating_expectations.exact_lattice()
    unary_count, unary_refusals = floating_expectations.unary_lattice()
    parts_count, parts_refusals = floating_expectations.parts_lattice()
    floating_expectations.explicit_gaps()
    shifter_count = shifter_expectations.bit_lattice()
    shifter_count += shifter_expectations.leading_lattice()
    fifo_count = shifter_expectations.fifo_lattice()
    fifo_count += shifter_expectations.pointer_lattice()
    unpack_count = shifter_expectations.unpack_lattice()
    shifter_expectations.explicit_gaps()
    conversion_count, conversion_refusals = conversion_expectations.quotient_lattice()
    multiplier_count = multiplier_expectations.product_lattice()
    lattice_count = rounding_lattice()
    explicit_gaps()
    if arguments.output:
        payload = {"contract": "strict-numeric-foundation-v1", "isa_execution_coverage": False,
                   "cases": records}
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps(payload, indent=2) + "\n")
    print(f"Validated {len(records)} numeric/conversion/accumulator anchors and "
          f"{lattice_count} rounding positions and {multiplier_count} limb-based multiplier cases, "
          f"{conversion_count} signed-quotient conversions ({conversion_refusals} explicit refusals); "
          f"{shifter_count} shifter, {fifo_count} FIFO, {unpack_count} unpack cases; "
          f"{floating_count + unary_count + parts_count} full FP cases "
          f"({floating_refusals + unary_refusals + parts_refusals} explicit FP refusals); "
          f"{parallel_count} multifunction cases; "
          f"{packing_count} pack fields ({packing_refusals} refused rounding cases), "
          f"{seed_count} seed special cases; "
          f"{condition_count} predicate/aggregation cases; "
          f"{writeback_count} bank/writeback cases ({writeback_refusals} refused priority ties); "
          "no installed-image ISA coverage claimed.")


if __name__ == "__main__":
    main()
