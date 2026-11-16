# SPDX-License-Identifier: GPL-3.0-only
"""Independent Strict numeric expectations, never imported by production.

This module has no instruction decoder, execution loop, LLVM dependency or host
floating-point evaluation. See doc/numeric-reference.md for its qualified scope.
"""

from dataclasses import dataclass
from fractions import Fraction


WORD_MASK = (1 << 32) - 1
RAW_MASK = (1 << 40) - 1
SIGN = 1 << 39
FRACTION_MASK = (1 << 31) - 1
INFINITY = 255 << 31
AZ, AV, AN, AC, AS, AI, AF = (1 << bit for bit in (0, 1, 2, 3, 4, 5, 10))
ALU_FLAGS = AZ | AV | AN | AC | AS | AI | AF
AOS = 1 << 2
AUS, AVS = 1, 2
AIS = 1 << 5
CACC = 255 << 24


class Unqualified(ValueError):
    """A requested expectation has not yet been justified independently."""


def raw_word(raw: int) -> int:
    if not isinstance(raw, int) or not 0 <= raw <= RAW_MASK:
        raise ValueError("a register input must contain exactly raw 40-bit storage")
    return raw >> 8


def signed(word: int) -> int:
    word &= WORD_MASK
    return word - (1 << 32) if word & (1 << 31) else word


def power(exponent: int) -> Fraction:
    return Fraction(1 << exponent) if exponent >= 0 else Fraction(1, 1 << -exponent)


def rounded_integer(value: Fraction, rounding: str) -> int:
    """Nearest ties-to-even or toward zero, with no intermediate float."""
    if rounding not in ("nearest", "zero"):
        raise ValueError("unsupported rounding direction")
    magnitude = abs(value)
    quotient, remainder = divmod(magnitude.numerator, magnitude.denominator)
    if rounding == "nearest" and (
        remainder * 2 > magnitude.denominator
        or (remainder * 2 == magnitude.denominator and quotient & 1)
    ):
        quotient += 1
    return -quotient if value < 0 else quotient


@dataclass(frozen=True)
class IntegerResult:
    """Raw register/flag effects; raw storage is integer-valued even for FP outputs."""

    raw: int | None
    astat: int
    stky: int


def fixed(operation: str, left: int, right: int = 0, *, astat: int = 0,
          stky: int = 0, saturate: bool = False, truncate: bool = False) -> IntegerResult:
    """One ALU numeric operation on raw register views, not an ISA executor.

    astat/stky are old lane flags. None means a compare writes flags only.
    Memory, predicates, parallel priority and register selection are not modeled.
    """
    if any(not isinstance(flags, int) or not 0 <= flags <= WORD_MASK
           for flags in (astat, stky)):
        raise ValueError("old flags must be raw 32-bit values")
    x, y = raw_word(left), raw_word(right)
    sx, sy = signed(x), signed(y)
    carry_in = int(bool(astat & AC))
    if operation in ("cmp", "cmpu"):
        a, b = (x, y) if operation == "cmpu" else (sx, sy)
        flags = (AZ if a == b else 0) | (AN if a < b else 0)
        history = ((astat >> 1) & (CACC >> 1) & CACC) | ((1 << 31) if a > b else 0)
        return IntegerResult(None, (astat & ~(ALU_FLAGS | CACC)) | history | flags, stky)

    carry, overflow, input_negative = False, False, False
    arithmetic = {
        "add": (sx + sy, x + y >= 1 << 32),
        "adc": (sx + sy + carry_in, x + y + carry_in >= 1 << 32),
        "sub": (sx - sy, x >= y),
        "sbc": (sx - sy + carry_in - 1, x - y + carry_in - 1 >= 0),
        "addc": (sx + carry_in, x + carry_in >= 1 << 32),
        "subc": (sx + carry_in - 1, x + carry_in - 1 >= 0),
        "inc": (sx + 1, x + 1 >= 1 << 32),
        "dec": (sx - 1, x >= 1),
        "neg": (-sx, x == 0),
        "abs": (abs(sx), False),
    }
    if operation in arithmetic:
        value, carry = arithmetic[operation]
        overflow = not -(1 << 31) <= value < 1 << 31
        if saturate:
            value = min(max(value, -(1 << 31)), (1 << 31) - 1)
        input_negative = operation == "abs" and sx < 0
    elif operation == "avg":
        value = rounded_integer(Fraction(sx + sy, 2), "zero" if truncate else "nearest")
        carry = x + y >= 1 << 32
    elif operation == "pass":
        value = sx
    elif operation == "and":
        value = x & y
    elif operation == "or":
        value = x | y
    elif operation == "xor":
        value = x ^ y
    elif operation == "not":
        value = ~x
    elif operation == "min":
        value = min(sx, sy)
    elif operation == "max":
        value = max(sx, sy)
    elif operation == "clip":
        value = min(abs(sx), abs(sy)) * (-1 if sx < 0 else 1)
    else:
        raise Unqualified(f"unmodeled fixed operation: {operation}")
    word = value & WORD_MASK
    flags = (AZ if word == 0 else 0) | (AN if word >> 31 else 0)
    flags |= (AC if carry else 0) | (AV if overflow else 0) | (AS if input_negative else 0)
    # ABS explicitly leaves AOS unchanged even when its AV result is set (11-14).
    sticky = AOS if overflow and operation != "abs" else 0
    return IntegerResult(word << 8, (astat & ~ALU_FLAGS) | flags, stky | sticky)


