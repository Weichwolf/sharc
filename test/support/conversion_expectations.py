# SPDX-License-Identifier: GPL-3.0-only
"""Authored FIX/TRUNC anchors and direct signed-quotient cross-checks."""

from dataclasses import asdict

from reference import IntegerResult, Mode, Unqualified, fixed_from_float


CONVERSIONS = [
    ("FIX positive odd tie", "fix", 0x3FC0000000, Mode(40), {},
     IntegerResult(0x0000000200, 0x400, 0)),
    ("FIX positive even tie", "fix", 0x4020000000, Mode(40), {},
     IntegerResult(0x0000000200, 0x400, 0)),
    ("FIX negative tie", "fix", 0xBFC0000000, Mode(40), {},
     IntegerResult(0xFFFFFFFE00, 0x404, 0)),
    ("FIX TRUNC bit floors", "fix", 0xBFA0000000, Mode(40, "zero"), {},
     IntegerResult(0xFFFFFFFE00, 0x404, 0)),
    ("TRUNC ignores clear mode bit", "trunc", 0xBFA0000000, Mode(40), {},
     IntegerResult(0xFFFFFFFF00, 0x404, 0)),
    ("TRUNC ignores set mode bit", "trunc", 0xBFA0000000, Mode(40, "zero"), {},
     IntegerResult(0xFFFFFFFF00, 0x404, 0)),
    ("negative half nearest zero with AUS", "fix", 0xBF00000000, Mode(40), {},
     IntegerResult(0, 0x401, 1)),
    ("negative half floor with AUS", "fix", 0xBF00000000, Mode(40, "zero"), {},
     IntegerResult(0xFFFFFFFF00, 0x404, 1)),
    ("negative half TRUNC with AUS", "trunc", 0xBF00000000, Mode(40), {},
     IntegerResult(0, 0x401, 1)),
    ("positive fraction rounds to one with AUS", "fix", 0x3F40000000, Mode(40), {},
     IntegerResult(0x0000000100, 0x400, 1)),
    ("negative one does not set AUS", "fix", 0xBF80000000, Mode(40), {},
     IntegerResult(0xFFFFFFFF00, 0x404, 0)),
    ("negative denormal flush does not set AUS", "fix", 0x8000000001, Mode(40, "zero"), {},
     IntegerResult(0, 0x401, 0)),
    ("negative zero converts to integer zero", "trunc", 0x8000000000, Mode(32), {},
     IntegerResult(0, 0x401, 0)),
    ("RND32 truncates conversion input extension", "fix", 0x3F00000001, Mode(32), {},
     IntegerResult(0, 0x401, 1)),
    ("extended conversion retains input extension", "fix", 0x3F00000001, Mode(40), {},
     IntegerResult(0x0000000100, 0x400, 1)),
    ("scaled conversion", "fix", 0x3FC0000000, Mode(40), {"scale": 1},
     IntegerResult(0x0000000300, 0x400, 0)),
    ("scale precedes rounding", "fix", 0x4020000000, Mode(40), {"scale": -1},
     IntegerResult(0x0000000100, 0x400, 0)),
    ("exact positive maximum", "fix", 0x4EFFFFFFFE, Mode(40), {},
     IntegerResult(0x7FFFFFFF00, 0x400, 0)),
    ("representable negative minimum sets AV", "fix", 0xCF00000000, Mode(40), {},
     IntegerResult(0x8000000000, 0x406, 2)),
    ("rounding overflow with AV clear saturates", "fix", 0x4EFFFFFFFF, Mode(40),
     {"saturate": True}, IntegerResult(0x7FFFFFFF00, 0x400, 0)),
    ("TRUNC below overflow", "trunc", 0x4EFFFFFFFF, Mode(40), {},
     IntegerResult(0x7FFFFFFF00, 0x400, 0)),
    ("positive overflow saturates", "fix", 0x4F00000000, Mode(40), {"saturate": True},
     IntegerResult(0x7FFFFFFF00, 0x402, 2)),
    ("positive infinity saturates", "fix", 0x7F80000000, Mode(32), {"saturate": True},
     IntegerResult(0x7FFFFFFF00, 0x402, 2)),
    ("negative infinity saturates", "trunc", 0xFF80000000, Mode(40), {"saturate": True},
     IntegerResult(0x8000000000, 0x406, 2)),
    ("extreme positive conversion scale", "fix", 0x3F80000000, Mode(40),
     {"scale": 0x7FFFFFFF, "saturate": True}, IntegerResult(0x7FFFFFFF00, 0x402, 2)),
    ("extreme negative scale floor", "fix", 0xBF80000000, Mode(40, "zero"),
     {"scale": -0x80000000}, IntegerResult(0xFFFFFFFF00, 0x404, 1)),
    ("extreme negative scale TRUNC", "trunc", 0xBF80000000, Mode(40, "zero"),
     {"scale": -0x80000000}, IntegerResult(0, 0x401, 1)),
    ("conversion preserves unrelated and sticky flags", "fix", 0x3FC0000000, Mode(40),
     {"astat": 0xFFFFFFFF, "stky": 0xFFFFFFFF},
     IntegerResult(0x0000000200, 0xFFFFFFC0, 0xFFFFFFFF)),
]


