// SPDX-License-Identifier: GPL-3.0-only
#include "reference.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

std::uint32_t bench::copy_checksum(std::span<const std::uint32_t> input,
                                   std::span<std::uint32_t> output,
                                   std::uint32_t passes) {
  std::uint32_t checksum{};
  for (std::uint32_t pass = 0; pass < passes; ++pass) {
    for (std::size_t i = 0; i < input.size(); ++i) {
      const auto value = input[i];
      output[i] = value;
      checksum += value;
    }
  }
  return checksum;
}

bench::Control bench::control(std::uint32_t transitions, Control initial) {
  for (std::uint32_t index = 0; index < transitions; ++index) {
    const auto selected = initial.state & 7;
    initial.accumulator += selected;
    initial.accumulator ^= selected + 1;
    initial.state = (selected + 3) & 7;
    initial.accumulator ^= std::rotr(initial.state, 1);
    initial.accumulator += 3;
    initial.last_handler = selected;
  }
  return initial;
}
