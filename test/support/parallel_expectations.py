# SPDX-License-Identifier: GPL-3.0-only
"""Independent dual/multifunction effects and authored two-lane expectations."""

from itertools import product as cartesian_product
from dataclasses import asdict, is_dataclass
from fractions import Fraction
import json

from floating_expectations import encode
from multiplier_expectations import _limb_accumulate, _limb_product
from expectation_check import require, require_raises
from reference import (
    IntegerResult, Mode, ParallelResult, RegisterWrite, binary32_operand, dual_alu, fixed,
    floating_alu, floating_multiply, parallel_compute, register_writeback,
)


DUAL = [
    ("dual zero and carry from subtraction", "fixed", 0x0000000100, 0x0000000100, {},
     ParallelResult((0x0000000200, 0), 9, 0)),
    ("dual overflow and subtraction carry", "fixed", 0x7FFFFFFF00, 0x0000000100, {},
     ParallelResult((0x8000000000, 0x7FFFFFFE00), 0xE, 4)),
    ("dual saturated overflow", "fixed", 0x7FFFFFFF00, 0x0000000100, {"saturate": True},
     ParallelResult((0x7FFFFFFF00, 0x7FFFFFFE00), 0xA, 4)),
    ("dual floating cancellation", "float", 0x3F80000000, 0xBF80000000, {"mode": Mode(40)},
     ParallelResult((0, 0x4000000000), 0x401, 0)),
    ("dual floating overflow and zero", "float", 0x7F7FFFFFFF, 0x7F7FFFFFFF,
     {"mode": Mode(40)}, ParallelResult((0x7F80000000, 0), 0x403, 2)),
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
     ParallelResult((0, 0x7F80000000, 0), 0x503, 0x102, 0)),
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
    ("lane-local floating overflow versus exact cancellation", "float", "add",
     (0x7F7FFFFFFF, 0x3F80000000), (0x7F7FFFFFFF, 0xBF80000000), {"mode": Mode(40)},
     (IntegerResult(0x7F80000000, 0x402, 2), IntegerResult(0, 0x401, 0))),
    ("lane-local multiplier overflow versus underflow", "multiply", "mul",
     (0x7F7FFFFFFF, 0x0080000000), (0x4000000000, 0x3F00000000), {"mode": Mode(40)},
     (IntegerResult(0x7F80000000, 0x80, 0x80), IntegerResult(0, 0x100, 0x100))),
    ("lane-local ABS invalid", "float", "abs", (0x3F80000000, 0xFFC0000000), (0, 0),
     {"mode": Mode(40)}, (IntegerResult(0x3F80000000, 0x400, 0),
                          IntegerResult(0xFFFFFFFFFF, 0x420, 0x20))),
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


