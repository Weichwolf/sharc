# SPDX-License-Identifier: GPL-3.0-only
"""Authored shifter cases and bit-position/list based cross-checks."""

from itertools import product as cartesian_product
from dataclasses import asdict
from fractions import Fraction

from expectation_check import require, require_raises
from reference import (
    FifoResult, IntegerResult, Unqualified, bit_fifo, field, float_unpack, leading, shift,
)


SHIFTS = [
    ("left shift clears extension", "lshift", 0x00000001FF, 1, IntegerResult(0x0000000200, 0x800, 0)),
    ("left zero still sets SV", "lshift", 0, 1, IntegerResult(0, 0x1800, 0)),
    ("logical right", "lshift", 0x8000000000, -1, IntegerResult(0x4000000000, 0, 0)),
    ("arithmetic right", "ashift", 0x8000000000, -1, IntegerResult(0xC000000000, 0, 0)),
    ("arithmetic left reaches the negative rail", "ashift", 0xFFFFFFFF00, 31,
     IntegerResult(0x8000000000, 0, 0)),
    ("arithmetic left changes the sign", "ashift", 0x4000000000, 1,
     IntegerResult(0x8000000000, 0x800, 0)),
    ("arithmetic zero beyond the word", "ashift", 0, 32, IntegerResult(0, 0x1000, 0)),
    ("off-scale arithmetic right", "ashift", 0x8000000000, -0x80000000,
     IntegerResult(0xFFFFFFFF00, 0, 0)),
    ("off-scale logical right", "lshift", 0xFFFFFFFF00, -0x80000000, IntegerResult(0, 0x1000, 0)),
    ("off-scale left", "ashift", 0xFFFFFFFF00, 0x7FFFFFFF, IntegerResult(0, 0x1800, 0)),
    ("rotate left wraps", "rot", 0x8000000100, 1, IntegerResult(0x0000000300, 0, 0)),
    ("rotate right wraps", "rot", 0x0000000100, -1, IntegerResult(0x8000000000, 0, 0)),
    ("rotate full word", "rot", 0x12345678FF, -32, IntegerResult(0x1234567800, 0, 0)),
    ("clear only bit", "bclr", 0x8000000000, 31, IntegerResult(0, 0x1000, 0)),
    ("set bit", "bset", 0, 31, IntegerResult(0x8000000000, 0, 0)),
    ("toggle bit", "btgl", 0x0000000100, 0, IntegerResult(0, 0x1000, 0)),
    ("off-scale set preserves data", "bset", 0x0000000100, 32, IntegerResult(0x0000000100, 0x800, 0)),
    ("negative clear position preserves data", "bclr", 0x0000000100, -1,
     IntegerResult(0x0000000100, 0x800, 0)),
    ("test one writes no data", "btst", 0x0000000100, 0, IntegerResult(None, 0, 0)),
    ("test zero writes no data", "btst", 0, 31, IntegerResult(None, 0x1000, 0)),
    ("off-scale bit test", "btst", 0xFFFFFFFF00, 32, IntegerResult(None, 0x1800, 0)),
    ("negative register bit test", "btst", 0xFFFFFFFF00, -1, IntegerResult(None, 0x1800, 0)),
]

