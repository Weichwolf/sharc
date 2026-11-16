# SPDX-License-Identifier: GPL-3.0-only
"""Short-field values and fully specified reciprocal-seed special cases."""

from dataclasses import asdict

from reference import IntegerResult, Mode, Unqualified, float_pack_value, floating_seed


PACK = [
    ("pack positive zero field", 0, 0),
    ("pack negative zero field", 0x8000000000, 0x8000),
    ("pack one field", 0x3F80000000, 0x3800),
    ("pack negative one field", 0xBF80000000, 0xB800),
    ("pack smallest normal field", 0x3C80000000, 0x0800),
    ("pack smallest subnormal field", 0x3700000000, 1),
    ("pack half minimum flushes", 0x3680000000, 0),
    ("pack below minimum exponent flushes", 0x36FFFFFF00, 0),
    ("pack maximum finite field", 0x43FFF00000, 0x7FFF),
    ("pack positive range clipping", 0x4400000000, 0x7FFF),
    ("pack negative range clipping", 0xC400000000, 0xFFFF),
    ("pack infinity exponent field clipping", 0x7F80000000, 0x7FFF),
    ("pack NaN exponent field clipping", 0x7FC0000000, 0x7FFF),
    ("pack ignores extension field", 0x3F800000FF, 0x3800),
]

SEEDS = [
    ("RECIPS positive zero", "recips", 0, Mode(32), {}, IntegerResult(0x7F80000000, 0x402, 2)),
    ("RECIPS negative zero", "recips", 0x8000000000, Mode(40), {},
     IntegerResult(0xFF80000000, 0x406, 2)),
    ("RECIPS positive denormal", "recips", 1, Mode(40), {},
     IntegerResult(0x7F80000000, 0x402, 2)),
    ("RECIPS negative denormal", "recips", 0x8000000001, Mode(32), {},
     IntegerResult(0xFF80000000, 0x406, 2)),
    ("RECIPS positive large exponent", "recips", 0x7E80000000, Mode(40), {},
     IntegerResult(0, 0x401, 1)),
    ("RECIPS negative large exponent has input-based AN", "recips", 0xFE80000000, Mode(40), {},
     IntegerResult(0x8000000000, 0x405, 1)),
    ("RECIPS positive infinity", "recips", 0x7F80000000, Mode(32), {},
     IntegerResult(0, 0x401, 1)),
    ("RECIPS negative infinity", "recips", 0xFF80000000, Mode(40), {},
     IntegerResult(0x8000000000, 0x405, 1)),
    ("RECIPS positive NaN", "recips", 0x7F80000001, Mode(40), {},
     IntegerResult(0xFFFFFFFFFF, 0x420, 0x20)),
    ("RSQRTS positive zero", "rsqrts", 0, Mode(32), {}, IntegerResult(0x7F80000000, 0x402, 2)),
    ("RSQRTS negative zero", "rsqrts", 0x8000000000, Mode(40), {},
     IntegerResult(0xFF80000000, 0x406, 2)),
    ("RSQRTS positive infinity leaves AUS unchanged", "rsqrts", 0x7F80000000, Mode(40),
     {"stky": 0x55}, IntegerResult(0, 0x401, 0x55)),
    ("RSQRTS negative infinity", "rsqrts", 0xFF80000000, Mode(40), {},
     IntegerResult(0xFFFFFFFFFF, 0x420, 0x20)),
    ("RSQRTS negative normal", "rsqrts", 0xBF80000000, Mode(40), {},
     IntegerResult(0xFFFFFFFFFF, 0x420, 0x20)),
    ("RSQRTS negative NaN clears AN", "rsqrts", 0xFFC0000000, Mode(40), {},
     IntegerResult(0xFFFFFFFFFF, 0x420, 0x20)),
    ("RSQRTS positive NaN", "rsqrts", 0x7FC0000000, Mode(40), {},
     IntegerResult(0xFFFFFFFFFF, 0x420, 0x20)),
    ("RSQRTS negative denormal flushes before invalid check", "rsqrts", 0x8000000001, Mode(40), {},
     IntegerResult(0xFF80000000, 0x406, 2)),
    ("RECIPS preserves unrelated flags and old sticky bits", "recips", 0x7E80000000, Mode(32),
     {"astat": 0xFFFFFFFF, "stky": 0x24}, IntegerResult(0, 0xFFFFFFC1, 0x25)),
]


def require(actual, expected, context):
    if actual != expected:
        raise AssertionError((context, expected, actual))