def installed_header():
    """Export authored multifunction packets with independently selected writeback winners."""
    rows = []
    input_registers = (0, 4, 8, 12)
    inputs = ((0x8000000011, 0x8000000022, 0x7FFFFFFF33, 0x0000000144),
              (0xFFFFFFFD55, 0x4000000066, 0xFFFFFFFD77, 0x0000000088))

    def emit(name, opcode, multiplier, destination, alu, outputs, mode1, conditional=False):
        astat = (0xA59903C1, 0x5A2203C0)
        stky = (0x20, 0x200)
        old_mr = (0x100000001, 0xFFFFFFFFFFFFFFFFFFFF)
        registers = [[[0x1000000080 * (1 + lane) + (bank << 32) + (reg << 8)
                       for reg in range(16)] for bank in range(2)] for lane in range(2)]
        effects = []
        proposals = []
        for lane, arguments in enumerate(inputs):
            for reg, value in zip(input_registers, arguments):
                mode_bit = 0x400 if reg < 8 else 0x80
                registers[lane][bool(mode1 & mode_bit)][reg] = value
            active = (lane == 0 or mode1 & 0x200000) and (not conditional or astat[lane] & 1)
            if active:
                effect = parallel_compute(
                    "fixed", *arguments, alu=alu, multiplier=multiplier, destination=destination,
                    modifier="SSF" if destination == "mr" else "SSFR", accumulator=old_mr[lane],
                    saturate=bool(mode1 & 0x2000), truncate=bool(mode1 & 0x8000),
                    astat=astat[lane], stky=stky[lane])
                pe = "x" if lane == 0 else "y"
                proposals.extend(RegisterWrite(producer + pe, lane, reg, raw)
                                 for producer, reg, raw in zip(("multiplier_", "alu_", "alu_"),
                                                              outputs, effect.raw)
                                 if raw is not None)
                effects.append((effect.accumulator, effect.astat, effect.stky))
            else:
                effects.append((old_mr[lane], astat[lane], stky[lane]))
        final = register_writeback(registers, proposals, mode1=mode1)
        rm, ra, *rs = outputs
        field = ((0x600000 | (rs[0] << 16)) if rs else 0x400000 | (opcode << 16)) | (rm << 12) | (ra << 8)

        def array(values, suffix=""):
            return "{" + ", ".join(f"0x{value:x}{suffix}" for value in values) + "}"

        def banks(values):
            return "{{" + ", ".join(array(bank, "ULL") for bank in values) + "}}"

        def limbs(value):
            return array((value >> shift) & 0xffffffff for shift in (0, 32, 64))

        lanes = []
        for lane, (mr, flags, sticky) in enumerate(effects):
            lanes.append("MultifunctionLane{" + ", ".join((
                banks(registers[lane]), banks(final[lane]), limbs(old_mr[lane]), limbs(mr),
                f"0x{astat[lane]:x}", f"0x{stky[lane]:x}", f"0x{flags:x}", f"0x{sticky:x}")) + "}")
        header = 0x010000000000 if conditional else 0x013e00000000
        rows.append(f"MultifunctionCase{{{json.dumps(name)}, 0x{header | field:x}ULL, 0x{mode1:x}, "
                    + "{" + ", ".join(lanes) + "}}")

    for base, multiplier, destination in ((4, "mul", "register"), (8, "mac", "mr"),
                                           (12, "mac", "register"), (16, "msub", "mr"),
                                           (20, "msub", "register")):
        for offset, alu in enumerate(("add", "sub", "avg")):
            mode1 = 0x200000 | (0x480 if base & 4 else 0) | (0x2000 if base & 8 else 0)
            mode1 |= (0x8004 if base & 16 else 0)
            emit(f"{multiplier} to {destination} with {alu}", base + offset, multiplier,
                 destination, alu, (8, 0), mode1)
    emit("three fixed outputs read all old inputs", 0, "mul", "register", "dual", (8, 0, 4), 0x200000)
    emit("add beats multiply on the same register", 4, "mul", "register", "add", (8, 8), 0x200000)
    emit("subtract beats multiply with lane-local EQ", 0, "mul", "register", "dual", (4, 0, 4),
         0x200480, True)
    emit("SISD MAC uses the swapped physical bank", 8, "mac", "mr", "add", (0, 8), 4)

    return """
struct MultifunctionLane {
  std::array<std::array<std::uint64_t, 16>, 2> old_registers, result_registers;
  std::array<std::uint32_t, 3> old_mr, result_mr;
  std::uint32_t old_astat, old_stky, astat, stky;
};
struct MultifunctionCase {
  std::string_view name;
  std::uint64_t word;
  std::uint32_t mode1;
  std::array<MultifunctionLane, 2> lanes;
};
inline constexpr std::array multifunction_cases{
""" + ",\n".join(rows) + "\n};\n"


def fixed_dual_header():
    """Export numeric dual-ALU effects without encoding or lane-admission logic."""
    operands = ((1, 1), (0x7fffffff, 1), (0x80000000, 0xffffffff),
                (0x7fffffff, 0xffffffff), (0x80000000, 1), (0x80000000, 0x80000000),
                (0, 0), (0xffffffff, 1), (0, 0x80000000), (0xffffffff, 0))

    def lane(pair, number, saturate):
        x, y = pair
        inputs = ((x << 8) | (0x13 + number), (y << 8) | (0xc2 - number))
        astat = (0xa50003fe, 0x5a4307fe)[number]
        stky = (0x40, 0x80)[number]
        result = dual_alu("fixed", *inputs, astat=astat, stky=stky, saturate=saturate)

        def pair_literal(values):
            return "{" + ", ".join(hex(item) for item in values) + "}"

        return (f"FixedDualLane{{{pair_literal(inputs)}, {pair_literal(result.raw)}, "
                f"{astat:#x}, {stky:#x}, {result.astat:#x}, {result.stky:#x}}}")

    def case(first, second, saturate):
        lanes = ", ".join(lane(pair, number, saturate)
                          for number, pair in enumerate((first, second)))
        return f"FixedDualCase{{{str(saturate).lower()}, {{{lanes}}}}}"

    cases = [case(first, tuple(reversed(operands[(i + 3) % len(operands)])), sat)
             for (i, first), sat in cartesian_product(enumerate(operands), (False, True))]
    declaration = """
struct FixedDualLane {
  std::array<std::uint64_t, 2> inputs, outputs;
  std::uint32_t old_astat, old_stky, astat, stky;
};
struct FixedDualCase {
  bool saturate;
  std::array<FixedDualLane, 2> lanes;
};
inline constexpr std::array fixed_dual_cases{
"""
    return (declaration + ",\n".join(cases) + "\n};\ninline constexpr auto fixed_dual_packet = "
            + case(operands[1], (0xffffffff, 1), False) + ";\n")