@dataclass(frozen=True)
class Mode:
    precision: int
    rounding: str = "nearest"

    def __post_init__(self):
        if (type(self.precision) is not int or self.precision not in (32, 40)
                or self.rounding not in ("nearest", "zero")):
            raise ValueError("Strict precision must be 32/40; rounding nearest/zero")


@dataclass(frozen=True)
class FloatingResult:
    """Numeric value and exceptions; instruction-specific ASTAT/STKY are separate."""

    raw: int
    overflow: bool = False
    underflow: bool = False
    invalid: bool = False


@dataclass(frozen=True)
class Operand:
    raw: int

    def __post_init__(self):
        raw_word(self.raw)

    @property
    def negative(self) -> bool:
        return bool(self.raw & SIGN)

    @property
    def exponent(self) -> int:
        return (self.raw >> 31) & 255

    @property
    def fraction(self) -> int:
        return self.raw & FRACTION_MASK

    @property
    def kind(self) -> str:
        if self.exponent == 255:
            return "nan" if self.fraction else "infinity"
        if self.exponent == 0:
            return "denormal" if self.fraction else "zero"
        return "normal"

    def number(self) -> Fraction:
        if self.kind in ("nan", "infinity"):
            raise ValueError("a nonfinite input is not an exact rational")
        if self.exponent == 0:
            return Fraction(0)  # SHARC flushes computational denormals, preserving sign separately.
        magnitude = Fraction((1 << 31) + self.fraction) * power(self.exponent - 158)
        return -magnitude if self.negative else magnitude


def operand(raw: int, mode: Mode) -> Operand:
    original = Operand(raw)
    if mode.precision == 32:
        if original.kind == "nan" and original.fraction < 256:
            raise Unqualified("RND32 NaN classification confined to extension bits")
        raw &= RAW_MASK ^ 255  # Strict RND32 truncates inputs; it does not round them.
    return Operand(raw)


def quantize(value: Fraction, mode: Mode, *, negative_zero: bool = False,
             scale: int = 0) -> FloatingResult:
    """Round a rational times 2**scale within the qualified numeric domain.

    Extreme signed 32-bit scale factors take constant-size exponent paths.
    The result always uses the raw 40-bit register representation.
    """
    if not isinstance(value, (int, Fraction)):
        raise TypeError("expectations must start from integers or exact fractions")
    if not isinstance(scale, int) or not -(1 << 31) <= scale < 1 << 31:
        raise ValueError("scale must be a signed 32-bit integer")
    value = Fraction(value)
    negative = value < 0 or (value == 0 and negative_zero)
    sign = SIGN if negative else 0
    if value == 0:
        return FloatingResult(sign)
    magnitude = abs(value)
    exponent = magnitude.numerator.bit_length() - magnitude.denominator.bit_length()
    if magnitude < power(exponent):
        exponent -= 1
    exponent += scale
    fraction_bits = mode.precision - 9
    if exponent > 127:
        return _overflow(sign, mode)
    if exponent < -126:
        if exponent == -127 and mode.rounding == "nearest":
            minimum = power(-126)
            if magnitude * power(scale) >= minimum - power(-126 - fraction_bits):
                raise Unqualified("nearest rounding immediately below the minimum normal")
        return FloatingResult(sign, underflow=True)
    quantum_exponent = exponent - fraction_bits
    significand = rounded_integer(magnitude * power(scale - quantum_exponent), mode.rounding)
    if significand == 1 << (fraction_bits + 1):
        significand >>= 1
        exponent += 1
    if exponent > 127:
        return _overflow(sign, mode)
    fraction = (significand - (1 << fraction_bits)) << (40 - mode.precision)
    return FloatingResult(sign | ((exponent + 127) << 31) | fraction)