FIELDS = [
    ("deposit", "deposit", 0x0000000BFF, 4, 4, {}, IntegerResult(0x000000B000, 0, 0)),
    ("deposit sign extension", "deposit", 0x0000000B00, 4, 4, {"sign_extend": True},
     IntegerResult(0xFFFFFFB000, 0, 0)),
    ("deposit OR", "deposit_or", 0x0000000B00, 4, 4, {"old_destination": 0x00000005FF},
     IntegerResult(0x000000B500, 0, 0)),
    ("sign extension precedes OR", "deposit_or", 0x0000000800, 4, 4,
     {"sign_extend": True, "old_destination": 0x0000000500}, IntegerResult(0xFFFFFF8500, 0, 0)),
    ("crossing deposit", "deposit", 0x0000000F00, 30, 4, {"sign_extend": True},
     IntegerResult(0xC000000000, 0x800, 0)),
    ("extract", "extract", 0x00000B00FF, 8, 4, {}, IntegerResult(0x0000000B00, 0, 0)),
    ("extract sign extension", "extract", 0x00000B0000, 8, 4, {"sign_extend": True},
     IntegerResult(0xFFFFFFFB00, 0, 0)),
    ("crossing extract does not sign extend", "extract", 0xC000000000, 30, 4,
     {"sign_extend": True}, IntegerResult(0x0000000300, 0x800, 0)),
    ("zero-length field", "extract", 0xFFFFFFFF00, 31, 0, {}, IntegerResult(0, 0x1000, 0)),
    ("signed empty extract", "extract", 0xFFFFFFFFFF, 32, 0, {"sign_extend": True},
     IntegerResult(0, 0x1000, 0)),
    ("signed empty deposit OR", "deposit_or", 0xFFFFFFFFFF, 63, 0,
     {"sign_extend": True, "old_destination": 0x00000005FF}, IntegerResult(0x0000000500, 0x800, 0)),
    ("off-scale zero-length deposit", "deposit", 0xFFFFFFFF00, 63, 0, {},
     IntegerResult(0, 0x1800, 0)),
    ("length beyond word", "extract", 0x8000000000, 0, 63, {"sign_extend": True},
     IntegerResult(0x8000000000, 0x800, 0)),
]

LEADING = [
    ("zero exponent", "exp", 0, {}, IntegerResult(0xFFFFFFE100, 0, 0)),
    ("negative all-sign exponent", "exp", 0xFFFFFFFF00, {}, IntegerResult(0xFFFFFFE100, 0x2000, 0)),
    ("normalized positive exponent", "exp", 0x4000000000, {}, IntegerResult(0, 0x1000, 0)),
    ("extended positive overflow", "exp_extended", 0x4000000000, {"astat": 2},
     IntegerResult(0x0000000100, 0x2002, 0)),
    ("extended negative overflow", "exp_extended", 0x8000000000, {"astat": 2},
     IntegerResult(0x0000000100, 2, 0)),
    ("32 leading zeros", "leftz", 0, {}, IntegerResult(0x0000002000, 0x800, 0)),
    ("no leading zeros", "leftz", 0x8000000000, {}, IntegerResult(0, 0x1000, 0)),
    ("32 leading ones", "lefto", 0xFFFFFFFF00, {}, IntegerResult(0x0000002000, 0x800, 0)),
]

FIFOS = [
    ("append", "deposit", 0xA000000000000000, 4, {"raw": 0x0000000B00, "length": 4},
     FifoResult(0xAB00000000000000, 8, None, 0, 0)),
    ("append ORs old tail", "deposit", 0xFFFFFFFFFFFFFFFF, 0, {"raw": 0, "length": 1},
     FifoResult(0xFFFFFFFFFFFFFFFF, 1, None, 0, 0)),
    ("extract and align", "extract", 0xAB00000000000000, 8, {"length": 4},
     FifoResult(0xB000000000000000, 4, 0x0000000A00, 0, 0)),
    ("query preserves FIFO and SF", "extract", 0xAB00000000000000, 40,
     {"length": 4, "no_update": True, "astat": 0x4000},
     FifoResult(0xAB00000000000000, 40, 0x0000000A00, 0x4000, 0)),
    ("empty zero-length extract", "extract", 0, 0, {"length": 0}, FifoResult(0, 0, 0, 0x1000, 0)),
    ("fill FIFO", "deposit", 0, 32, {"raw": 0x1234567800, "length": 32},
     FifoResult(0x0000000012345678, 64, None, 0x4000, 0)),
    ("write pointer masks seven bits", "write_pointer", 0x1234, 0, {"raw": 0x000000FFFF},
     FifoResult(0x1234, 127, None, 0x4800, 0)),
    ("read pointer leaves SF unchanged", "read_pointer", 0x1234, 64, {"astat": 0x4000},
     FifoResult(0x1234, 64, 0x0000004000, 0x4000, 0)),
]