def anchors():
    records = []
    for name, operation, raw, mode, options, expected in CONVERSIONS:
        actual = fixed_from_float(operation, raw, mode, **options)
        if actual != expected:
            raise AssertionError(f"{name}: expected {expected}, got {actual}")
        records.append({"name": name, "kind": "conversion", "operation": operation,
                        "left": raw, "mode": asdict(mode), "options": options,
                        "expected": asdict(expected)})
    return records


def quotient_lattice():
    """Use signed floor division and remainder, separately from absolute rounding.

    Build input rationals directly from authored exponent/fraction fields. No
    reference operand classification, normalization or rounding helper is used.
    """
    count, refusals = 0, 0
    for exponent in (1, 125, 126, 127, 128, 156, 157, 158, 159, 254):
        for fraction in (0, 1, 127, 128, 255, 256, 0x7FFFFF00, 0x7FFFFFFF):
            for width in (32, 40):
                mantissa = (1 << 31) + (fraction if width == 40 else fraction & ~255)
                for scale in (-2, -1, 0, 1, 2):
                    shift = exponent - 158 + scale
                    numerator = mantissa << max(shift, 0)
                    denominator = 1 << max(-shift, 0)
                    for negative in (False, True):
                        number = -numerator if negative else numerator
                        raw = (int(negative) << 39) | (exponent << 31) | fraction
                        for rounding in ("nearest", "zero"):
                            for operation in ("fix", "trunc"):
                                quotient, remainder = divmod(number, denominator)
                                if operation == "trunc":
                                    quotient += int(negative and remainder != 0)
                                elif rounding == "nearest":
                                    quotient += int(2 * remainder > denominator or (
                                        2 * remainder == denominator and quotient & 1))
                                for saturate in (False, True):
                                    options = dict(scale=scale, saturate=saturate,
                                                   astat=0xFFFFFFFF, stky=0x100)
                                    overflow = not -(1 << 31) <= quotient < 1 << 31
                                    if overflow and not saturate:
                                        try:
                                            fixed_from_float(operation, raw, Mode(width, rounding),
                                                             **options)
                                        except Unqualified:
                                            refusals += 1
                                            continue
                                        raise AssertionError("unqualified conversion accepted")
                                    value = min(max(quotient, -(1 << 31)), (1 << 31) - 1)
                                    av = exponent + scale > 157
                                    flags = 0xFFFFFFC0 | int(value == 0) | (int(av) << 1)
                                    flags |= int(value < 0) << 2
                                    sticky = 0x100 | int(0 < abs(number) < denominator) | (int(av) << 1)
                                    expected = IntegerResult((value & 0xFFFFFFFF) << 8, flags, sticky)
                                    actual = fixed_from_float(operation, raw, Mode(width, rounding),
                                                              **options)
                                    if actual != expected:
                                        raise AssertionError((operation, raw, rounding, options,
                                                              expected, actual))
                                    count += 1
    return count, refusals