def _overflow(sign: int, mode: Mode) -> FloatingResult:
    if mode.rounding == "nearest":
        return FloatingResult(sign | INFINITY, overflow=True)
    fraction = FRACTION_MASK & (RAW_MASK ^ ((1 << (40 - mode.precision)) - 1))
    return FloatingResult(sign | (254 << 31) | fraction, overflow=True)


def _invalid(mode: Mode) -> FloatingResult:
    if mode.precision == 32:
        raise Unqualified("RND32 canonical NaN extension bits require qualification")
    return FloatingResult(RAW_MASK, invalid=True)


def floating(operation: str, left: int, right: int, mode: Mode) -> FloatingResult:
    """Exact ADD/SUB/average/multiply numeric results; no host FP or FMA."""
    if operation not in ("add", "sub", "avg", "mul"):
        raise Unqualified(f"unmodeled floating operation: {operation}")
    x, y = operand(left, mode), operand(right, mode)
    if "nan" in (x.kind, y.kind):
        return _invalid(mode)
    if operation == "sub":
        y = Operand(y.raw ^ SIGN)
    if operation == "mul":
        negative = x.negative != y.negative
        if "infinity" in (x.kind, y.kind):
            other = y if x.kind == "infinity" else x
            if other.kind != "infinity" and other.number() == 0:
                return _invalid(mode)
            return FloatingResult(INFINITY | (SIGN if negative else 0))
        return quantize(x.number() * y.number(), mode, negative_zero=negative)
    if "infinity" in (x.kind, y.kind):
        if x.kind == y.kind and x.negative != y.negative:
            return _invalid(mode)
        infinite = x if x.kind == "infinity" else y
        return FloatingResult(INFINITY | (SIGN if infinite.negative else 0))
    value = x.number() + y.number()
    if operation == "avg":
        value /= 2
    return quantize(value, mode, negative_zero=x.negative and y.negative)


def float_from_fixed(raw: int, mode: Mode, *, scale: int = 0) -> FloatingResult:
    # FLOAT's 40-bit boundary is independent of MODE1.RND32 (11-39).
    return quantize(Fraction(signed(raw_word(raw))), Mode(40, mode.rounding), scale=scale)


def float_round(raw: int, mode: Mode) -> FloatingResult:
    # RND consumes the extension bits that it is asked to round (11-33).
    x = Operand(raw)
    destination = Mode(32, mode.rounding)
    if x.kind == "nan":
        return _invalid(destination)
    if x.kind == "infinity":
        return FloatingResult(x.raw)
    return quantize(x.number(), destination, negative_zero=x.negative)


def _floating_negative(raw: int) -> bool:
    value = Operand(raw)
    if value.kind == "nan" or (value.kind == "zero" and value.negative):
        raise Unqualified("floating negative status for NaN or negative zero requires qualification")
    return value.negative


def _floating_order(raw: int):
    value = Operand(raw)
    if value.kind == "infinity":
        return (0 if value.negative else 2, 0)
    return (1, value.number())


def _floating_effects(result: FloatingResult, astat: int, stky: int, *,
                      zero_sticky: bool = False, overflow_sticky: bool = True,
                      absolute: bool = False, input_sign: bool = False) -> IntegerResult:
    if any(not isinstance(flags, int) or not 0 <= flags <= WORD_MASK
           for flags in (astat, stky)):
        raise ValueError("old flags must be raw 32-bit values")
    x = Operand(result.raw)
    negative = False if absolute else _floating_negative(x.raw)
    flags = AF | (AZ if x.kind == "zero" else 0)
    flags |= AN if negative else 0
    flags |= (AV if result.overflow else 0) | (AI if result.invalid else 0)
    flags |= AS if input_sign else 0
    sticky = (AUS if zero_sticky and flags & AZ else 0)
    sticky |= (AVS if overflow_sticky and flags & AV else 0) | (AIS if flags & AI else 0)
    return IntegerResult(x.raw, (astat & ~ALU_FLAGS) | flags, stky | sticky)