def anchors():
    records = []
    for name, raw, short in PACK:
        require(float_pack_value(raw), short, name)
        records.append(dict(name=name, kind="pack-value", left=raw, expected={"short": short}))
    for name, operation, raw, mode, options, expected in SEEDS:
        require(floating_seed(operation, raw, mode, **options), expected, name)
        records.append(dict(name=name, kind="seed", operation=operation, left=raw,
                            mode=asdict(mode), options=options, expected=asdict(expected)))
    return records


def packing_lattice():
    """Construct exact IEEE inputs with strings, not the packer's numeric shifts."""
    count = 0
    for short in range(65536):
        bits = f"{short:016b}"
        sign, exponent, fraction = bits[0], bits[1:5], bits[5:]
        if int(exponent, 2):
            opposite = "0" if exponent[0] == "1" else "1"
            ieee_exponent = exponent[0] + opposite * 4 + exponent[1:]
            ieee_fraction = fraction + "0" * 12
        elif int(fraction, 2):
            significant = fraction.lstrip("0")
            leading = len(fraction) - len(significant)
            ieee_exponent = f"{120 - leading:08b}"
            ieee_fraction = significant[1:].ljust(23, "0")
        else:
            ieee_exponent, ieee_fraction = "0" * 8, "0" * 23
        ieee = int(sign + ieee_exponent + ieee_fraction, 2)
        for extension in (0, 255):
            require(float_pack_value(ieee * 256 + extension), short, short)
            count += 1
    for exponent in (*range(110), *range(136, 256)):
        for negative in (False, True):
            for fraction in (0, 1, 4095, 4096, 0x7FF000, 0x7FFFFF):
                word = int(negative) * 2147483648 + exponent * 8388608 + fraction
                short = (32768 if negative else 0) + (32767 if exponent > 135 else 0)
                require(float_pack_value(word * 256 + 165), short, (exponent, negative, fraction))
                count += 1
    refusals = 0
    for exponent in range(110, 136):
        for fraction in (1, 127, 128, 255, 4095):
            for negative in (False, True):
                word = int(negative) * 2147483648 + exponent * 8388608 + fraction
                try:
                    float_pack_value(word * 256)
                except Unqualified:
                    refusals += 1
                    continue
                raise AssertionError("unqualified FPACK rounding accepted")
    return count, refusals


def seed_lattice():
    count = 0
    # These tuples specify documented special cases and literal full effects;
    # ordinary seeds never use a reciprocal/square-root approximation here.
    cases = []
    for negative in (False, True):
        sign = 0x8000000000 if negative else 0
        for fraction in (0, 1, 255, 256, 0x7FFFFFFF):
            cases += [(operation, sign | fraction, width, 0xFF80000000 if negative else 0x7F80000000,
                       0x406 if negative else 0x402, 2)
                      for operation in ("recips", "rsqrts") for width in (32, 40)]
        for exponent in (253, 254):
            for fraction in (0, 1, 127, 128, 255, 256, 0x7FFFFFFF):
                cases += [("recips", sign | (exponent << 31) | fraction, width, sign,
                           0x405 if negative else 0x401, 1) for width in (32, 40)]
        cases += [("recips", sign | 0x7F80000000, width, sign, 0x405 if negative else 0x401, 1)
                  for width in (32, 40)]
    cases += [("rsqrts", 0x7F80000000, width, 0, 0x401, 0) for width in (32, 40)]
    for exponent in (1, 126, 127, 254, 255):
        for fraction in (0, 0x40000000):
            cases.append(("rsqrts", 0x8000000000 | (exponent << 31) | fraction, 40,
                          0xFFFFFFFFFF, 0x420, 0x20))
    for operation, raw, width, result, flags, sticky in cases:
        for rounding in ("nearest", "zero"):
            for old in (0, 0xFFFFFFFF):
                for previous in (0, 0xA5):
                    expected = IntegerResult(result, (old & ~0x43F) | flags, previous | sticky)
                    require(floating_seed(operation, raw, Mode(width, rounding), astat=old, stky=previous),
                            expected, (operation, raw, width, rounding, old, previous))
                    count += 1
    for call in (
        lambda: floating_seed("recips", 0x3F80000000, Mode(40)),
        lambda: floating_seed("rsqrts", 0x3F80000000, Mode(40)),
        lambda: floating_seed("recips", 0xFFC0000000, Mode(40)),
        lambda: floating_seed("rsqrts", 0xBF80000000, Mode(32)),
    ):
        try:
            call()
        except Unqualified:
            continue
        raise AssertionError("unqualified seed expectation generated")
    return count
