# SPDX-License-Identifier: GPL-3.0-only
"""Authored condition truth tables and conditional numeric lane expectations."""

from dataclasses import asdict

from reference import (
    IntegerResult, LaneCondition, Mode, Unqualified, condition, lane_condition, predicated_alu,
)


# Order: AF, ALUSAT, AV, AN, AZ, with AZ changing fastest. These are literal
# truth tables transcribed independently from the bars in table 4-37, not a
# second evaluation of the reference's boolean expression.
LESS = "00111100001100110010001000100010"
GREATER = "10000010100010001000100010001000"
FLAG_BITS = {"ac": 8, "av": 2, "mv": 128, "ms": 64, "sv": 2048,
             "sz": 4096, "sf": 16384, "tf": 262144}

CASES = [
    ("predicate uses old AZ independently in both lanes", "fixed", "add",
     dict(left=(0x7FFFFFFF00, 0xFFFFFFFF00), right=(0x0000000100, 0x0000000100),
          old_destination=(0x1122334455, 0xAABBCCDDEE), astat=(1, 0), stky=(0, 0x20), mnemonic="eq"),
     (IntegerResult(0x8000000000, 6, 4), IntegerResult(0xAABBCCDDEE, 0, 0x20))),
    ("SISD leaves PEy untouched despite true PEy condition", "fixed", "add",
     dict(left=(0x7FFFFFFF00, 0xFFFFFFFF00), right=(0x0000000100, 0x0000000100),
          old_destination=(0x1122334455, 0xAABBCCDDEE), astat=(0, 1), stky=(4, 0x20),
          mnemonic="eq", simd=False),
     (IntegerResult(0x1122334455, 0, 4), IntegerResult(0xAABBCCDDEE, 1, 0x20))),
    ("inactive unqualified FP input is not evaluated", "float", "pass",
     dict(left=(0x7FC0000000, 0x3F800000FF), right=(0, 0),
          old_destination=(0x8000000001, 0xAABBCCDDEE), astat=(0x402, 0), stky=(5, 0),
          mnemonic="not_av", mode=Mode(32)),
     (IntegerResult(0x8000000001, 0x402, 5), IntegerResult(0x3F80000000, 0x400, 0))),
    ("conditional compare keeps complete old destinations", "fixed", "cmp",
     dict(left=(0x0000000200, 0x0000000100), right=(0x0000000100, 0x0000000200),
          old_destination=(0x1122334455, 0xAABBCCDDEE), astat=(0xA5000000, 0x5A000000),
          stky=(1, 2), mnemonic="always"),
     (IntegerResult(0x1122334455, 0xD2000000, 1), IntegerResult(0xAABBCCDDEE, 0x2D000004, 2))),
    ("old AF selects predicate before new fixed operation", "fixed", "pass",
     dict(left=(0x00000001FF, 0x00000003FF), right=(0, 0),
          old_destination=(0x1122334455, 0xAABBCCDDEE), astat=(0x405, 5), stky=(1, 2), mnemonic="lt"),
     (IntegerResult(0x1122334455, 0x405, 1), IntegerResult(0x0000000300, 0, 2))),
]


def require(actual, expected, context):
    if actual != expected:
        raise AssertionError((context, expected, actual))


def anchors():
    records = []
    for name, kind, operation, options, expected in CASES:
        require(predicated_alu(kind, operation, **options), expected, name)
        serialized = {key: asdict(value) if isinstance(value, Mode) else value
                      for key, value in options.items()}
        records.append(dict(name=name, kind="conditional-numeric", format=kind,
                            operation=operation, options=serialized,
                            expected=[asdict(value) for value in expected]))
    return records


def predicate_lattice():
    count = 0
    for index, (lt, gt) in enumerate(zip(LESS, GREATER)):
        af, sat, av, an, az = (bool(index & bit) for bit in (16, 8, 4, 2, 1))
        status = (int(af) << 10) | (int(av) << 1) | (int(an) << 2) | int(az)
        expected = {"lt": lt == "1", "gt": gt == "1", "ge": lt == "0",
                    "le": gt == "0", "eq": az, "ne": not az, "always": True}
        for noise in (0, 0xA5FFFBC0):  # No ALU relation input bits in the noise.
            for name, value in expected.items():
                require(condition(name, status | noise, saturate=sat), value,
                        (name, index, noise))
                count += 1
    for name, bit in FLAG_BITS.items():
        for value in (False, True):
            for noise in (0, 0xFFFFFFFF, 0xA5C30000, 0x5A3CFFFF):
                status = (noise & ~bit) | (bit if value else 0)
                require(condition(name, status), value, (name, status))
                require(condition("not_" + name, status), not value, (name, status))
                count += 2
    return count


def lane_lattice():
    count = 0
    # Independently authored false/true status words, valid with either ALUSAT.
    inputs = {"eq": (0, 1), "ne": (1, 0), "gt": (1, 0), "le": (0, 1),
              "lt": (0x400, 0x404), "ge": (0x404, 0x400)}
    inputs.update({name: (0, bit) for name, bit in FLAG_BITS.items()})
    inputs.update({"not_" + name: (bit, 0) for name, bit in FLAG_BITS.items()})
    simd_table = (
        LaneCondition((False, False), False, False), LaneCondition((False, True), False, True),
        LaneCondition((True, False), False, True), LaneCondition((True, True), True, True),
    )
    sisd_table = (
        LaneCondition((False, False), False, False), LaneCondition((False, False), False, False),
        LaneCondition((True, False), True, True), LaneCondition((True, False), True, True),
    )
    for mnemonic, status in inputs.items():
        for index in range(4):
            for saturate in (False, True):
                for simd, table in ((False, sisd_table), (True, simd_table)):
                    require(lane_condition(mnemonic, (status[index // 2], status[index % 2]),
                                           simd=simd, saturate=saturate), table[index],
                            (mnemonic, index, simd, saturate))
                    count += 1
    return count


def explicit_gaps():
    for name in ("flag0_in", "not_lce", "bm", "ai"):
        try:
            condition(name, 0)
        except Unqualified:
            continue
        raise AssertionError("unmodeled external/system condition accepted")
