# SPDX-License-Identifier: GPL-3.0-only
"""Full scalar FP anchors, exact dyadic arithmetic and raw-field cross-checks."""

from itertools import product as cartesian_product
from dataclasses import asdict
from fractions import Fraction

from expectation_check import anchor, require, require_raises
from reference import (
    IntegerResult, Mode, Unqualified, float_parts, floating_alu, floating_compare,
    floating_multiply,
)


ALU = [
    ("ADD cancellation sets AUS", "add", 0x3F80000000, 0xBF80000000, Mode(40), {},
     IntegerResult(0, 0x401, 1)),
    ("ADD overflow flags", "add", 0x7F7FFFFFFF, 0x7F7FFFFFFF, Mode(40), {},
     IntegerResult(0x7F80000000, 0x402, 2)),
    ("SUB negative overflow", "sub", 0xFF7FFFFFFF, 0x7F7FFFFFFF, Mode(40), {},
     IntegerResult(0xFF80000000, 0x406, 2)),
    ("absolute sum invalid clears AN", "absadd", 0x7F80000000, 0xFF80000000, Mode(40), {},
     IntegerResult(0xFFFFFFFFFF, 0x420, 0x20)),
    ("absolute difference invalid clears AN", "abssub", 0x7F80000000, 0x7F80000000, Mode(40), {},
     IntegerResult(0xFFFFFFFFFF, 0x420, 0x20)),
    ("absolute sum clears negative zero", "absadd", 0x8000000000, 0x8000000000, Mode(40), {},
     IntegerResult(0, 0x401, 1)),
    ("average cannot overflow", "avg", 0x7F7FFFFFFF, 0x7F7FFFFFFF, Mode(40), {},
     IntegerResult(0x7F7FFFFFFF, 0x400, 0)),
    ("PASS zero leaves AUS unchanged", "pass", 0, 0, Mode(40), {}, IntegerResult(0, 0x401, 0)),
    ("PASS denormal flush leaves AUS unchanged", "pass", 1, 0, Mode(40), {},
     IntegerResult(0, 0x401, 0)),
    ("NEG normal", "neg", 0x3F80000000, 0, Mode(32), {},
     IntegerResult(0xBF80000000, 0x404, 0)),
    ("ABS negative zero records input sign", "abs", 0x8000000000, 0, Mode(40), {},
     IntegerResult(0, 0x411, 0)),
    ("ABS negative NaN", "abs", 0xFFC0000000, 0, Mode(40), {},
     IntegerResult(0xFFFFFFFFFF, 0x430, 0x20)),
    ("RND overflow", "round", 0x7F7FFFFF80, 0, Mode(40), {},
     IntegerResult(0x7F80000000, 0x402, 2)),
    ("RND odd tie effects", "round", 0x3F80000180, 0, Mode(32), {},
     IntegerResult(0x3F80000200, 0x400, 0)),
    ("RND toward zero effects", "round", 0xBF800001FF, 0, Mode(40, "zero"), {},
     IntegerResult(0xBF80000100, 0x404, 0)),
    ("RND zero leaves AUS unchanged", "round", 0, 0, Mode(32), {},
     IntegerResult(0, 0x401, 0)),
    ("SCALB underflow sets AUS", "scale", 0x0080000000, 0, Mode(40), {"scale": -1},
     IntegerResult(0, 0x401, 1)),
    ("SCALB overflow to truncated maximum", "scale", 0x7F7FFFFFFF, 0, Mode(40, "zero"),
     {"scale": 1}, IntegerResult(0x7F7FFFFFFF, 0x402, 2)),
    ("SCALB infinity is not finite overflow", "scale", 0x7F80000000, 0, Mode(40),
     {"scale": -0x80000000}, IntegerResult(0x7F80000000, 0x400, 0)),
    ("unscaled FLOAT zero preserves AUS", "float", 0, 0, Mode(32), {},
     IntegerResult(0, 0x401, 0)),
    ("explicit scale zero FLOAT updates AUS", "float", 0, 0, Mode(32), {"scale": 0},
     IntegerResult(0, 0x401, 1)),
    ("FLOAT always uses full precision", "float", 0x0100000100, 0, Mode(32), {},
     IntegerResult(0x4B80000080, 0x400, 0)),
    ("MIN orders negative infinity", "min", 0xFF80000000, 0xBF80000000, Mode(40), {},
     IntegerResult(0xFF80000000, 0x404, 0)),
    ("MAX opposite zeros selects positive", "max", 0x8000000000, 0, Mode(40), {},
     IntegerResult(0, 0x401, 0)),
    ("CLIP copies negative sign", "clip", 0xC040000000, 0x4000000000, Mode(40), {},
     IntegerResult(0xC000000000, 0x404, 0)),
    ("COPYSIGN infinity", "copysign", 0x7F80000000, 0xBF80000000, Mode(40), {},
     IntegerResult(0xFF80000000, 0x404, 0)),
    ("RND32 PASS clears extension", "pass", 0x3F800000FF, 0, Mode(32), {},
     IntegerResult(0x3F80000000, 0x400, 0)),
    ("PASS preserves unrelated flags and old sticky bits", "pass", 0xBF80000000, 0, Mode(40),
     {"astat": 0xFFFFFFFF, "stky": 0x246}, IntegerResult(0xBF80000000, 0xFFFFFFC4, 0x246)),
]

