// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <span>

namespace bench {
std::uint32_t copy_checksum(std::span<const std::uint32_t> input,
                            std::span<std::uint32_t> output,
                            std::uint32_t passes);
} // namespace bench