UNPACK = [
    ("unpack zero", 0x0000000000, IntegerResult(0, 0, 0)),
    ("unpack negative zero", 0x0000800000, IntegerResult(0x8000000000, 0, 0)),
    ("unpack smallest positive", 0x0000000100, IntegerResult(0x3700000000, 0, 0)),
    ("unpack upper denormal bit", 0x0000040000, IntegerResult(0x3C00000000, 0, 0)),
    ("unpack smallest normal", 0x0000080000, IntegerResult(0x3C80000000, 0, 0)),
    ("unpack maximum magnitude", 0x00007FFF00, IntegerResult(0x43FFF00000, 0, 0)),
]


def anchors():
    records = []
    for name, operation, raw, amount, expected in SHIFTS:
        require(shift(operation, raw, amount), expected, name)
        records.append(dict(name=name, kind="shifter", operation=operation, left=raw,
                            amount=amount, expected=asdict(expected)))
    for name, operation, raw, position, length, options, expected in FIELDS:
        require(field(operation, raw, position, length, **options), expected, name)
        records.append(dict(name=name, kind="field", operation=operation, left=raw,
                            position=position, length=length, options=options, expected=asdict(expected)))
    for name, operation, raw, options, expected in LEADING:
        require(leading(operation, raw, **options), expected, name)
        records.append(dict(name=name, kind="leading", operation=operation, left=raw,
                            options=options, expected=asdict(expected)))
    for name, operation, fifo, pointer, options, expected in FIFOS:
        require(bit_fifo(operation, fifo, pointer, **options), expected, name)
        record = dict(name=name, kind="fifo", operation=operation, fifo=f"0x{fifo:016x}",
                      pointer=pointer, options=options, expected=asdict(expected))
        record["expected"]["fifo"] = f"0x{expected.fifo:016x}"
        records.append(record)
    for name, raw, expected in UNPACK:
        require(float_unpack(raw), expected, name)
        records.append(dict(name=name, kind="shifter", operation="unpack", left=raw,
                            expected=asdict(expected)))
    return records