PARTS = [
    ("MANT retains extension", "mant", 0xBF80000001, Mode(40), {},
     IntegerResult(0x8000000100, 0x410, 0)),
    ("MANT negative denormal", "mant", 0x8000000001, Mode(40), {}, IntegerResult(0, 0x411, 0)),
    ("MANT positive infinity", "mant", 0x7F80000000, Mode(32), {},
     IntegerResult(0xFFFFFFFF00, 0x402, 2)),
    ("MANT negative NaN fixed sentinel", "mant", 0xFFC0000000, Mode(40), {},
     IntegerResult(0xFFFFFFFF00, 0x430, 0x20)),
    ("LOGB exact zero exponent does not set AUS", "logb", 0xBF80000000, Mode(40), {},
     IntegerResult(0, 0x401, 0)),
    ("LOGB minimum normal", "logb", 0x0080000000, Mode(32), {},
     IntegerResult(0xFFFFFF8200, 0x404, 0)),
    ("LOGB negative infinity returns positive infinity", "logb", 0xFF80000000, Mode(40), {},
     IntegerResult(0x7F80000000, 0x402, 2)),
    ("LOGB denormal returns negative infinity", "logb", 0x0000000001, Mode(40), {},
     IntegerResult(0xFF80000000, 0x406, 2)),
    ("LOGB zero saturated", "logb", 0, Mode(32), {"saturate": True},
     IntegerResult(0x8000000000, 0x406, 2)),
    ("LOGB infinity saturated", "logb", 0xFF80000000, Mode(40), {"saturate": True},
     IntegerResult(0x7FFFFFFF00, 0x402, 2)),
]

MULTIPLY = [
    ("FP multiply preserves ALU flags", 0xBF80000000, 0x4000000000, Mode(40),
     {"astat": 0xFFFFFFFF, "stky": 4}, IntegerResult(0xC000000000, 0xFFFFFC7F, 4)),
    ("FP multiply exact zero has no MU", 0, 0x3F80000000, Mode(32), {}, IntegerResult(0, 0, 0)),
    ("FP multiply post-rounded underflow", 0x0080000000, 0x3F00000000, Mode(40), {},
     IntegerResult(0, 0x100, 0x100)),
    ("FP multiply overflow", 0x7F7FFFFFFF, 0x4000000000, Mode(40), {},
     IntegerResult(0x7F80000000, 0x80, 0x80)),
]

