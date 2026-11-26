# SPDX-License-Identifier: GPL-3.0-only
"""Authored accumulator anchors and independent limb-based expectation checks."""

from itertools import product as cartesian_product
from dataclasses import asdict
import json

from expectation_check import anchor, require, require_raises
from reference import (
    MultiplierResult, accumulator_move, accumulator_unary, multiply,
)


MULTIPLIES = [
    ("signed integer negative", "mul", 0x0000000200, 0xFFFFFFFD00, {"modifier": "SSI"},
     MultiplierResult(0xFFFFFFFFFFFFFFFFFFFA, None, 0x40, 0)),
    ("unsigned product overflow to register", "mul", 0xFFFFFFFF00, 0x0000000200,
     {"modifier": "UUI", "accumulator": 0x1234, "destination": "register"},
     MultiplierResult(0x1234, 0xFFFFFFFE00, 0x80, 0x40)),
    ("signed unsigned integer", "mul", 0xFFFFFFFF00, 0xFFFFFFFF00, {"modifier": "SUI"},
     MultiplierResult(0xFFFFFFFFFFFF00000001, None, 0xC0, 0x40)),
    ("unsigned signed integer", "mul", 0xFFFFFFFF00, 0xFFFFFFFF00, {"modifier": "USI"},
     MultiplierResult(0xFFFFFFFFFFFF00000001, None, 0xC0, 0x40)),
    ("signed fractional half squared", "mul", 0x4000000000, 0x4000000000, {},
     MultiplierResult(0x00002000000000000000, None, 0, 0)),
    ("unsigned fractional half squared", "mul", 0x8000000000, 0x8000000000, {"modifier": "UUF"},
     MultiplierResult(0x00004000000000000000, None, 0, 0)),
    ("minus one squared overflows signed fraction", "mul", 0x8000000000, 0x8000000000, {},
     MultiplierResult(0x00008000000000000000, None, 0x80, 0x40)),
    ("mixed fractional has no redundant-sign shift", "mul", 0x8000000000, 0x8000000000,
     {"modifier": "SUF"}, MultiplierResult(0xFFFFC000000000000000, None, 0x40, 0)),
    ("unsigned fractional maximum squared", "mul", 0xFFFFFFFF00, 0xFFFFFFFF00,
     {"modifier": "UUF"}, MultiplierResult(0x0000FFFFFFFE00000001, None, 0, 0)),
    ("positive fractional underflow leaves MUS clear", "mul", 0x0000000100, 0x0000000100, {},
     MultiplierResult(2, None, 0x100, 0)),
    ("negative fractional underflow leaves MUS clear", "mul", 0xFFFFFFFF00, 0x0000000100, {},
     MultiplierResult(0xFFFFFFFFFFFFFFFFFFFE, None, 0x140, 0)),
    ("discarding negative fraction is not a zero-directed cast", "mul", 0xFFFFFFFF00,
     0x0000000100, {"destination": "register"}, MultiplierResult(0, 0xFFFFFFFF00, 0x140, 0)),
    ("rounded negative tiny product", "mul", 0xFFFFFFFF00, 0x0000000100,
     {"modifier": "SSFR"}, MultiplierResult(0, None, 0, 0)),
    ("rounded even tie", "mul", 0x0000000100, 0x4000000000,
     {"modifier": "SSFR"}, MultiplierResult(0, None, 0, 0)),
    ("rounded odd tie", "mul", 0x0000000300, 0x4000000000,
     {"modifier": "SSFR"}, MultiplierResult(0x0000000200000000, None, 0, 0)),
    ("rounded negative odd tie", "mul", 0xFFFFFFFD00, 0x4000000000,
     {"modifier": "SSFR"}, MultiplierResult(0xFFFFFFFFFFFE00000000, None, 0x40, 0)),
    ("MAC low-limb carry preserves unrelated flags", "mac", 0x0000000100, 0x0000000100,
     {"modifier": "UUI", "accumulator": 0xFFFFFFFF, "astat": 0xFFFFFFFF, "stky": 0x200},
     MultiplierResult(0x00000000000100000000, None, 0xFFFFFCBF, 0x240)),
    ("MAC wraps the complete 80-bit accumulator", "mac", 0x0000000200, 0x0000000100,
     {"modifier": "SSI", "accumulator": 0xFFFFFFFFFFFFFFFFFFFF},
     MultiplierResult(1, None, 0, 0)),
    ("MSUB signed result", "msub", 0x0000000200, 0x0000000100,
     {"modifier": "SSI", "accumulator": 1},
     MultiplierResult(0xFFFFFFFFFFFFFFFFFFFF, None, 0x40, 0)),
    ("MAC guard sign changes only at bit 79", "mac", 0x0000000100, 0x0000000100,
     {"modifier": "SSI", "accumulator": 0x7FFFFFFFFFFFFFFFFFFF},
     MultiplierResult(0x80000000000000000000, None, 0xC0, 0x40)),
]