def installed_header():
    """Emit separate operand and result records for native shifter operations."""
    register_form, immediate_form = 0x010000200000, 0x020000000000
    destination, source, count_register = 11, 3, 9
    rows = []

    fields = ("deposit", "deposit_or", "extract")

    def emit(word, operation, sources, amounts, *, leading_operation=False, av=False, extend=False):
        lanes = []
        for lane, (x, amount) in enumerate(zip(sources, amounts, strict=True)):
            raw = (x << 8) | (0x39 + lane)
            count = ((amount & 0xffffffff) << 8) | (0x91 + lane)
            old_destination = (0x00000000df, 0x81234567ef)[lane]
            astat = (0xa5003ffc, 0x5a004ffc)[lane] | (2 if bool(lane) != av else 0)
            stky = (0x20000004, 0x00000100)[lane]
            if leading_operation:
                effect = leading(operation, raw, astat=astat, stky=stky)
            elif operation in fields:
                effect = field(operation, raw, amount & 63, (amount >> 6) & 63, sign_extend=extend,
                               old_destination=old_destination, astat=astat, stky=stky)
            else:
                effect = shift(operation, raw, amount, astat=astat, stky=stky)
            lanes.append("{" + ", ".join(hex(value) for value in
                         (raw, count, old_destination, 0 if effect.raw is None else effect.raw,
                          astat, effect.astat, effect.stky)) + "}")
        writes = str(operation != "btst").lower()
        rows.append(f"{{{word:#x}, {writes}, {{{{" + ", ".join(lanes) + "}}}")

    positions = (-0x80000000, -257, -1, 0, 1, 7, 15, 30, 31, 32, 33, 255, 256, 0x7fffffff)
    immediate_positions = (0, 1, 7, 15, 30, 31)
    sources = ((0, 0xffffffff), (0x80000001, 0x00008000))
    for opcode, operation in ((0xc0, "bset"), (0xc4, "bclr"), (0xc8, "btgl"), (0xcc, "btst")):
        target = 0 if operation == "btst" else destination
        word = register_form | (opcode << 12) | (target << 8) | (source << 4) | count_register
        for index, amount in enumerate(positions):
            for pair in sources:
                emit(word, operation, pair, (amount, positions[(index + 5) % len(positions)]))
        immediate = immediate_positions if operation == "btst" else (*immediate_positions, 32, 127, 128, 255)
        for amount in immediate:
            word = immediate_form | (opcode << 14) | (amount << 8) | (target << 4) | source
            for pair in sources:
                emit(word, operation, pair, (amount, amount))

    values = (0, 1, 0x3fffffff, 0x40000000, 0x7fffffff, 0x80000000, 0xc0000000, 0xffffffff)
    for opcode, operation in ((0x80, "exp"), (0x84, "exp_extended"), (0x88, "leftz"), (0x8c, "lefto")):
        word = register_form | (opcode << 12) | (destination << 8) | (source << 4)
        for value, av in cartesian_product(values, (False, True)):
            emit(word, operation, (value, value ^ 0xffffffff), (0, 0), leading_operation=True, av=av)

    bounds = ((0, 0), (32, 0), (63, 0), (0, 1), (4, 4), (31, 1), (32, 1), (0, 15),
              (2, 16), (28, 4), (30, 4), (0, 31), (0, 32), (0, 63), (63, 63))
    amounts = tuple((length << 6) | position for position, length in bounds)
    ignored = 0xa531e000
    for base, operation in ((0x40, "extract"), (0x44, "deposit"), (0x64, "deposit_or")):
        for extend in (False, True):
            opcode = base | (8 if extend else 0)
            word = register_form | (opcode << 12) | (destination << 8) | (source << 4) | count_register
            for index, amount in enumerate(amounts):
                for pair in sources:
                    other = amounts[(index + 4) % len(amounts)]
                    emit(word, operation, pair, (ignored | amount, ignored | other), extend=extend)
                dataex = (amount >> 8) << 27
                immediate = immediate_form | (opcode << 14) | dataex | ((amount & 255) << 8)
                for pair in sources:
                    emit(immediate | (destination << 4) | source, operation, pair,
                         (amount, amount), extend=extend)

    return """
struct ShifterLane {
  std::uint64_t source, count, old_destination, result;
  std::uint32_t old_astat, astat, stky;
};
struct ShifterCase {
  std::uint64_t word;
  bool writes;
  std::array<ShifterLane, 2> lanes;
};
inline constexpr std::array<ShifterCase, """ + str(len(rows)) + "> shifter_cases{{\n" + ",\n".join(rows) + "\n}};\n"