COMPARE = [
    ("compare opposite zeros equal", 0x8000000000, 0, Mode(40), {},
     IntegerResult(None, 0x401, 0)),
    ("compare infinities ordered", 0xFF80000000, 0x7F80000000, Mode(32), {},
     IntegerResult(None, 0x404, 0)),
    ("compare greater infinity", 0x7F80000000, 0xFF80000000, Mode(40), {},
     IntegerResult(None, 0x80000400, 0)),
    ("floating compare history", 0x4000000000, 0x3F80000000, Mode(40), {"astat": 0xA5000000},
     IntegerResult(None, 0xD2000400, 0)),
    ("RND32 compare input extension", 0x3F80000001, 0x3F80000000, Mode(32), {},
     IntegerResult(None, 0x401, 0)),
    ("extended compare input extension", 0x3F80000001, 0x3F80000000, Mode(40), {},
     IntegerResult(None, 0x80000400, 0)),
]


def anchors():
    records = []
    for name, operation, left, right, mode, options, expected in ALU:
        records.append(anchor(name, "floating-alu",
                              floating_alu(operation, left, right, mode=mode, **options), expected,
                              operation=operation, left=left, right=right,
                              mode=asdict(mode), options=options))
    for name, operation, raw, mode, options, expected in PARTS:
        records.append(anchor(name, "floating-parts", float_parts(operation, raw, mode, **options),
                              expected, operation=operation, left=raw, mode=asdict(mode), options=options))
    for kind, function, cases in (("floating-multiply", floating_multiply, MULTIPLY),
                                  ("floating-compare", floating_compare, COMPARE)):
        for name, left, right, mode, options, expected in cases:
            records.append(anchor(name, kind, function(left, right, mode, **options), expected,
                                  left=left, right=right, mode=asdict(mode), options=options))
    return records


# Every value needed by the exact dyadic lattice has an authored wire constant.
# Arithmetic selects from this codebook; it never calls the oracle's quantizer.
CODEBOOK = {
    Fraction(0): 0, Fraction(1, 4): 0x3E80000000, Fraction(1, 2): 0x3F00000000,
    Fraction(1): 0x3F80000000, Fraction(3, 2): 0x3FC0000000,
    Fraction(2): 0x4000000000, Fraction(5, 2): 0x4020000000,
    Fraction(3): 0x4040000000, Fraction(4): 0x4080000000, Fraction(5): 0x40A0000000,
    Fraction(6): 0x40C0000000, Fraction(8): 0x4100000000, Fraction(16): 0x4180000000,
}


def encode(value):
    return CODEBOOK[abs(value)] | (0x8000000000 if value < 0 else 0)


def exact_lattice():
    count, refusals = 0, 0
    values = (-4, -2, -1, 0, 1, 2, 4)
    for width, rounding in cartesian_product((32, 40), ("nearest", "zero")):
        mode = Mode(width, rounding)
        for x, y in cartesian_product(values, values):
            left, right = encode(x), encode(y)
            outputs = {"add": x + y, "sub": x - y, "absadd": abs(x + y),
                       "abssub": abs(x - y), "avg": Fraction(x + y, 2),
                       "min": min(x, y), "max": max(x, y),
                       "clip": min(abs(x), abs(y)) * (-1 if x < 0 else 1),
                       "copysign": abs(x) * (-1 if y < 0 else 1), "mul": x * y}
            for operation, value in outputs.items():
                negative_zero = value == 0 and (
                    (operation == "clip" and x < 0) or
                    (operation == "copysign" and y < 0) or
                    (operation == "mul" and (x < 0) != (y < 0)))
                def run():
                    if operation == "mul":
                        return floating_multiply(left, right, mode, astat=0xFFFFFFFF, stky=0x44)
                    return floating_alu(operation, left, right, mode=mode, astat=0xFFFFFFFF, stky=0x44)
                if negative_zero:
                    require_raises(Unqualified, run, 'unqualified negative-zero flags generated')
                    refusals += 1
                    continue
                if operation == "mul":
                    flags, sticky = 0xFFFFFC3F | (int(value < 0) << 6), 0x44
                else:
                    flags = 0xFFFFFFC0 | int(value == 0) | (int(value < 0) << 2)
                    sticky = 0x44 | int(value == 0 and operation in (
                        "add", "sub", "absadd", "abssub", "avg"))
                require(run(), IntegerResult(encode(value), flags, sticky), (operation, x, y, mode))
                count += 1
            # History is independently assembled as a textual shift register.
            history = int(str(int(x > y)) + "10100101"[:7], 2) << 24
            flags = 0x00FFFFC0 | int(x == y) | (int(x < y) << 2) | history
            require(floating_compare(left, right, mode, astat=0xA5FFFFFF, stky=0x44),
                    IntegerResult(None, flags, 0x44), ("compare", x, y, mode))
            count += 1
    return count, refusals