def floating_alu(operation: str, left: int, right: int = 0, *, mode: Mode,
                 scale: int | None = None, astat: int = 0, stky: int = 0) -> IntegerResult:
    """Qualified full scalar ALU effects; no partial status masks are returned."""
    if scale is not None and operation not in ("scale", "float"):
        raise ValueError("a scale factor is valid only for SCALB and FLOAT")
    if scale is not None and (not isinstance(scale, int) or not -(1 << 31) <= scale < 1 << 31):
        raise ValueError("scale must be a signed 32-bit integer")
    if operation == "scale" and scale is None:
        raise ValueError("SCALB requires a scale factor")
    zero_sticky, overflow_sticky, absolute, input_sign = False, True, False, False
    if operation in ("add", "sub", "avg", "absadd", "abssub"):
        base = {"absadd": "add", "abssub": "sub"}.get(operation, operation)
        result = floating(base, left, right, mode)
        absolute = operation in ("absadd", "abssub")
        if absolute and not result.invalid:
            result = FloatingResult(result.raw & ~SIGN, result.overflow, result.underflow)
        zero_sticky, overflow_sticky = True, operation != "avg"
    elif operation == "float":
        result = float_from_fixed(left, mode, scale=0 if scale is None else scale)
        zero_sticky = overflow_sticky = scale is not None
    elif operation == "round":
        result = float_round(left, mode)
    elif operation in ("neg", "abs", "pass", "scale", "min", "max", "clip", "copysign"):
        x = operand(left, mode)
        y = operand(right, mode) if operation in ("min", "max", "clip", "copysign") else None
        if x.kind == "nan" or (y is not None and y.kind == "nan"):
            result = _invalid(mode)
        else:
            # Flush operands before bitwise selection. All surviving magnitudes
            # are monotone in their positive raw encodings, including infinity.
            a = x.raw if x.kind != "denormal" else x.raw & SIGN
            b = (y.raw if y.kind != "denormal" else y.raw & SIGN) if y is not None else 0
            if operation == "scale":
                result = (FloatingResult(a) if x.kind == "infinity" else
                          quantize(x.number(), mode, negative_zero=x.negative, scale=scale))
            else:
                if operation == "neg":
                    raw = a ^ SIGN
                elif operation == "abs":
                    raw = a & ~SIGN
                elif operation == "pass":
                    raw = a
                elif operation == "copysign":
                    raw = (a & ~SIGN) | (b & SIGN)
                elif operation == "clip":
                    raw = min(a & ~SIGN, b & ~SIGN) | (a & SIGN)
                else:
                    # Order keys make -infinity < finite < +infinity and compare
                    # signed zeros numerically equal. Resolve zero ties explicitly.
                    if (a & ~SIGN) == (b & ~SIGN) == 0:
                        raw = (a | b) if operation == "min" else (a & b)
                    else:
                        raw = (min if operation == "min" else max)((a, b), key=_floating_order)
                result = FloatingResult(raw)
        absolute = operation == "abs"
        input_sign = absolute and x.negative
        zero_sticky = operation == "scale"
    else:
        raise Unqualified(f"unmodeled floating ALU operation: {operation}")
    return _floating_effects(result, astat, stky, zero_sticky=zero_sticky,
                             overflow_sticky=overflow_sticky, absolute=absolute,
                             input_sign=input_sign)


def floating_compare(left: int, right: int, mode: Mode, *, astat: int = 0,
                     stky: int = 0) -> IntegerResult:
    if any(not isinstance(flags, int) or not 0 <= flags <= WORD_MASK
           for flags in (astat, stky)):
        raise ValueError("old flags must be raw 32-bit values")
    x, y = operand(left, mode), operand(right, mode)
    if "nan" in (x.kind, y.kind):
        raise Unqualified("unordered floating compare flags require qualification")
    a, b = _floating_order(x.raw), _floating_order(y.raw)
    flags = AF | (AZ if a == b else 0) | (AN if a < b else 0)
    history = ((astat & CACC) >> 1) & CACC
    history |= 1 << 31 if a > b else 0
    return IntegerResult(None, (astat & ~(ALU_FLAGS | CACC)) | history | flags, stky)


