// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <sharc/core.hpp>

namespace sharc::detail {
inline constexpr std::uint8_t full_instruction_bits = 48;
inline constexpr std::uint8_t wide_instruction_bits = 32;
inline constexpr std::uint8_t short_instruction_bits = 16;
inline constexpr std::uint32_t maximum_pc = 0xffffff;

struct Instruction {
  std::uint32_t variant{}, address{};
  std::uint64_t word{};
  std::uint8_t type{}, bits{};
};

[[nodiscard]] std::vector<Instruction> decode(const CodeImage &image, std::string_view identity);
[[nodiscard]] std::string input_identity(const CodeImage &image, const CompileOptions &options);
[[noreturn]] void refuse(FaultReason reason,
                         std::string_view identity,
                         std::uint32_t variant,
                         std::uint32_t address,
                         std::string message,
                         std::uint64_t word = 0,
                         std::uint8_t bits = 0);
} // namespace sharc::detail