def unary_lattice():
    count, refusals = 0, 0
    for width, rounding in cartesian_product((32, 40), ("nearest", "zero")):
        mode = Mode(width, rounding)
        for value in (-4, -2, -1, 0, 1, 2, 4):
            cases = [(operation, None) for operation in ("pass", "neg", "abs", "round")]
            cases += [(operation, scale) for operation in ("scale", "float")
                      for scale in (-2, -1, 0, 1, 2)] + [("float", None)]
            for operation, scale in cases:
                source = ((value % 4294967296) * 256 + 255) if operation == "float" else encode(value)
                if operation == "neg" and value == 0:
                    require_raises(Unqualified, floating_alu, 'unqualified NEG zero flags generated',
                                   operation, source, mode=mode)
                    refusals += 1
                    continue
                output = -value if operation == "neg" else abs(value) if operation == "abs" else value
                if scale is not None:
                    output *= Fraction(2)**scale
                flags = 0xFFFFFFC0 | int(output == 0) | (int(output < 0) << 2)
                flags |= 0x10 if operation == "abs" and value < 0 else 0
                sticky = 0x44 | int(output == 0 and scale is not None)
                require(floating_alu(operation, source, mode=mode, scale=scale,
                                     astat=0xFFFFFFFF, stky=0x44),
                        IntegerResult(encode(output), flags, sticky), (operation, value, mode, scale))
                count += 1
    return count, refusals


def parts_lattice():
    count, refusals = 0, 0
    for exponent in range(1, 255):
        for fraction, negative in cartesian_product((0, 1, 127, 128, 255, 256, 0x7FFFFF00, 0x7FFFFFFF), (False, True)):
            raw = (int(negative) << 39) | (exponent << 31) | fraction
            for width, rounding, operation in cartesian_product((32, 40), ("nearest", "zero"), ("mant", "logb")):
                if operation == "mant" and width == 32 and fraction % 256:
                    require_raises(Unqualified, float_parts, 'unqualified MANT extension accepted',
                                   operation, raw, Mode(width, rounding))
                    refusals += 1
                    continue
                if operation == "mant":
                    value = (2147483648 + fraction) * 256
                    flags = 0xFFFFFFC0 | (int(negative) << 4)
                else:
                    value = (exponent - 127) % 4294967296 * 256
                    flags = 0xFFFFFFC0 | int(exponent == 127) | (int(exponent < 127) << 2)
                require(float_parts(operation, raw, Mode(width, rounding),
                                    astat=0xFFFFFFFF, stky=0x44),
                        IntegerResult(value, flags, 0x44), (operation, raw, width, rounding))
                count += 1
    return count, refusals


def explicit_gaps():
    for call in (
        lambda: floating_alu("pass", 0x8000000000, mode=Mode(40)),
        lambda: floating_alu("add", 0x7FC0000000, mode=Mode(40)),
        lambda: floating_multiply(0, 0xBF80000000, Mode(40)),
        lambda: floating_compare(0x7FC0000000, 0, Mode(40)),
        lambda: float_parts("logb", 0x7FC0000000, Mode(40)),
    ):
        require_raises(Unqualified, call, 'unqualified floating flags generated')