def float_parts(operation: str, raw: int, mode: Mode, *, saturate: bool = False,
                astat: int = 0, stky: int = 0) -> IntegerResult:
    """MANT/LOGB: mixed fixed/floating outputs with instruction-specific flags."""
    if any(not isinstance(flags, int) or not 0 <= flags <= WORD_MASK
           for flags in (astat, stky)):
        raise ValueError("old flags must be raw 32-bit values")
    if operation == "mant":
        if mode.precision == 32 and raw & 255:
            raise Unqualified("MANT extension input with RND32 requires qualification")
    x = operand(raw, mode)
    flags = AF
    if operation == "mant":
        value = ((1 << 31) | x.fraction) if x.kind == "normal" else 0
        if x.kind in ("infinity", "nan"):
            value = WORD_MASK
        flags |= AS if x.negative else 0
        flags |= AV if x.kind == "infinity" else 0
        flags |= AI if x.kind == "nan" else 0
        result = value << 8
    elif operation == "logb":
        if x.kind == "nan":
            raise Unqualified("LOGB NaN sentinel status requires qualification")
        if x.kind == "normal":
            value = x.exponent - 127
            result = (value & WORD_MASK) << 8
            flags |= AN if value < 0 else 0
        else:
            zero = x.kind != "infinity"
            result = (((0x80000000 if zero else 0x7FFFFFFF) << 8) if saturate else
                      INFINITY | (SIGN if zero else 0))
            flags |= AV | (AN if zero else 0)
    else:
        raise Unqualified(f"unmodeled float-part operation: {operation}")
    flags |= AZ if result == 0 else 0
    sticky = (AVS if flags & AV else 0) | (AIS if flags & AI else 0)
    return IntegerResult(result, (astat & ~ALU_FLAGS) | flags, stky | sticky)


def fixed_from_float(operation: str, raw: int, mode: Mode, *, scale: int = 0,
                     saturate: bool = False, astat: int = 0,
                     stky: int = 0) -> IntegerResult:
    """Qualified FIX/TRUNC result and flags, including exponent-based AV.

    The all-ones floating sentinel's AN effect remains unqualified. This function
    refuses NaNs and unsaturated overflow rather than guessing complete flags.
    """
    if operation not in ("fix", "trunc"):
        raise Unqualified(f"unmodeled float-to-fixed conversion: {operation}")
    if not isinstance(scale, int) or not -(1 << 31) <= scale < 1 << 31:
        raise ValueError("scale must be a signed 32-bit integer")
    if any(not isinstance(flags, int) or not 0 <= flags <= WORD_MASK
           for flags in (astat, stky)):
        raise ValueError("old flags must be raw 32-bit values")
    x = operand(raw, mode)
    if x.kind == "nan":
        raise Unqualified("FIX/TRUNC NaN sentinel status requires qualification")
    shifted_exponent = x.exponent + scale
    overflow_flag = x.kind == "infinity" or shifted_exponent > 157
    small = False
    if x.kind in ("zero", "denormal"):
        value = 0
    elif x.kind == "infinity" or shifted_exponent > 158:
        # This sentinel is used only for range detection, never as an output.
        value = -(1 << 32) if x.negative else 1 << 32
    elif shifted_exponent < 126:
        small = True
        value = -1 if x.negative and operation == "fix" and mode.rounding == "zero" else 0
    else:
        number = x.number() * power(scale)
        small = 0 < abs(number) < 1
        if operation == "fix" and mode.rounding == "zero":
            # Instruction-specific 11-37 overrides the general toward-zero rule.
            value = number.numerator // number.denominator
        else:
            value = rounded_integer(number, "zero" if operation == "trunc" else "nearest")
    if not -(1 << 31) <= value < 1 << 31:
        if not saturate:
            raise Unqualified("FIX/TRUNC unsaturated overflow sentinel status requires qualification")
        value = -(1 << 31) if value < 0 else (1 << 31) - 1
    flags = AF | (AZ if value == 0 else 0) | (AN if value < 0 else 0)
    flags |= AV if overflow_flag else 0
    sticky = (AUS if small else 0) | (AVS if overflow_flag else 0)
    return IntegerResult((value & WORD_MASK) << 8,
                         (astat & ~ALU_FLAGS) | flags, stky | sticky)


SV, SZ, SS, SF = (1 << bit for bit in (11, 12, 13, 14))
SHIFTER_FLAGS = SV | SZ | SS


def _shifter_result(value: int, astat: int, stky: int, *, overflow: bool = False,
                    negative: bool = False, write: bool = True) -> IntegerResult:
    if any(not isinstance(flags, int) or not 0 <= flags <= WORD_MASK
           for flags in (astat, stky)):
        raise ValueError("old flags must be raw 32-bit values")
    value &= WORD_MASK
    flags = (SV if overflow else 0) | (SZ if value == 0 else 0) | (SS if negative else 0)
    return IntegerResult(value << 8 if write else None, (astat & ~SHIFTER_FLAGS) | flags, stky)


