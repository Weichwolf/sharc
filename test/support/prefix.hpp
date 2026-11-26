// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "check.hpp"
#include "isa.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sharc/core.hpp>
#include <span>

namespace fixture {
struct Prefix {
  sharc::Registers registers;
  std::uint64_t cycles;
};

inline void
verify_prefixes(sharc::Core &core, sharc::Core &replay, std::span<const Prefix> prefixes) {
  const auto start = core.save();
  for (std::size_t i = 0; i < prefixes.size(); ++i) {
    core.restore(start);
    const auto result = core.run(i + 1);
    const auto &[registers, cycles] = prefixes[i];
    check(fixture::completed(result, i + 1, cycles) && core.registers() == registers,
          "sequencer state differs at an instruction bound");
    replay.restore(core.save());
    if (i + 1 < prefixes.size()) {
      const auto &next = prefixes[i + 1];
      check(fixture::completed(replay.run(1), 1, next.cycles - cycles) &&
                replay.registers() == next.registers,
            "cross-Core snapshot lost the sequencer continuation");
    }
  }
  for (std::uint64_t deadline = 1; deadline <= prefixes.back().cycles; ++deadline) {
    core.restore(start);
    const auto found = std::ranges::find_if(
        prefixes, [deadline](const Prefix &prefix) { return prefix.cycles >= deadline; });
    const auto count = static_cast<std::uint64_t>(found - prefixes.begin()) + 1;
    check(fixture::completed(core.run_until(deadline), count, found->cycles) &&
              core.registers() == found->registers,
          "cycle deadline observed the wrong sequencer boundary");
  }
}
} // namespace fixture
