// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstdint>
#include <sharc/core.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace sharc::detail {
inline constexpr std::uint8_t full_instruction_bits = 48;
inline constexpr std::uint8_t wide_instruction_bits = 32;
inline constexpr std::uint8_t short_instruction_bits = 16;
inline constexpr std::uint32_t maximum_pc = 0xffffff;

enum class ComputeKind : std::uint8_t {
  none,
  alu,
  multiplier,
  shifter,
  shift_immediate,
  dual_alu,
  multiply_alu,
  multiply_dual_alu,
  mr_transfer
};

struct Compute {
  ComputeKind kind{ComputeKind::none};
  std::uint8_t opcode{};
  std::array<std::uint8_t, 3> outputs{};
  std::array<std::uint8_t, 4> inputs{};
  std::uint32_t raw{};
  std::uint16_t immediate{};
};

struct Instruction {
  std::uint32_t variant{}, address{};
  std::uint64_t word{};
  std::uint8_t type{}, bits{};
  Compute compute;
  InstructionSet instruction_set{InstructionSet::isa};
};

struct MemoryMove {
  unsigned index{}, reg{}, lane{};
  MemorySpace space{};
  bool write{};
};

[[nodiscard]] MemoryMove memory_move(const Instruction &instruction);
void validate_options(const CompileOptions &options, std::string_view identity);

[[nodiscard]] Compute decode_compute(const Instruction &instruction, std::string_view identity);
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