def shift(operation: str, raw: int, amount: int, *, astat: int = 0,
          stky: int = 0) -> IntegerResult:
    """Normalized signed shift/bit position; operand projection is preparation's job."""
    if not isinstance(amount, int) or not -(1 << 31) <= amount < 1 << 31:
        raise ValueError("shift amount must be a signed 32-bit integer")
    x = raw_word(raw)
    overflow = False
    if operation in ("lshift", "ashift"):
        overflow = amount > 0  # Independent of whether any nonzero bit is lost.
        if amount >= 32:
            value = 0
        elif amount >= 0:
            value = x << amount
        else:
            value = (signed(x) if operation == "ashift" else x) >> min(-amount, 32)
    elif operation == "rot":
        count = amount % 32
        value = (x << count) | (x >> (32 - count))
    elif operation in ("bclr", "bset", "btgl", "btst"):
        if amount < 0 and operation == "btst":
            raise Unqualified("negative BTST position has no qualified SZ expectation")
        overflow = amount > 31
        bit = 1 << amount if 0 <= amount < 32 else 0
        value = {"bclr": x & ~bit, "bset": x | bit,
                 "btgl": x ^ bit, "btst": x & bit}[operation]
    else:
        raise Unqualified(f"unmodeled shifter operation: {operation}")
    return _shifter_result(value, astat, stky, overflow=overflow, write=operation != "btst")


def field(operation: str, raw: int, position: int, length: int, *,
          sign_extend: bool = False, old_destination: int = 0,
          astat: int = 0, stky: int = 0) -> IntegerResult:
    """FDEP/FEXT with normalized unsigned six-bit position and length."""
    if any(not isinstance(value, int) or not 0 <= value < 64 for value in (position, length)):
        raise ValueError("field position and length must be unsigned six-bit values")
    x, old = raw_word(raw), raw_word(old_destination)
    if sign_extend and length == 0:
        raise Unqualified("sign extension of a zero-length field requires qualification")
    mask = (1 << length) - 1
    if operation in ("deposit", "deposit_or"):
        value = (x & mask) << position
        sign = (value >> (position + length - 1)) & 1 if length else 0
        if sign_extend and sign and position + length <= 32:
            value |= WORD_MASK ^ ((1 << (position + length)) - 1)
        if operation == "deposit_or":
            value |= old
    elif operation == "extract":
        value = (x >> position) & mask
        if sign_extend and position + length <= 32 and value & (1 << (length - 1)):
            value |= WORD_MASK ^ mask
    else:
        raise Unqualified(f"unmodeled field operation: {operation}")
    return _shifter_result(value, astat, stky, overflow=position + length > 32)


def leading(operation: str, raw: int, *, astat: int = 0, stky: int = 0) -> IntegerResult:
    x = raw_word(raw)
    sign = bool(x & (1 << 31))
    if operation in ("exp", "exp_extended"):
        overflow = operation == "exp_extended" and bool(astat & AV)
        count = 32 - ((x ^ WORD_MASK) if sign else x).bit_length()
        value = 1 if overflow else 1 - count
        return _shifter_result(value, astat, stky, negative=sign != overflow)
    if operation not in ("leftz", "lefto"):
        raise Unqualified(f"unmodeled leading-bit operation: {operation}")
    value = 32 - (x if operation == "leftz" else x ^ WORD_MASK).bit_length()
    return _shifter_result(value, astat, stky, overflow=value == 32)


@dataclass(frozen=True)
class FifoResult:
    fifo: int
    pointer: int
    raw: int | None
    astat: int
    stky: int


def bit_fifo(operation: str, fifo: int, pointer: int, *, raw: int = 0,
             length: int = 0, no_update: bool = False, astat: int = 0,
             stky: int = 0) -> FifoResult:
    """Bit FIFO numeric effects; undefined data/pointer results are refused."""
    if not isinstance(fifo, int) or not 0 <= fifo < 1 << 64:
        raise ValueError("bit FIFO must be raw 64-bit storage")
    if not isinstance(pointer, int) or not 0 <= pointer < 128:
        raise ValueError("bit FIFO pointer must be raw seven-bit storage")
    if no_update and operation != "extract":
        raise ValueError("NU applies only to BITEXT")
    x = raw_word(raw)
    result, overflow = None, False
    if operation == "read_pointer":
        result = pointer << 8
    elif operation == "write_pointer":
        pointer = x & 127
        overflow = pointer > 64
    elif operation in ("deposit", "extract"):
        if not isinstance(length, int) or not 0 <= length <= 32 or pointer > 64:
            raise Unqualified("bit FIFO length/state has undefined result data or pointer")
        if operation == "deposit":
            if pointer + length > 64:
                raise Unqualified("bit FIFO deposit exceeds available capacity")
            pointer += length
            fifo |= (x & ((1 << length) - 1)) << (64 - pointer)
        else:
            if length > pointer:
                raise Unqualified("bit FIFO extract exceeds available data")
            result = (fifo >> (64 - length)) << 8
            if not no_update:
                fifo = (fifo << length) & ((1 << 64) - 1)
                pointer -= length
    else:
        raise Unqualified(f"unmodeled bit FIFO operation: {operation}")
    flags = _shifter_result(1, astat, stky, overflow=overflow).astat
    if operation == "extract" and result == 0:
        flags |= SZ
    if operation != "read_pointer" and not no_update:
        flags = (flags & ~SF) | (SF if pointer >= 32 else 0)
    return FifoResult(fifo, pointer, result, flags, stky)


