# SPDX-License-Identifier: GPL-3.0-only
"""Independent register-bank and parallel writeback expectations from PRM 2-5..7."""

from dataclasses import asdict
from itertools import product

from reference import IntegerResult, RegisterWrite, Unqualified, fixed, register_writeback, swap_writes


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


def swap_anchors():
    cases = (
        ("SISD swap moves PEy while its computation is inactive", 0, (True, False),
         0, 0x7FFFFFFF00, 0x1122334455, (0x1122334455, 0x7FFFFFFF00), (6, 0), (4, 0)),
        ("SIMD PEy half reads old PEx source and defeats its ALU write", 0x200000,
         (False, True), 1, 0x123456789A, 0x7FFFFFFF00,
         (0x123456789A, 0x123456789A), (0, 6), (0, 4)),
    )
    records = []
    for name, mode, selected, lane, x, y, outputs, flags, sticky in cases:
        x_reg, y_reg = 0, 1
        compute_reg, addend_reg = (0, 1) if lane == 0 else (1, 2)
        old = replaced(replaced(registers(), 0, 0, x_reg, x), 1, 0, y_reg, y)
        old = replaced(old, lane, 0, addend_reg, 0x100)
        # EQ is selected from these old AZ bits, before the ADD changes AZ.
        old_astat, old_stky = tuple(int(value) for value in selected), (0, 0)
        computed = fixed("add", old[lane][0][compute_reg], 0x100,
                         astat=old_astat[lane], stky=old_stky[lane])
        require(computed, IntegerResult(0x8000000000, 6, 4), name)
        proposals = (RegisterWrite("alu_x" if lane == 0 else "alu_y", lane, compute_reg, computed.raw),)
        proposals += swap_writes(old, x_reg, y_reg, mode1=mode, selected=selected)
        actual = register_writeback(old, proposals, mode1=mode)
        expected = replaced(replaced(old, 0, 0, x_reg, outputs[0]), 1, 0, y_reg, outputs[1])
        actual_flags, actual_sticky = list(old_astat), list(old_stky)
        actual_flags[lane], actual_sticky[lane] = computed.astat, computed.stky
        require((actual, tuple(actual_flags), tuple(actual_sticky)), (expected, flags, sticky), name)
        records.append(dict(name=name, kind="compute-and-data-swap", mode1=mode,
                            condition="eq", selected=selected, old_astat=old_astat, old_stky=old_stky,
                            x_register=x_reg, y_register=y_reg, old_registers=old,
                            compute=dict(lane=lane, operation="add", destination=compute_reg,
                                         left=compute_reg, right=addend_reg, expected=asdict(computed)),
                            proposals=[asdict(item) for item in proposals],
                            expected_registers=expected, expected_astat=flags, expected_stky=sticky))
    return records


def swap_lattice():
    # Every physical source has a distinct extension byte as well as word value.
    old = tuple(tuple(tuple(raw - 165 + lane * 64 + bank * 32 + reg
                            for reg, raw in enumerate(values)) for bank, values in enumerate(banks))
                for lane, banks in enumerate(registers()))
    bank_modes = ((0, 0, 0), (0x400, 1, 0), (0x80, 0, 1), (0x480, 1, 1))
    # Literal transfer truth tables from PRM 4-98. The SISD PEy half is
    # deliberately different from the existing computation-lane predicate.
    tables = (((False, False), (False, False), (True, True), (True, True)),
              ((False, False), (False, True), (True, False), (True, True)))
    count = 0
    axes = product(bank_modes, enumerate(tables), (0, 0xA5A5A5A5 & ~0x200480),
                   range(4), range(16), range(16))
    for (mode, low_bank, high_bank), (simd, table), noise, index, x_reg, y_reg in axes:
        context = mode | (simd << 21) | noise
        selected, admitted = (index >= 2, index % 2 == 1), table[index]
        x_bank, y_bank = (low_bank if reg < 8 else high_bank for reg in (x_reg, y_reg))
        x, y = old[0][x_bank][x_reg], old[1][y_bank][y_reg]
        expected = old
        if admitted[0]:
            expected = replaced(expected, 0, x_bank, x_reg, y)
        if admitted[1]:
            expected = replaced(expected, 1, y_bank, y_reg, x)
        writes = swap_writes(old, x_reg, y_reg, mode1=context, selected=selected)
        require(register_writeback(old, writes, mode1=context), expected,
                (context, selected, x_reg, y_reg))
        count += 1
    for args in ((-1, 0, (True, True)), (0, 16, (True, True)), (0, 1, (True,)),
                 (0, 1, (1, True))):
        try:
            swap_writes(old, args[0], args[1], selected=args[2])
        except ValueError:
            continue
        raise AssertionError("invalid swap operands or predicates were accepted")
    return count
