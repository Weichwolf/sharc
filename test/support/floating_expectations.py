# SPDX-License-Identifier: GPL-3.0-only
"""Full scalar FP anchors, exact dyadic arithmetic and raw-field cross-checks."""

from itertools import product as cartesian_product
from dataclasses import asdict
from fractions import Fraction

from expectation_check import anchor, require, require_raises
from reference import (
    IntegerResult, Mode, Operand, Unqualified, fixed_float_effects, float_parts, floating_alu,
    floating_compare, floating_multiply, quantize, raw_word, signed,
)


ALU = [
    ("ADD cancellation preserves AUS", "add", 0x3F80000000, 0xBF80000000, Mode(40), {},
     IntegerResult(0, 0x401, 0)),
    ("SUB tiny result sets AUS", "sub", 0x0080000000, 0x0080000001, Mode(40), {},
     IntegerResult(0x8000000000, 0x405, 1)),
    ("ADD overflow flags", "add", 0x7F7FFFFFFF, 0x7F7FFFFFFF, Mode(40), {},
     IntegerResult(0x7F80000000, 0x402, 2)),
    ("SUB negative overflow", "sub", 0xFF7FFFFFFF, 0x7F7FFFFFFF, Mode(40), {},
     IntegerResult(0xFF80000000, 0x406, 2)),
    ("absolute sum invalid clears AN", "absadd", 0x7F80000000, 0xFF80000000, Mode(40), {},
     IntegerResult(0xFFFFFFFFFF, 0x420, 0x20)),
    ("absolute difference invalid clears AN", "abssub", 0x7F80000000, 0x7F80000000, Mode(40), {},
     IntegerResult(0xFFFFFFFFFF, 0x420, 0x20)),
    ("absolute sum clears negative zero", "absadd", 0x8000000000, 0x8000000000, Mode(40), {},
     IntegerResult(0, 0x401, 0)),
    ("average cannot overflow", "avg", 0x7F7FFFFFFF, 0x7F7FFFFFFF, Mode(40), {},
     IntegerResult(0x7F7FFFFFFF, 0x400, 0)),
    ("average flushes midpoint below minimum normal", "avg", 0x0140000000, 0x8080000001,
     Mode(40), {}, IntegerResult(0, 0x401, 1)),
    ("binary32 average flushes negative midpoint", "avg", 0x8140000000, 0x0080000100,
     Mode(32), {}, IntegerResult(0x8000000000, 0x405, 1)),
    ("PASS zero leaves AUS unchanged", "pass", 0, 0, Mode(40), {}, IntegerResult(0, 0x401, 0)),
    ("PASS denormal flush leaves AUS unchanged", "pass", 1, 0, Mode(40), {},
     IntegerResult(0, 0x401, 0)),
    ("PASS negative zero sets AN", "pass", 0x8000000000, 0, Mode(40), {},
     IntegerResult(0x8000000000, 0x405, 0)),
    ("RND32 PASS treats an extension-only NaN as infinity", "pass",
     0x7F80000001, 0, Mode(32), {}, IntegerResult(0x7F80000000, 0x400, 0)),
    ("RND32 PASS writes a binary32 NaN", "pass", 0xFFC0000000, 0, Mode(32), {},
     IntegerResult(0xFFFFFFFF00, 0x420, 0x20)),
    ("NEG normal", "neg", 0x3F80000000, 0, Mode(32), {},
     IntegerResult(0xBF80000000, 0x404, 0)),
    ("ABS negative zero records input sign", "abs", 0x8000000000, 0, Mode(40), {},
     IntegerResult(0, 0x411, 0)),
    ("ABS negative NaN clears AS", "abs", 0xFFC0000000, 0, Mode(40), {},
     IntegerResult(0xFFFFFFFFFF, 0x420, 0x20)),
    ("RND32 ABS sees negative infinity in an extension-only NaN", "abs", 0xFF80000001,
     0, Mode(32), {}, IntegerResult(0x7F80000000, 0x410, 0)),
    ("RND overflow", "round", 0x7F7FFFFF80, 0, Mode(40), {},
     IntegerResult(0x7F80000000, 0x402, 2)),
    ("RND odd tie effects", "round", 0x3F80000180, 0, Mode(40), {},
     IntegerResult(0x3F80000200, 0x400, 0)),
    ("RND with RND32 ignores input extension", "round", 0x3F80000180, 0, Mode(32), {},
     IntegerResult(0x3F80000100, 0x400, 0)),
    ("RND produces a binary32 NaN", "round", 0x7F80000001, 0, Mode(40), {},
     IntegerResult(0xFFFFFFFF00, 0x420, 0x20)),
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
    ("scaled FLOAT exact zero preserves AUS", "float", 0x42, 0, Mode(32), {"scale": 0},
     IntegerResult(0, 0x401, 0)),
    ("FLOAT always uses full precision", "float", 0x0100000100, 0, Mode(32), {},
     IntegerResult(0x4B80000080, 0x400, 0)),
    ("scaled FLOAT preserves the sign of underflow", "float", 0xFFFFFFFF00, 0,
     Mode(40), {"scale": -127}, IntegerResult(0x8000000000, 0x405, 1)),
    ("scaled FLOAT overflow truncates to the extended finite limit", "float", 0x100, 0,
     Mode(32, "zero"), {"scale": 128}, IntegerResult(0x7F7FFFFFFF, 0x402, 2)),
    ("scaled FLOAT accepts minimum signed scale", "float", 0x100, 0,
     Mode(40), {"scale": -(1 << 31)}, IntegerResult(0, 0x401, 1)),
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
    ("FP multiply negative zero sets MN", 0, 0xBF80000000, Mode(40), {},
     IntegerResult(0x8000000000, 0x40, 0)),
    ("FP multiply NaN uses left NaN sign", 0x7FC0000000, 0xFFC0000000, Mode(40), {},
     IntegerResult(0x7FFFFFFFFF, 0x200, 0x200)),
    ("FP multiply NaN uses right NaN sign", 0x3F80000000, 0xFFC0000000, Mode(32), {},
     IntegerResult(0xFFFFFFFF00, 0x200, 0x200)),
    ("FP zero times negative infinity uses product sign", 0, 0xFF80000000, Mode(32), {},
     IntegerResult(0xFFFFFFFF00, 0x200, 0x200)),
    ("FP multiply midpoint promotes with underflow", 0x0080000000, 0x3F7FFFFFFF, Mode(40), {},
     IntegerResult(0x0080000000, 0x100, 0x100)),
    ("FP multiply just above midpoint promotes without underflow",
     0x0080000001, 0x3F7FFFFFFE, Mode(40), {}, IntegerResult(0x0080000000, 0, 0)),
    ("FP multiply negative binary32 midpoint promotes with underflow",
     0x8080000000, 0x3F7FFFFF00, Mode(32), {}, IntegerResult(0x8080000000, 0x140, 0x100)),
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
    ("unordered compare shifts one into CACC", 0x3F80000000, 0x7F80000001,
     Mode(40), {"astat": 0xA5000405}, IntegerResult(None, 0xD2000420, 0x20)),
    ("RND32 compare treats extension-only NaN as infinity", 0x3F80000000, 0x7F80000001,
     Mode(32), {"astat": 0xA5000405}, IntegerResult(None, 0x52000404, 0)),
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
    count = 0
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
                negative = value < 0 or negative_zero
                raw = encode(value) | (0x8000000000 if negative_zero else 0)
                if operation == "mul":
                    flags = 0xFFFFFC3F | (int(negative) << 6)
                else:
                    flags = 0xFFFFFFC0 | int(value == 0) | (int(negative) << 2)
                require(run(), IntegerResult(raw, flags, 0x44), (operation, x, y, mode))
                count += 1
            # History is independently assembled as a textual shift register.
            history = int(str(int(x > y)) + "10100101"[:7], 2) << 24
            flags = 0x00FFFFC0 | int(x == y) | (int(x < y) << 2) | history
            require(floating_compare(left, right, mode, astat=0xA5FFFFFF, stky=0x44),
                    IntegerResult(None, flags, 0x44), ("compare", x, y, mode))
            count += 1
    return count


def unary_lattice():
    count = 0
    for width, rounding in cartesian_product((32, 40), ("nearest", "zero")):
        mode = Mode(width, rounding)
        for value in (-4, -2, -1, 0, 1, 2, 4):
            cases = [(operation, None) for operation in ("pass", "neg", "abs", "round")]
            cases += [(operation, scale) for operation in ("scale", "float")
                      for scale in (-2, -1, 0, 1, 2)] + [("float", None)]
            for operation, scale in cases:
                source = ((value % 4294967296) * 256 + 255) if operation == "float" else encode(value)
                output = -value if operation == "neg" else abs(value) if operation == "abs" else value
                if scale is not None:
                    output *= Fraction(2)**scale
                negative_zero = operation == "neg" and value == 0
                flags = 0xFFFFFFC0 | int(output == 0) | (int(output < 0 or negative_zero) << 2)
                flags |= 0x10 if operation == "abs" and value < 0 else 0
                sticky = 0x44 | int(output == 0 and operation == "scale")
                raw = 0x8000000000 if negative_zero else encode(output)
                require(floating_alu(operation, source, mode=mode, scale=scale,
                                     astat=0xFFFFFFFF, stky=0x44),
                        IntegerResult(raw, flags, sticky), (operation, value, mode, scale))
                count += 1
    return count


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
    require_raises(Unqualified, float_parts, 'unqualified floating flags generated',
                   "logb", 0x7FC0000000, Mode(40))


def installed_header():
    """Operand-level FastFP32 rounding and full installed-image lane effects."""
    fields = {"add": 0x081000, "sub": 0x082000, "avg": 0x089000, "cmp": 0x08a000,
              "absadd": 0x091000, "abssub": 0x092000, "pass": 0x0a1000,
              "neg": 0x0a2000, "round": 0x0a5000, "abs": 0x0b0000,
              "float": 0x0ca000, "float-scaled": 0x0da000,
              "copysign": 0x0e0000, "min": 0x0e1000, "max": 0x0e2000, "clip": 0x0e3000,
              "mul": 0x130000}
    unary = {"pass", "neg", "round", "abs", "float"}
    edges = [(0x3f800000ff, 0), (0x3f80000180, 0x3380000000),
             (0x3f80000000, 0xbf80000000), (0x3f80000000, 0x0080000000),
             (0x7f7fffffff, 0x7f7fffffff), (0x7f7fffffff, 0x4000000000),
             (0x0080000000, 0x3f00000000), (0x0080000000, 0x8080000000),
             (0x7f80000000, 0x3f80000000), (0x0000000001, 0x4000000000),
             (0x7fc0000000, 0x3f80000000), (0x7f80000001, 0xbf80000000),
             (0x8000000000, 0), (0x8000000000, 0x8000000000),
             (0x8080000000, 0x3f7fffffff), (0x0080000000, 0x3f7ffffe00)]
    # Vary significands and alignment distances near and beyond both boundaries.
    for index in range(24):
        top = (0x4a + index) << 31
        lower = (0x4a + index - index * 3) << 31
        left = top | ((index * 0x2853a7d) & 0x7fffffff)
        right = lower | ((0x79ae52cf - index * 0x1473c1b) & 0x7fffffff)
        edges.append((left, right | ((index % 2) << 39)))
    anchors = {operation: [(left, right) for _, name, left, right, _, _, _ in ALU
                           if name == operation] for operation in fields}
    anchors["mul"] = [(left, right) for _, left, right, _, _, _ in MULTIPLY]
    anchors["cmp"] = [(left, right) for _, left, right, _, _, _ in COMPARE]
    anchors["float-scaled"] = [
        (left, (options["scale"] & 0xffffffff) << 8)
        for _, name, left, _, _, options, _ in ALU if name == "float" and "scale" in options
    ]
    fixed_edges = [
        (0, 0),
        (0x0100000157, 0xffffff8100),
        (0x0100000357, 0x0000000200),
        (0x7fffffffef, 0x7fffffff00),
        (0x8000000042, 0x8000000000),
        (0xfeffffff57, 0x0000008000),
    ]

    def approximate(raw, mode):
        value = Operand(raw)
        if value.kind == "nan":
            # Preserve the computational NaN class and sign, including payloads
            # that are confined to the source extension byte.
            return 0x7fc0000000 | (raw & 0x8000000000)
        if value.kind == "infinity":
            return raw
        return quantize(value.number(), mode, negative_zero=value.negative).raw

    def result(operation, left, right, mode, fast, astat, stky):
        if operation in ("float", "float-scaled"):
            scale = signed(raw_word(right)) if operation == "float-scaled" else None
            return fixed_float_effects(left, mode, scale=scale, precision=32 if fast else 40,
                                       astat=astat, stky=stky)
        if fast:
            left, right = approximate(left, mode), approximate(right, mode)
        if operation == "mul":
            return floating_multiply(left, right, mode, astat=astat, stky=stky)
        if operation == "cmp":
            return floating_compare(left, right, mode, astat=astat, stky=stky)
        return floating_alu(operation, left, right, mode=mode, astat=astat, stky=stky)

    def array(values):
        return "{" + ", ".join(f"0x{value:x}ULL" for value in values) + "}"

    rows = []
    for operation, field in fields.items():
        inputs = fixed_edges if operation in ("float", "float-scaled") else edges
        pairs = [(x, 0) for x, _ in inputs] if operation in unary else inputs
        for index, (left, right) in enumerate(dict.fromkeys(anchors[operation] + pairs)):
            for fast, rnd32, truncate in cartesian_product((False, True), repeat=3):
                mode = Mode(32 if fast or rnd32 else 40, "zero" if truncate else "nearest")
                phase = index % 2
                old_astat = (0xa500043a | phase, 0x420003c4 | (1 - phase))
                old_stky = (0x44 | phase, 0x108 | (1 - phase))
                x = (left, left ^ (1 << 39)) if operation in unary else (left, right)
                y = (right, left)
                expected = [result(operation, a, b, mode, fast, status, sticky)
                            for a, b, status, sticky in zip(x, y, old_astat, old_stky)]
                controls = (fast, rnd32, truncate, operation != "cmp")
                rows.append("{" + ", ".join((hex(field), *(str(bit).lower() for bit in controls),
                            array(x), array(y), array(old_astat), array(old_stky),
                            array(item.raw or 0 for item in expected),
                            array(item.astat for item in expected),
                            array(item.stky for item in expected))) + "}")
    return """struct FloatingCase {
  std::uint32_t field;
  bool fast, rnd32, truncate, writes;
  std::array<std::uint64_t, 2> x, y;
  std::array<std::uint32_t, 2> old_astat, old_stky;
  std::array<std::uint64_t, 2> raw;
  std::array<std::uint32_t, 2> astat, stky;
};
inline constexpr std::array<FloatingCase, """ + str(len(rows)) + """> floating_cases{{
""" + ",\n".join(rows) + "\n}};\n"