def float_unpack(raw: int, *, astat: int = 0, stky: int = 0) -> IntegerResult:
    """Exact short-float to binary32 conversion, performed by the shifter."""
    short = raw_word(raw) & 65535
    exponent, fraction = (short >> 11) & 15, short & 2047
    if exponent:
        magnitude = Fraction(2048 + fraction) * power(exponent - 18)
    else:
        magnitude = Fraction(fraction, 1 << 17)
    value = quantize(-magnitude if short & 32768 else magnitude, Mode(32),
                     negative_zero=bool(short & 32768)).raw
    flags = _shifter_result(1, astat, stky).astat  # SZ is cleared even for ±zero.
    return IntegerResult(value, flags, stky)


ACCUMULATOR_MASK = (1 << 80) - 1
MN, MV, MU, MI = (1 << bit for bit in (6, 7, 8, 9))
MULTIPLIER_FLAGS = MN | MV | MU | MI
MOS = 1 << 6


def floating_multiply(left: int, right: int, mode: Mode, *, astat: int = 0,
                      stky: int = 0) -> IntegerResult:
    if any(not isinstance(flags, int) or not 0 <= flags <= WORD_MASK
           for flags in (astat, stky)):
        raise ValueError("old flags must be raw 32-bit values")
    result = floating("mul", left, right, mode)
    flags = (MN if _floating_negative(result.raw) else 0)
    flags |= (MV if result.overflow else 0) | (MU if result.underflow else 0)
    flags |= MI if result.invalid else 0
    # Floating multiplication preserves every ALU bit, including AF.
    sticky = (1 << 7 if result.overflow else 0) | (1 << 8 if result.underflow else 0)
    sticky |= 1 << 9 if result.invalid else 0
    return IntegerResult(result.raw, (astat & ~MULTIPLIER_FLAGS) | flags, stky | sticky)


@dataclass(frozen=True)
class MultiplierResult:
    accumulator: int
    raw: int | None
    astat: int
    stky: int


def _accumulator_input(accumulator: int, astat: int, stky: int):
    if not isinstance(accumulator, int) or not 0 <= accumulator <= ACCUMULATOR_MASK:
        raise ValueError("accumulator must be raw 80-bit storage")
    if any(not isinstance(flags, int) or not 0 <= flags <= WORD_MASK
           for flags in (astat, stky)):
        raise ValueError("old flags must be raw 32-bit values")


def _signed_accumulator(accumulator: int) -> int:
    return accumulator - (1 << 80) if accumulator & (1 << 79) else accumulator


def _round_accumulator(accumulator: int) -> int:
    # Rounding is a separate requested operation. Reading MR1 without rounding
    # simply discards MR0, including for negative fixed-point values (3-14/16).
    return (rounded_integer(Fraction(accumulator, 1 << 32), "nearest") << 32) & ACCUMULATOR_MASK


def _multiplier_result(value: int, old: int, destination: str, fractional: bool,
                       signed_result: bool, astat: int, stky: int, *,
                       saturation: bool = False) -> MultiplierResult:
    if destination not in ("mr", "register"):
        raise ValueError("multiplier destination must be mr or register")
    value &= ACCUMULATOR_MASK
    number = _signed_accumulator(value) if signed_result else value
    width = 64 if fractional else 32
    lower = -(1 << (width - 1)) if signed_result else 0
    upper = (1 << (width - int(signed_result))) - 1
    overflow = not lower <= number <= upper
    if saturation:
        overflow = False
    high = value >> 32
    underflow = fractional and bool(value & WORD_MASK) and (
        high == 0 or (signed_result and high == (1 << 48) - 1)
    )
    flags = (MN if value >> 79 else 0) | (MV if overflow else 0) | (MU if underflow else 0)
    # Instruction-specific rules leave MUS/MVS/MIS unchanged for fixed operations.
    sticky = MOS if overflow and not saturation else 0
    raw = ((value >> (32 if fractional else 0)) & WORD_MASK) << 8
    return MultiplierResult(value if destination == "mr" else old,
                            None if destination == "mr" else raw,
                            (astat & ~MULTIPLIER_FLAGS) | flags, stky | sticky)


