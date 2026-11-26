# SPDX-License-Identifier: GPL-3.0-only
"""Independent dual/multifunction effects and authored two-lane expectations."""

from itertools import product as cartesian_product
from dataclasses import asdict, is_dataclass
from fractions import Fraction

from floating_expectations import encode
from multiplier_expectations import _limb_accumulate, _limb_product
from expectation_check import require, require_raises
from reference import (
    IntegerResult, Mode, ParallelResult, Unqualified, dual_alu, fixed, floating_alu,
    floating_multiply, parallel_compute,
)


DUAL = [
    ("dual zero and carry from subtraction", "fixed", 0x0000000100, 0x0000000100, {},
     ParallelResult((0x0000000200, 0), 9, 0)),
    ("dual overflow and subtraction carry", "fixed", 0x7FFFFFFF00, 0x0000000100, {},
     ParallelResult((0x8000000000, 0x7FFFFFFE00), 0xE, 4)),
    ("dual saturated overflow", "fixed", 0x7FFFFFFF00, 0x0000000100, {"saturate": True},
     ParallelResult((0x7FFFFFFF00, 0x7FFFFFFE00), 0xA, 4)),
    ("dual floating cancellation", "float", 0x3F80000000, 0xBF80000000, {"mode": Mode(40)},
     ParallelResult((0, 0x4000000000), 0x401, 1)),
    ("dual floating overflow and zero", "float", 0x7F7FFFFFFF, 0x7F7FFFFFFF,
     {"mode": Mode(40)}, ParallelResult((0x7F80000000, 0), 0x403, 3)),
]

PACKETS = [
    ("fixed destination aliases both old input groups", "fixed",
     (0x4000000000, 0x4000000000, 0x0000000300, 0x0000000400), (8, 0),
     {"accumulator": 0x1234}, ParallelResult((0x2000000000, 0x0000000700), 0, 0, 0x1234)),
    ("fixed triple destination aliases old inputs", "fixed",
     (0x4000000000, 0x4000000000, 0x0000000300, 0x0000000400), (8, 0, 4),
     {"alu": "dual", "accumulator": 0x1234},
     ParallelResult((0x2000000000, 0x0000000700, 0xFFFFFFFF00), 4, 0, 0x1234)),
    ("fixed MRF accumulation and old ALU input", "fixed",
     (0x4000000000, 0x4000000000, 0x0000000300, 0x0000000400), (None, 0),
     {"multiplier": "mac", "destination": "mr", "modifier": "SSF", "accumulator": 0xFFFFFFFF},
     ParallelResult((None, 0x0000000700), 0, 0, 0x000020000000FFFFFFFF)),
    ("floating destination aliases both old input groups", "float",
     (0x3FC0000000, 0x4000000000, 0x4080000000, 0x3F80000000), (8, 0),
     {"mode": Mode(40)}, ParallelResult((0x4040000000, 0x40A0000000), 0x400, 0, 0)),
    ("floating triple reads old inputs", "float",
     (0x3FC0000000, 0x4000000000, 0x4080000000, 0x3F80000000), (8, 0, 4),
     {"mode": Mode(32), "alu": "dual"},
     ParallelResult((0x4040000000, 0x40A0000000, 0x4040000000), 0x400, 0, 0)),
    ("floating product and scaled FLOAT", "float",
     (0x3F80000000, 0x4000000000, 0xFFFFFFFF00, 0), (8, 0),
     {"mode": Mode(32), "alu": "float"},
     ParallelResult((0x4000000000, 0xBF80000000), 0x404, 0, 0)),
    ("floating product and scaled FIX", "float",
     (0x4000000000, 0x4040000000, 0x3FC0000000, 0), (8, 0),
     {"mode": Mode(40), "alu": "fix"}, ParallelResult((0x40C0000000, 0x0000000200), 0x400, 0, 0)),
    ("multiplier underflow plus dual ALU overflow and zero", "float",
     (0x0080000000, 0x3F00000000, 0x7F7FFFFFFF, 0x7F7FFFFFFF), (8, 0, 4),
     {"mode": Mode(40), "alu": "dual"},
     ParallelResult((0, 0x7F80000000, 0), 0x503, 0x103, 0)),
    ("multiplier overflow plus absolute ALU invalid", "float",
     (0x7F7FFFFFFF, 0x4000000000, 0x7FC0000000, 0), (8, 0),
     {"mode": Mode(40), "alu": "abs"},
     ParallelResult((0x7F80000000, 0xFFFFFFFFFF), 0x4A0, 0xA0, 0)),
]

