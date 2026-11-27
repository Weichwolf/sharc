// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace bench {
inline constexpr std::size_t fir_taps = 64;

// The input includes fir_taps - 1 history words. All words hold signed Q1.31.
void fir(std::span<const std::uint32_t> input,
         std::span<const std::uint32_t, fir_taps> coefficients,
         std::span<std::uint32_t> output);

struct Control {
  std::uint32_t state{}, accumulator{}, last_handler{};
  bool operator==(const Control &) const = default;
};

Control control(std::uint32_t transitions, Control initial);

std::uint32_t copy_checksum(std::span<const std::uint32_t> input,
                            std::span<std::uint32_t> output,
                            std::uint32_t passes);
} // namespace bench
