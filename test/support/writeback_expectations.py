# SPDX-License-Identifier: GPL-3.0-only
"""Independent register-bank and parallel writeback expectations from PRM 2-5..7."""

from dataclasses import asdict

from reference import IntegerResult, RegisterWrite, Unqualified, fixed, register_writeback


def registers():
    return tuple(tuple(tuple((lane + 1) * (1 << 36) + bank * (1 << 32) + reg * 256 + 165
                             for reg in range(16)) for bank in range(2)) for lane in range(2))


def replaced(old, lane, bank, register, value):
    # Independently form every expected physical cell; no model bank selector.
    return tuple(tuple(tuple(value if (p, b, r) == (lane, bank, register) else old[p][b][r]
                             for r in range(16)) for b in range(2)) for p in range(2))


def require(actual, expected, context):
    if actual != expected:
        raise AssertionError((context, expected, actual))


def anchors():
    cases = (
        ("DAG1 wins data while ALU overflow flags survive", 0x7FFFFFFF00, 0x100,
         "dag1", 0x1122334455, IntegerResult(0x8000000000, 6, 4)),
        ("DAG2 wins data while ALU zero and carry survive", 0xFFFFFFFF00, 0x100,
         "dag2", 0xAABBCCDDEE, IntegerResult(0, 9, 0)),
    )
    records = []
    for name, left, right, source, loaded, expected_compute in cases:
        old = replaced(replaced(registers(), 0, 0, 0, left), 0, 0, 1, right)
        computed = fixed("add", old[0][0][0], old[0][0][1])
        require(computed, expected_compute, name)
        proposals = (RegisterWrite("alu_x", 0, 0, computed.raw),
                     RegisterWrite(source, 0, 0, loaded))
        result = register_writeback(old, proposals)
        expected = replaced(old, 0, 0, 0, loaded)
        require(result, expected, name)
        require(old[0][0][0], left, name + ": old source was overwritten")
        records.append(dict(name=name, kind="parallel-register-writeback", mode1=0,
                            old_registers=old, proposals=[asdict(item) for item in proposals],
                            compute=dict(operation="add", left=left, right=right,
                                         expected=asdict(expected_compute)),
                            expected_registers=expected))
    old = registers()
    proposals = (RegisterWrite("dag1", 0, 9, 0x1122334400),
                 RegisterWrite("dag1", 1, 9, 0xAABBCCDD00))
    expected = replaced(replaced(old, 0, 1, 9, 0x1122334400), 1, 1, 9, 0xAABBCCDD00)
    require(register_writeback(old, proposals, mode1=0x80), expected, "independent PE files")
    records.append(dict(name="equal sources in distinct PE files do not compete",
                        kind="register-writeback", mode1=0x80, old_registers=old,
                        proposals=[asdict(item) for item in proposals], expected_registers=expected))
    return records


def bank_priority_lattice():
    old = registers()
    # Rows are the literal descending priority in the manual, independently of
    # the model's numeric rank dictionary. DAG1 and UREG share one row.
    rows = (("dag1", "ureg"), ("dag2",), ("alu_x",), ("alu_y",),
            ("multiplier_x",), ("multiplier_y",), ("shifter_x",), ("shifter_y",))
    bank_modes = ((0, 0, 0), (0x400, 1, 0), (0x80, 0, 1), (0x480, 1, 1))
    winning, losing = 0x1122334455, 0xAABBCCDDEE
    count = refusals = 0
    for mode, low_bank, high_bank in bank_modes:
        # Unrelated MODE1 bits must not select a different physical data bank.
        mode |= 0xA5A5A5A5 & ~0x480
        require(register_writeback(old, (), mode1=mode), old, "empty writeback")
        for lane in (0, 1):
            for reg in range(16):
                expected = replaced(old, lane, low_bank if reg < 8 else high_bank, reg, winning)
                for row_index, row in enumerate(rows):
                    for source in row:
                        higher = RegisterWrite(source, lane, reg, winning)
                        require(register_writeback(old, (higher,), mode1=mode), expected,
                                (mode, lane, reg, source))
                        count += 1
                        for lower in rows[row_index + 1:]:
                            for other in lower:
                                weaker = RegisterWrite(other, lane, reg, losing)
                                for writes in ((higher, weaker), (weaker, higher)):
                                    require(register_writeback(old, writes, mode1=mode), expected,
                                            (mode, lane, reg, source, other))
                                    count += 1
                for sources in (("dag1", "ureg"), ("alu_x", "alu_x")):
                    proposals = tuple(RegisterWrite(source, lane, reg, raw)
                                      for source, raw in zip(sources, (winning, losing)))
                    try:
                        register_writeback(old, proposals, mode1=mode)
                    except Unqualified:
                        refusals += 1
                    else:
                        raise AssertionError("unqualified winning-priority tie was accepted")
    # A tie between losing sources cannot affect the unique higher-priority write.
    proposals = (RegisterWrite("alu_x", 0, 0, losing), RegisterWrite("alu_x", 0, 0, 0),
                 RegisterWrite("dag2", 0, 0, winning))
    require(register_writeback(old, proposals), replaced(old, 0, 0, 0, winning), "losing tie")
    return count, refusals


def invalid_inputs():
    old = registers()
    calls = (
        lambda: register_writeback((), ()),
        lambda: register_writeback(old, (), mode1=-1),
        lambda: register_writeback(old, (RegisterWrite("unknown", 0, 0, 0),)),
        lambda: register_writeback(old, (RegisterWrite("dag1", 2, 0, 0),)),
        lambda: register_writeback(old, (RegisterWrite("dag1", 0, 16, 0),)),
        lambda: register_writeback(old, (RegisterWrite("dag1", 0, 0, 1 << 40),)),
        lambda: register_writeback(replaced(old, 0, 1, 15, -1), ()),
    )
    for call in calls:
        try:
            call()
        except ValueError:
            continue
        raise AssertionError("invalid writeback input was accepted")
