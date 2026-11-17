// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <span>

namespace bench {
struct Control {
  std::uint32_t state{}, accumulator{}, last_handler{};
  bool operator==(const Control &) const = default;
};

Control control(std::uint32_t transitions, Control initial);

std::uint32_t copy_checksum(std::span<const std::uint32_t> input,
                            std::span<std::uint32_t> output,
                            std::uint32_t passes);
} // namespace bench
