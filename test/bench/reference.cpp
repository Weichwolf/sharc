// SPDX-License-Identifier: GPL-3.0-only
#include "reference.hpp"

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