def floating_header():
    """Complete results of the published floating compute fields at raw register boundaries."""
    shapes = [(0x0f808c, "dual-only", (0, 8))]
    shapes += [(0x400800 | (opcode << 16), operation, (0, 8))
               for opcode, operation in enumerate(("add", "sub", "float", "fix", "avg",
                                                   "abs", "max", "min"), 0x18)]
    shapes += [(0x740800, "dual", (0, 8, 4)), (0x588800, "add", (8, 8)),
               (0x748800, "dual", (8, 8, 4)), (0x744800, "dual", (4, 8, 4)),
               (0x700800, "dual", (0, 8, 0))]
    ordinary = (0x3f80000180, 0x3fc0000000, 0x3f80000000, 0x3380000000)
    special = (0x0080000000, 0x3f7fffffff, 0x7f7fffffff, 0x7f7fffffff)
    conversions = {"float": (0x0100000357, 0x0000000200),
                   "fix": (0xbfc0000000, 0xffffffff34)}
    exceptional = (0x7f80000001, 0xbf80000000, 0xffc0000000, 0x8000000000)
    inputs = (ordinary, special, exceptional)
    registers = (0, 4, 8, 12)
    rows = []

    def values(items):
        return "{" + ", ".join(hex(item) for item in items) + "}"

    for field, alu, outputs in shapes:
        for fast, flags, banks, selected in cartesian_product(
                (False, True), (0, 0x8000, 0x10000, 0x1a000),
                (0, 0x200000, 0x200480), (None, 0, 1)):
            mode1 = flags | banks
            mode = Mode(32 if fast or flags & 0x10000 else 40,
                        "zero" if flags & 0x8000 else "nearest")
            conditional = selected is not None
            lanes = []
            for lane in range(2):
                args = inputs[(lane + int(conditional)) % len(inputs)]
                if alu in conversions:
                    args = (*args[:2], *conversions[alu])
                    if lane == 1:
                        args = (*args[:2], *exceptional[2:]) if alu == "fix" else (
                            *args[:2], 0x100, 0x8000)
                status = (0xa50003fe, 0x420007fe)[lane] | int(lane == selected)
                sticky = (0x10, 0x204)[lane]
                result = dict(zip(registers, args, strict=True))
                active = (lane == 0 or banks & 0x200000) and (not conditional or status & 1)
                if active:
                    opts = dict(mode=mode, astat=status, stky=sticky)
                    if alu == "dual-only":
                        operands = tuple(binary32_operand(raw, mode) for raw in args[2:]) if fast else args[2:]
                        effect = dual_alu("float", *operands, **opts)
                    else:
                        effect = parallel_compute("float", *args, alu=alu, fast_fp32=fast,
                                                  saturate=bool(flags & 0x2000), **opts)
                    result.update(zip(outputs, effect.raw, strict=True))
                    if alu == "dual" and outputs[0] == outputs[2]:
                        result[outputs[0]] = effect.raw[0]
                    after_status, after_sticky = effect.astat, effect.stky
                else:
                    after_status, after_sticky = status, sticky
                final = values(result[reg] for reg in registers[:3])
                lanes.append("{" + ", ".join((values(args), final, hex(status), hex(sticky),
                                               hex(after_status), hex(after_sticky))) + "}")
            header = 0x010000000000 if conditional else 0x013e00000000
            rows.append("{" + ", ".join((hex(header | field), hex(mode1), str(fast).lower(),
                                        "{{" + ", ".join(lanes) + "}}")) + "}")

    return """
struct FloatingPacketLane {
  std::array<std::uint64_t, 4> inputs;
  std::array<std::uint64_t, 3> result;
  std::uint32_t old_astat, old_stky, astat, stky;
};
struct FloatingPacket {
  std::uint64_t word;
  std::uint32_t mode1;
  bool fast;
  std::array<FloatingPacketLane, 2> lanes;
};
inline constexpr std::array<FloatingPacket, """ + str(len(rows)) + """> floating_packets{{
""" + ",\n".join(rows) + "\n}};\n"


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
                    expected = ParallelResult((encode(x * y), *(encode(value) for value in outputs)),
                                              flags, 0x20, 0x1234)
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