def bit_lattice():
    """Route individual bits instead of using whole-word shifts/masks."""
    count = 0
    words = (0, 1, 0x80000000, 0xFFFFFFFF, 0xA35C7E19)
    for word in words:
        bits = [int(char) for char in f"{word:032b}"[::-1]]
        for amount in (*range(-128, 128), -0x80000000, 0x7FFFFFFF):
            for operation in ("lshift", "lshift_or", "ashift", "ashift_or", "rot", "bclr", "bset", "btgl", "btst"):
                old_destination = 0
                if operation in ("lshift", "lshift_or", "ashift", "ashift_or", "rot"):
                    arithmetic = operation.startswith("ashift")
                    output = []
                    for target in range(32):
                        source = target - amount
                        if operation == "rot":
                            output.append(bits[source % 32])
                        else:
                            output.append(bits[source] if 0 <= source < 32 else
                                          bits[31] if source >= 32 and arithmetic else 0)
                    sv = operation != "rot" and amount > 0
                    if arithmetic and amount > 0:
                        sv = ((any(bits) or operation == "ashift_or") if amount >= 32 else
                              any(bit != bits[31] for bit in bits[31 - amount:]))
                    if operation.endswith("_or"):
                        old_bits = [int(index % 3 == 0) for index in range(32)]
                        output = [bit | old for bit, old in zip(output, old_bits, strict=True)]
                        old_destination = 255 + sum(bit * 2**(index + 8)
                                                    for index, bit in enumerate(old_bits))
                else:
                    output = bits.copy() if operation != "btst" else [0] * 32
                    if 0 <= amount < 32:
                        output[amount] = {"bclr": 0, "bset": 1, "btgl": 1 - bits[amount],
                                          "btst": bits[amount]}[operation]
                    sv = not 0 <= amount < 32
                value = sum(bit * 2**index for index, bit in enumerate(output))
                flags = 0xFFFFC7FF | (int(sv) << 11) | (int(not any(output)) << 12)
                expected = IntegerResult(None if operation == "btst" else value * 256, flags, 0x55)
                require(shift(operation, word * 256 + 255, amount, old_destination=old_destination,
                              astat=0xFFFFFFFF, stky=0x55),
                        expected, (operation, word, amount))
                count += 1
        for position, length, sign_extend in cartesian_product(range(64), range(64), (False, True)):
            for operation in ("deposit", "deposit_or", "extract"):
                output = []
                for target in range(32):
                    source = target - position if operation != "extract" else target + position
                    active = (0 <= source < min(length, 32) if operation != "extract" else
                              target < length and source < 32)
                    bit = bits[source] if active else 0
                    above = target >= (position + length if operation != "extract" else length)
                    if sign_extend and length and position + length <= 32 and above:
                        bit = bits[length - 1 if operation != "extract" else position + length - 1]
                    if operation == "deposit_or":
                        bit |= (target % 3 == 0)
                    output.append(bit)
                value = sum(bit * 2**index for index, bit in enumerate(output))
                flags = 0xFFFFC7FF | (int(position + length > 32) << 11)
                flags |= int(not any(output)) << 12
                expected = IntegerResult(value * 256, flags, 0x55)
                old = sum(2**bit for bit in range(0, 32, 3)) * 256 + 255
                require(field(operation, word * 256 + 255, position, length,
                              sign_extend=sign_extend, old_destination=old,
                              astat=0xFFFFFFFF, stky=0x55), expected,
                        (operation, word, position, length, sign_extend))
                count += 1
    return count


def fifo_lattice():
    count = 0
    for data in (0, 0xFFFFFFFFFFFFFFFF, 0xA35C7E19B2468DF0):
        bits = [int(char) for char in f"{data:064b}"]
        for pointer, length, operation in cartesian_product(range(65), range(33), ("deposit", "extract")):
            if operation == "deposit" and pointer + length > 64:
                continue
            if operation == "extract" and length > pointer:
                continue
            for no_update in ((False, True) if operation == "extract" else (False,)):
                output = bits.copy()
                updated, result = pointer, None
                if operation == "deposit":
                    incoming = [int(char) for char in f"{0xB2468DF0:032b}"][-length:] if length else []
                    for index, bit in enumerate(incoming, start=pointer):
                        output[index] |= bit
                    updated += length
                else:
                    result = int("".join(map(str, bits[:length])) or "0", 2) * 256
                    if not no_update:
                        output = bits[length:] + [0] * length
                        updated -= length
                flags = 0xFFFF87FF | (int(operation == "extract" and result == 0) << 12)
                flags |= 0x4000 if (pointer if no_update else updated) >= 32 else 0
                expected = FifoResult(int("".join(map(str, output)), 2), updated,
                                      result, flags, 0x55)
                old_flags = (0xFFFFFFFF & ~0x4000) | (0x4000 if pointer >= 32 else 0)
                require(bit_fifo(operation, data, pointer, raw=0xB2468DF0FF, length=length,
                                 no_update=no_update, astat=old_flags, stky=0x55),
                        expected, (operation, data, pointer, length, no_update))
                count += 1
    return count