UNARY = [
    ("saturate positive signed fraction", "sat", 0x00008000000000000000, {},
     MultiplierResult(0x00007FFFFFFFFFFFFFFF, None, 0, 0)),
    ("saturate negative signed fraction", "sat", 0xFFFF7FFFFFFFFFFFFFFF, {},
     MultiplierResult(0xFFFF8000000000000000, None, 0x40, 0)),
    ("saturate positive signed integer", "sat", 0x80000000, {"format": "SI"},
     MultiplierResult(0x7FFFFFFF, None, 0, 0)),
    ("saturate negative signed integer", "sat", 0xFFFFFFFFFFFF7FFFFFFF, {"format": "SI"},
     MultiplierResult(0xFFFFFFFFFFFF80000000, None, 0x40, 0)),
    ("saturate unsigned fraction", "sat", 0x00010000000000000000, {"format": "UF"},
     MultiplierResult(0x0000FFFFFFFFFFFFFFFF, None, 0, 0)),
    ("saturate unsigned integer", "sat", 0x100000000, {"format": "UI"},
     MultiplierResult(0xFFFFFFFF, None, 0, 0)),
    ("SAT retains a valid negative sub-word fraction", "sat", 0xFFFFFFFFFFFFFFFFFFFF, {},
     MultiplierResult(0xFFFFFFFFFFFFFFFFFFFF, None, 0x140, 0)),
    ("SAT to register does not replace MR", "sat", 0x100000000,
     {"format": "UI", "destination": "register", "stky": 0x200},
     MultiplierResult(0x100000000, 0xFFFFFFFF00, 0, 0x200)),
    ("RND to register does not clear old MR0", "round", 0x180000000,
     {"destination": "register"}, MultiplierResult(0x180000000, 0x0000000200, 0, 0)),
    ("RND to MR clears its discarded fraction", "round", 0x180000000, {},
     MultiplierResult(0x200000000, None, 0, 0)),
    ("clear preserves ALU and sticky flags", "clear", 0xFFFFFFFFFFFFFFFFFFFF,
     {"astat": 0xFFFFFFFF, "stky": 0xFFFFFFFF}, MultiplierResult(0, None, 0xFFFFFC3F, 0xFFFFFFFF)),
]

MOVES = [
    ("MR0 read", "read", 0, 0x123456789ABCDEF01234, {},
     MultiplierResult(0x123456789ABCDEF01234, 0xDEF0123400, 0, 0)),
    ("MR1 read", "read", 1, 0x123480000001DEADBEEF, {},
     MultiplierResult(0x123480000001DEADBEEF, 0x8000000100, 0, 0)),
    ("MR2 signed read clears multiplier flags", "read", 2, 0x800156789ABCDEF01234,
     {"astat": 0xFFFFFFFF, "stky": 0x200},
     MultiplierResult(0x800156789ABCDEF01234, 0xFFFF800100, 0xFFFFFC3F, 0x200)),
    ("MR0 write ignores extension and retains guard", "write", 0, 0x123456789ABCDEF01234,
     {"register": 0xAABBCCDD11}, MultiplierResult(0x123456789ABCAABBCCDD, None, 0, 0)),
    ("MR1 write sign extends to MR2", "write", 1, 0x123456789ABCDEF01234,
     {"register": 0x80000001FF}, MultiplierResult(0xFFFF80000001DEF01234, None, 0, 0)),
    ("MR1 positive write clears old negative guard", "write", 1, 0xFFFF56789ABCDEF01234,
     {"register": 0x0000000100}, MultiplierResult(0x000000000001DEF01234, None, 0, 0)),
    ("MR2 write truncates to its 16 physical bits", "write", 2, 0x123456789ABCDEF01234,
     {"register": 0xABCD8001FF}, MultiplierResult(0x800156789ABCDEF01234, None, 0, 0)),
]


def _record(fields):
    # Preserve all 80 bits through readers whose JSON integer type is narrower.
    return {key: (_record(value) if isinstance(value, dict)
                  else f"0x{value:020x}" if key == "accumulator" else value)
            for key, value in fields.items()}