def multiply(operation: str, left: int, right: int, *, modifier: str = "SSF",
             accumulator: int = 0, destination: str = "mr", astat: int = 0,
             stky: int = 0) -> MultiplierResult:
    """Fixed multiply/MAC/MSUB with old raw accumulator and independent modifiers.

    A register destination leaves MR unchanged. Accumulator arithmetic wraps at
    80 bits; only SS fractional products have the extra left shift (3-14).
    """
    _accumulator_input(accumulator, astat, stky)
    rounded = modifier.endswith("R")
    base = modifier[:-1] if rounded else modifier
    if (len(base) != 3 or base[0] not in "SU" or base[1] not in "SU"
            or base[2] not in "IF" or (rounded and base[2] != "F")):
        raise ValueError("invalid fixed multiplier modifier")
    x, y = raw_word(left), raw_word(right)
    if base[0] == "S":
        x = signed(x)
    if base[1] == "S":
        y = signed(y)
    product = x * y
    fractional = base[2] == "F"
    if fractional and base[:2] == "SS":
        product *= 2
    if operation == "mul":
        value = product
    elif operation == "mac":
        value = accumulator + product
    elif operation == "msub":
        value = accumulator - product
    else:
        raise Unqualified(f"unmodeled multiplier operation: {operation}")
    value &= ACCUMULATOR_MASK
    if rounded:
        value = _round_accumulator(value)
    return _multiplier_result(value, accumulator, destination, fractional, "S" in base,
                              astat, stky)


def accumulator_unary(operation: str, accumulator: int, *, format: str = "SF",
                      destination: str = "mr", astat: int = 0,
                      stky: int = 0) -> MultiplierResult:
    """Explicit SAT/RND/clear; the format selects signedness and binary point."""
    _accumulator_input(accumulator, astat, stky)
    if operation == "clear":
        if destination != "mr":
            raise ValueError("accumulator clear has no register destination")
        return MultiplierResult(0, None, astat & ~MULTIPLIER_FLAGS, stky)
    if format not in ("SI", "UI", "SF", "UF"):
        raise ValueError("invalid accumulator format")
    fractional, signed_result = format[1] == "F", format[0] == "S"
    if operation == "round":
        if not fractional:
            raise ValueError("explicit RND requires fractional format")
        value = _round_accumulator(accumulator)
    elif operation == "sat":
        width = 64 if fractional else 32
        number = _signed_accumulator(accumulator) if signed_result else accumulator
        lower = -(1 << (width - 1)) if signed_result else 0
        upper = (1 << (width - int(signed_result))) - 1
        value = min(max(number, lower), upper) & ACCUMULATOR_MASK
    else:
        raise Unqualified(f"unmodeled accumulator operation: {operation}")
    return _multiplier_result(value, accumulator, destination, fractional, signed_result,
                              astat, stky, saturation=operation == "sat")


def accumulator_move(direction: str, part: int, accumulator: int, *, register: int = 0,
                     astat: int = 0, stky: int = 0) -> MultiplierResult:
    """MR0/MR1/MR2 transfers; MR1 writes sign-extend to MR2 (3-15)."""
    _accumulator_input(accumulator, astat, stky)
    if part not in (0, 1, 2):
        raise ValueError("accumulator part must be MR0, MR1 or MR2")
    word = raw_word(register)
    raw = None
    if direction == "write":
        if part == 0:
            accumulator = (accumulator & (ACCUMULATOR_MASK ^ WORD_MASK)) | word
        elif part == 1:
            accumulator = ((signed(word) << 32) | (accumulator & WORD_MASK)) & ACCUMULATOR_MASK
        else:
            accumulator = ((word & 0xFFFF) << 64) | (accumulator & ((1 << 64) - 1))
    elif direction == "read":
        word = (accumulator >> (32 * part)) & (0xFFFF if part == 2 else WORD_MASK)
        if part == 2 and word & 0x8000:
            word |= 0xFFFF0000
        raw = word << 8
    else:
        raise ValueError("accumulator transfer must read or write")
    return MultiplierResult(accumulator, raw, astat & ~MULTIPLIER_FLAGS, stky)