def pointer_lattice():
    count = 0
    for pointer in range(128):
        old = (0xFFFFFFFF & ~0x4000) | (0x4000 if pointer >= 32 else 0)
        require(bit_fifo("read_pointer", 0xA35C7E19B2468DF0, pointer, astat=old, stky=0x55),
                FifoResult(0xA35C7E19B2468DF0, pointer, pointer * 256, old & ~0x3800, 0x55),
                ("pointer read", pointer))
        count += 1
    for byte in range(256):
        word = 0xA35C7E00 + byte
        written = int(f"{word:032b}"[-7:], 2)
        for pointer in (0, 32, 127):
            old = (0xFFFFFFFF & ~0x4000) | (0x4000 if pointer >= 32 else 0)
            flags = (old & ~0x7800) | (0x4000 if written >= 32 else 0)
            flags |= 0x800 if written > 64 else 0
            require(bit_fifo("write_pointer", 0xA35C7E19B2468DF0, pointer,
                             raw=word * 256 + 255, astat=old, stky=0x55),
                    FifoResult(0xA35C7E19B2468DF0, written, None, flags, 0x55),
                    ("pointer write", byte, pointer))
            count += 1
    return count


def leading_lattice():
    count = 0
    words = {value for width in range(33) for value in
             ((1 << width) - 1, 0xFFFFFFFF ^ ((1 << width) - 1))}
    for word in sorted(words):
        bits = f"{word:032b}"
        for av in (False, True):
            old = (0xFFFFFFFF & ~2) | (2 if av else 0)
            for operation in ("exp", "exp_extended", "leftz", "lefto"):
                if operation.startswith("exp"):
                    run = next((i for i, bit in enumerate(bits) if bit != bits[0]), 32)
                    extended = operation == "exp_extended" and av
                    value = 1 if extended else 1 - run
                    ss = (bits[0] == "1") != extended
                    sv = False
                else:
                    wanted = "0" if operation == "leftz" else "1"
                    value = next((i for i, bit in enumerate(bits) if bit != wanted), 32)
                    ss, sv = False, value == 32
                flags = old & ~0x3800
                flags |= (int(value == 0) << 12) | (int(ss) << 13) | (int(sv) << 11)
                require(leading(operation, word * 256 + 255, astat=old, stky=0x55),
                        IntegerResult((value & 0xFFFFFFFF) * 256, flags, 0x55),
                        (operation, word, av))
                count += 1
    return count


def unpack_lattice():
    """Decode output's rational value; compare to the short format's value equation."""
    for short in range(65536):
        result = float_unpack((0xA5A50000 | short) * 256 + 255, astat=0xFFFFFFFF, stky=0x55)
        exponent, fraction = (short // 2048) % 16, short % 2048
        expected = (Fraction(2048 + fraction) * Fraction(2)**(exponent - 18) if exponent else
                    Fraction(fraction, 131072))
        if short >= 32768:
            expected = -expected
        packed = result.raw // 256
        e, f = (packed // 8388608) % 256, packed % 8388608
        actual = Fraction(0) if e == 0 else Fraction(8388608 + f) * Fraction(2)**(e - 150)
        if packed >= 2147483648:
            actual = -actual
        require(actual, expected, short)
        require(bool(packed & 0x80000000), short >= 32768, short)
        require((result.raw % 256, result.astat, result.stky), (0, 0xFFFFC7FF, 0x55), short)
    return 65536


def explicit_gaps():
    for call in (
        lambda: bit_fifo("deposit", 0, 64, length=1),
        lambda: bit_fifo("extract", 0, 0, length=1),
        lambda: bit_fifo("deposit", 0, 0, length=33),
    ):
        require_raises(Unqualified, call, 'unqualified shifter result generated')