def anchors():
    records = []
    for name, operation, left, right, options, expected in MULTIPLIES:
        records.append(anchor(name, "fixed-multiplier", multiply(operation, left, right, **options),
                              expected, operation=operation, left=left, right=right, options=options))
    for name, operation, value, options, expected in UNARY:
        records.append(anchor(name, "accumulator", accumulator_unary(operation, value, **options),
                              expected, operation=operation, accumulator=value, options=options))
    for name, direction, part, value, options, expected in MOVES:
        records.append(anchor(name, "accumulator-transfer",
                              accumulator_move(direction, part, value, **options), expected,
                              direction=direction, part=part, accumulator=value, options=options))
    return [_record(record) for record in records]


def installed_header():
    """Export authored cases with independent Table 12-5 and 12-10 encodings."""
    rows = []

    def row(name, field, bank, operands, result):
        lanes = []
        for lane, (left, right) in enumerate(operands):
            old, register, astat, stky, final = result(left, right, lane)
            def limbs(value):
                return "{" + ", ".join(f"0x{(value >> shift) & 0xffffffff:x}" for shift in (0, 32, 64)) + "}"
            lanes.append("MultiplierLane{" + ", ".join((
                f"0x{left:x}ULL", f"0x{right:x}ULL", f"0x{register:x}ULL",
                f"0x{(register if final.raw is None else final.raw):x}ULL",
                limbs(old), limbs(final.accumulator), f"0x{astat:x}", f"0x{stky:x}",
                f"0x{final.astat:x}", f"0x{final.stky:x}")) + "}")
        rows.append(f"MultiplierCase{{{json.dumps(name)}, 0x{field:x}, {bank}, "
                    + "{" + ", ".join(lanes) + "}}")

    for name, operation, left, right, options, _ in MULTIPLIES:
        destination = options.get("destination", "mr")
        modifier = options.get("modifier", "SSF")
        opcode = {"mul": 0x40, "mac": 0x80, "msub": 0xc0}[operation]
        opcode |= ((modifier[0] == "S") << 4) | ((modifier[1] == "S") << 5)
        opcode |= ((modifier[2] == "F") << 3) | modifier.endswith("R")
        opcode |= (destination == "mr") << 2
        for bank in range(1 if operation == "mul" and destination == "register" else 2):
            def evaluate(x, y, lane):
                old = options.get("accumulator", 0)
                astat = options.get("astat", 0) ^ (0xA5000208 if lane else 0)
                stky = options.get("stky", 0) ^ (0x220 if lane else 0)
                return old, 0xAABBCCDD11, astat, stky, multiply(
                    operation, x, y, **(options | {"astat": astat, "stky": stky}))
            field = 0x100012 | ((opcode | (bank << 1)) << 12)
            row(name, field, bank, ((left, right), (right, left)), evaluate)

    for name, operation, old, options, _ in UNARY:
        fmt = options.get("format", "SF")
        destination = options.get("destination", "mr")
        opcode = {"sat": 0, "round": 0x10, "clear": 0x14}[operation]
        if operation != "clear":
            opcode |= ((fmt[1] == "F") << 3) | ((destination == "mr") << 2) | (fmt[0] == "S")
        for bank in range(2):
            def evaluate(unused_left, unused_right, lane):
                astat = options.get("astat", 0) ^ (0xA5000208 if lane else 0)
                stky = options.get("stky", 0) ^ (0x220 if lane else 0)
                return old, 0xAABBCCDD11, astat, stky, accumulator_unary(
                    operation, old, **(options | {"astat": astat, "stky": stky}))
            row(name, 0x100000 | ((opcode | (bank << 1)) << 12), bank,
                ((0, 0), (0, 0)), evaluate)

    for name, direction, part, old, options, _ in MOVES:
        for bank in range(2):
            def evaluate(unused_left, unused_right, lane):
                register = options.get("register", 0xAABBCCDD11)
                astat = options.get("astat", 0) ^ (0xA5000208 if lane else 0)
                stky = options.get("stky", 0) ^ (0x220 if lane else 0)
                return old, register, astat, stky, accumulator_move(
                    direction, part, old, register=register, astat=astat, stky=stky)
            field = 0x400000 | ((direction == "write") << 16) | ((part | (bank << 2)) << 12)
            row(name, field, bank, ((0, 0), (0, 0)), evaluate)

    return """
struct MultiplierLane {
  std::uint64_t left, right, old_register, result_register;
  std::array<std::uint32_t, 3> old_mr, result_mr;
  std::uint32_t old_astat, old_stky, astat, stky;
};
struct MultiplierCase {
  std::string_view name;
  std::uint32_t field;
  unsigned bank;
  std::array<MultiplierLane, 2> lanes;
};
inline constexpr std::array multiplier_cases{
""" + ",\n".join(rows) + "\n};\n"