LANES = [
    ("lane-local carry versus overflow", "fixed", "add",
     (0xFFFFFFFF00, 0x7FFFFFFF00), (0x0000000100, 0x0000000100), {},
     (IntegerResult(0, 9, 0), IntegerResult(0x8000000000, 6, 4))),
    ("lane-local saturation", "fixed", "add",
     (0x7FFFFFFF00, 0x0000000100), (0x0000000100, 0x0000000200), {"saturate": True},
     (IntegerResult(0x7FFFFFFF00, 2, 4), IntegerResult(0x0000000300, 0, 0))),
    ("lane-local floating sticky predicates", "float", "add",
     (0x7F7FFFFFFF, 0x3F80000000), (0x7F7FFFFFFF, 0xBF80000000), {"mode": Mode(40)},
     (IntegerResult(0x7F80000000, 0x402, 2), IntegerResult(0, 0x401, 1))),
    ("lane-local multiplier overflow versus underflow", "multiply", "mul",
     (0x7F7FFFFFFF, 0x0080000000), (0x4000000000, 0x3F00000000), {"mode": Mode(40)},
     (IntegerResult(0x7F80000000, 0x80, 0x80), IntegerResult(0, 0x100, 0x100))),
    ("lane-local ABS invalid", "float", "abs", (0x3F80000000, 0xFFC0000000), (0, 0),
     {"mode": Mode(40)}, (IntegerResult(0x3F80000000, 0x400, 0),
                          IntegerResult(0xFFFFFFFFFF, 0x430, 0x20))),
    ("distinct old lane flags survive independently", "fixed", "pass",
     (0x00000001FF, 0xFFFFFFFFFF), (0, 0), {"astat": (0xFFFF, 0), "stky": (4, 0x20)},
     (IntegerResult(0x0000000100, 0xFBC0, 4), IntegerResult(0xFFFFFFFF00, 4, 0x20))),
]


