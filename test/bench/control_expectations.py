#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Independent unbounded-integer K4 result and nominal-cycle receipt."""

import hashlib
import json
import struct


TRANSITIONS = 1 << 20
MASK = (1 << 32) - 1
SUCCESSORS = (3, 4, 5, 6, 7, 0, 1, 2)


def expected():
    state, accumulator = 3, 0x2468ACE0
    visits = [0] * 8
    first = []
    overflow = False
    for index in range(TRANSITIONS):
        last = state
        visits[last] += 1
        total = accumulator + state
        overflow |= accumulator < 1 << 31 <= total
        mixed = (total & MASK) ^ (state + 1)
        state = SUCCESSORS[state]
        # Reconstruct a rotate-right-by-one from the low bit and quotient.
        mixed ^= (state // 2) + ((state % 2) * (1 << 31))
        accumulator = (mixed + 3) & MASK
        overflow |= mixed < 1 << 31 <= mixed + 3
        if index < 8:
            first.append([state, accumulator, last])
    output = struct.pack('<III', state, accumulator, last)
    assert output.hex() == '03000000e0acd02400000000'
    assert not overflow and visits == [TRANSITIONS // 8] * 8
    return dict(state=state, accumulator=accumulator, last_handler=last,
                signed_overflow_sticky=overflow, handler_visits=visits,
                first_eight=first, output_le_hex=output.hex(),
                output_sha256=hashlib.sha256(output).hexdigest(),
                guest_instructions=18 * TRANSITIONS + 1,
                guest_base_cycles=38 * TRANSITIONS - 2)


if __name__ == '__main__':
    print(json.dumps(expected(), indent=2))