def _limb_product(x, y, modifier):
    # Schoolbook 16-bit partial products, then independently apply signed-input
    # corrections. The reference itself uses an unbounded signed multiplication.
    x0, x1, y0, y1 = x & 0xFFFF, x >> 16, y & 0xFFFF, y >> 16
    product = x0 * y0 + ((x0 * y1 + x1 * y0) << 16) + ((x1 * y1) << 32)
    if modifier[0] == "S" and x >> 31:
        product -= y << 32
    if modifier[1] == "S" and y >> 31:
        product -= x << 32
    if modifier[:2] == "SS" and x >> 31 and y >> 31:
        product += 1 << 64
    if modifier[:3] == "SSF":
        product <<= 1
    return [(product >> (16 * i)) & 0xFFFF for i in range(5)]


def _limb_accumulate(parts, accumulator, operation, rounded):
    if operation != "mul":
        carry = int(operation == "msub")
        for i in range(5):
            part = parts[i] if operation == "mac" else parts[i] ^ 0xFFFF
            value = ((accumulator >> (16 * i)) & 0xFFFF) + part + carry
            parts[i], carry = value & 0xFFFF, value >> 16
    if rounded:
        discarded = parts[0] | (parts[1] << 16)
        carry = int(discarded > 0x80000000 or (discarded == 0x80000000 and parts[2] & 1))
        parts[0] = parts[1] = 0
        for i in range(2, 5):
            value = parts[i] + carry
            parts[i], carry = value & 0xFFFF, value >> 16
    return sum(part << (16 * i) for i, part in enumerate(parts))


def product_lattice():
    invalid = [
        lambda: multiply("mul", 0, 0, modifier="SSIR"),
        lambda: multiply("mul", 0, 0, accumulator=1 << 80),
        lambda: accumulator_unary("round", 0, format="SI"),
        lambda: accumulator_unary("clear", 0, destination="register"),
        lambda: accumulator_move("read", 3, 0),
    ]
    for call in invalid:
        require_raises(ValueError, call, 'invalid accumulator expectation request accepted')
    words = (0, 1, 0xFFFF, 0x10000, 0x12345678, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)
    modifiers = ("SSI", "SUI", "USI", "UUI", "SSF", "SUF", "USF", "UUF",
                 "SSFR", "SUFR", "USFR", "UUFR")
    accumulators = (0, 0xFFFFFFFF, 0x123456789ABCDEF01234,
                    0x7FFFFFFFFFFFFFFFFFFF, 0x80000000000000000000, 0xFFFFFFFFFFFFFFFFFFFF)
    count = 0
    for x, y in cartesian_product(words, words):
        for modifier in modifiers:
            for old in accumulators:
                for operation in ("mul", "mac", "msub"):
                    parts = _limb_product(x, y, modifier)
                    value = _limb_accumulate(parts, old, operation, modifier.endswith("R"))
                    fractional, signed = modifier[2] == "F", "S" in modifier
                    guard_bits = (17 if signed else 16) if fractional else (49 if signed else 48)
                    guard = value >> (80 - guard_bits)
                    overflow = guard not in ((0, (1 << guard_bits) - 1) if signed else (0,))
                    number = value - (1 << 80) if signed and value >> 79 else value
                    underflow = fractional and 0 < abs(number) < 1 << 32
                    flags = (0x40 if value >> 79 else 0) | (0x80 if overflow else 0)
                    flags |= 0x100 if underflow else 0
                    for destination in ("mr", "register"):
                        actual = multiply(operation, (x << 8) | 0xA5, (y << 8) | 0x5A,
                                          modifier=modifier, accumulator=old,
                                          destination=destination, astat=0xFFFFFFFF, stky=0x200)
                        first = 2 if fractional else 0
                        raw = (parts[first] | (parts[first + 1] << 16)) << 8
                        expected = MultiplierResult(value if destination == "mr" else old,
                                                    None if destination == "mr" else raw,
                                                    0xFFFFFC3F | flags,
                                                    0x200 | (0x40 if overflow else 0))
                        require(actual, expected, (modifier, operation, x, y, old, destination))
                        count += 1
    return count