def record_value(value):
    if is_dataclass(value):
        value = asdict(value)
    if isinstance(value, dict):
        return {key: f"0x{item:020x}" if key == "accumulator" and item is not None else record_value(item)
                for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [record_value(item) for item in value]
    return value


def anchors():
    records = []
    for name, kind, left, right, options, expected in DUAL:
        require(dual_alu(kind, left, right, **options), expected, name)
        records.append(dict(name=name, kind="dual-alu", format=kind, left=left, right=right,
                            options=options, expected=expected))
    for name, kind, inputs, destinations, options, expected in PACKETS:
        require(parallel_compute(kind, *inputs, **options), expected, name)
        records.append(dict(name=name, kind="multifunction", format=kind,
                            registers=dict(zip((0, 4, 8, 12), inputs)), destinations=destinations,
                            options=options, expected=expected))
    for name, kind, operation, left, right, options, expected in LANES:
        actual = []
        for lane, (x, y) in enumerate(zip(left, right)):
            lane_options = {key: value[lane] if key in ("astat", "stky") else value
                            for key, value in options.items()}
            if kind == "fixed":
                actual.append(fixed(operation, x, y, **lane_options))
            elif kind == "float":
                actual.append(floating_alu(operation, x, y, **lane_options))
            else:
                actual.append(floating_multiply(x, y, **lane_options))
        require(tuple(actual), expected, name)
        records.append(dict(name=name, kind="simd-numeric", format=kind, operation=operation,
                            left=left, right=right, options=options, expected=expected))
    return [record_value(record) for record in records]


def _fixed_alu(operation, x, y, saturate, truncate):
    """Expected overflow from sign-bit identities, not the model's numeric bounds."""
    if operation == "dual":
        a, af, ast = _fixed_alu("add", x, y, saturate, truncate)
        s, sf, sst = _fixed_alu("sub", x, y, saturate, truncate)
        return (*a, *s), af | sf, ast | sst
    if operation == "avg":
        sx, sy = (word - 4294967296 if word >= 2147483648 else word for word in (x, y))
        value, remainder = divmod(sx + sy, 2)
        value += int(remainder != 0 and ((sx + sy < 0) if truncate else value % 2 != 0))
        value %= 4294967296
        carry, overflow = x + y >= 4294967296, False
    else:
        value = (x + y if operation == "add" else x - y) % 4294967296
        if operation == "add":
            overflow = bool(((x ^ value) & (y ^ value)) >> 31)
            carry = x + y >= 4294967296
        else:
            overflow = bool(((x ^ y) & (x ^ value)) >> 31)
            carry = x >= y
        if overflow and saturate:
            value = 2147483648 if x >= 2147483648 else 2147483647
    flags = int(value == 0) | (int(value >= 2147483648) << 2)
    flags |= (int(carry) << 3) | (int(overflow) << 1)
    return (value * 256,), flags, 4 if overflow else 0


def fixed_lattice():
    count = 0
    pairs = ((0, 0), (1, 1), (0x40000000, 0x40000000), (0x7FFFFFFF, 1),
             (0x80000000, 1), (0xFFFFFFFF, 1), (0xFFFFFFFF, 0xFFFFFFFF), (0x80000000, 0x80000000))
    shapes = [(m, d, a) for m, d in (("mul", "register"), ("mac", "register"),
                                   ("msub", "register"), ("mac", "mr"), ("msub", "mr"))
              for a in ("add", "sub", "avg")] + [("mul", "register", "dual")]
    for x, y in pairs:
        for a, b in pairs:
            for old in (0, 0xFFFFFFFF, 0x123456789ABCDEF01234):
                for multiplier, destination, alu in shapes:
                    modifier = "SSF" if destination == "mr" else "SSFR"
                    parts = _limb_product(x, y, modifier)
                    computed = _limb_accumulate(parts, old, multiplier, modifier.endswith("R"))
                    guard = computed // (1 << 63)
                    overflow = guard not in (0, 131071)
                    signed = computed - (1 << 80) if computed >= 1 << 79 else computed
                    mflags = (int(signed < 0) << 6) | (int(overflow) << 7)
                    mflags |= int(0 < abs(signed) < 1 << 32) << 8
                    product = None if destination == "mr" else (parts[2] + parts[3] * 65536) * 256
                    mr = computed if destination == "mr" else old
                    for saturate, truncate in cartesian_product((False, True), (False, True)):
                        values, aflags, sticky = _fixed_alu(alu, a, b, saturate, truncate)
                        expected = ParallelResult((product, *values), 0xFFFFF800 | mflags | aflags,
                                                  0x20 | (0x40 if overflow else 0) | sticky, mr)
                        actual = parallel_compute("fixed", x * 256 + 255, y * 256 + 255,
                                                  a * 256 + 255, b * 256 + 255, alu=alu,
                                                  multiplier=multiplier, destination=destination,
                                                  modifier=modifier, accumulator=old,
                                                  saturate=saturate, truncate=truncate,
                                                  astat=0xFFFFFFFF, stky=0x20)
                        require(actual, expected, (x, y, a, b, old, multiplier, destination, alu))
                        count += 1
    return count


def floating_lattice():
    count = 0
    for width, rounding in cartesian_product((32, 40), ("nearest", "zero")):
        for x, y in ((1, 2), (-1, 2), (4, 4), (0, 1)):
            for a, b in ((1, 1), (1, -1), (-1, 1), (4, 2), (0, 0), (2, 4), (4, -2)):
                for alu in ("add", "sub", "avg", "abs", "min", "max", "dual"):
                    outputs = {"add": (a + b,), "sub": (a - b,), "avg": (Fraction(a + b, 2),),
                               "abs": (abs(a),), "min": (min(a, b),), "max": (max(a, b),),
                               "dual": (a + b, a - b)}[alu]
                    az, an = any(value == 0 for value in outputs), any(value < 0 for value in outputs)
                    flags = 0xFFFFFC00 | int(az) | (int(an) << 2) | (int(x * y < 0) << 6)
                    flags |= 0x10 if alu == "abs" and a < 0 else 0
                    sticky = 0x20 | int(az and alu in ("add", "sub", "avg", "dual"))
                    expected = ParallelResult((encode(x * y), *(encode(value) for value in outputs)),
                                              flags, sticky, 0x1234)
                    require(parallel_compute("float", encode(x), encode(y), encode(a), encode(b),
                                             mode=Mode(width, rounding), alu=alu, astat=0xFFFFFFFF,
                                             stky=0x20, accumulator=0x1234), expected,
                            (x, y, a, b, width, rounding, alu))
                    count += 1
    return count


def explicit_refusals():
    invalid = [
        lambda: parallel_compute("fixed", 0, 0, 0, 0, modifier="SSI"),
        lambda: parallel_compute("fixed", 0, 0, 0, 0, destination="mr", modifier="SSF"),
        lambda: parallel_compute("fixed", 0, 0, 0, 0, alu="dual", multiplier="mac"),
        lambda: parallel_compute("float", 0, 0, 0, 0, mode=Mode(40), multiplier="mac"),
    ]
    for call in invalid:
        require_raises(ValueError, call, 'unsupported multifunction combination accepted')
    for call in (
        lambda: dual_alu("float", 0x8000000000, 0x8000000000, mode=Mode(40)),
        lambda: parallel_compute("float", 0, 0xBF80000000, 0, 0, mode=Mode(40)),
    ):
        require_raises(Unqualified, call, 'partial parallel result escaped an unqualified scalar result')
