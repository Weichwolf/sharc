// SPDX-License-Identifier: GPL-3.0-only
#include "decode.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <sharc/core.hpp>
#include <string_view>

namespace sharc::detail {
namespace {
constexpr unsigned opcode_shift = 12;
constexpr unsigned unit_shift = 20;
constexpr unsigned multifunc_shift = 16;
constexpr unsigned opcode_mask = 0xff;
constexpr unsigned compute_mask = 0x7fffff;

// Programming Reference 2.4, tables 12-3, 12-4, 12-11 and 12-12.
constexpr std::array<std::uint8_t, 46> alu_codes{
    0x01, 0x02, 0x05, 0x06, 0x09, 0x0a, 0x0b, 0x21, 0x22, 0x25, 0x26, 0x29, 0x2a, 0x30, 0x40, 0x41,
    0x42, 0x43, 0x61, 0x62, 0x63, 0x81, 0x82, 0x89, 0x8a, 0x91, 0x92, 0xa1, 0xa2, 0xa5, 0xad, 0xb0,
    0xbd, 0xc1, 0xc4, 0xc5, 0xc9, 0xca, 0xcd, 0xd9, 0xda, 0xdd, 0xe0, 0xe1, 0xe2, 0xe3};
constexpr std::array<std::uint8_t, 26> shifter_codes{
    0x00, 0x04, 0x08, 0x20, 0x24, 0x40, 0x44, 0x48, 0x4c, 0x50, 0x58, 0x64, 0x6c,
    0x70, 0x74, 0x7c, 0x80, 0x84, 0x88, 0x8c, 0x90, 0x94, 0xc0, 0xc4, 0xc8, 0xcc};
constexpr std::array<std::uint8_t, 23> multifunction_codes{
    0x04, 0x05, 0x06, 0x08, 0x09, 0x0a, 0x0c, 0x0d, 0x0e, 0x10, 0x11, 0x12,
    0x14, 0x15, 0x16, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};

bool multiplier_code(std::uint8_t opcode) {
  constexpr unsigned float_multiply = 0x30;
  constexpr unsigned clear_foreground = 0x14;
  constexpr unsigned clear_background = 0x16;
  constexpr unsigned round_first = 0x18;
  constexpr unsigned round_last = 0x1f;
  constexpr unsigned multiply_first = 0x40;
  constexpr unsigned accumulate_first = 0x80;
  if (opcode < 16 || opcode == float_multiply || opcode == clear_foreground ||
      opcode == clear_background || (opcode >= round_first && opcode <= round_last)) {
    return true;
  }
  if (opcode < multiply_first) { return false; }
  // Table 12-7 has rounded fractional forms, but no rounded integer form.
  if ((opcode & 1U) != 0 && (opcode & 8U) == 0) { return false; }
  // Plain multiplication has Rn/MRF/MRB destinations, but no Rn-from-MRB form.
  return opcode >= accumulate_first || ((opcode >> 1) & 3U) != 1;
}

template <std::size_t Size>
bool contains(const std::array<std::uint8_t, Size> &codes, std::uint8_t opcode) {
  return std::ranges::find(codes, opcode) != codes.end();
}

Compute short_compute(std::uint32_t raw) {
  // Page 12-12 compact operations normalize to the same operations as ISA.
  constexpr std::array<std::uint8_t, 16> opcodes{0x01,
                                                 0x02,
                                                 0x21,
                                                 0x0a,
                                                 0x43,
                                                 0x29,
                                                 0x2a,
                                                 0x70,
                                                 0x81,
                                                 0x82,
                                                 0xca,
                                                 0x8a,
                                                 0x40,
                                                 0x41,
                                                 0x42,
                                                 0x30};
  const auto operation = (raw >> 8) & 15;
  const auto destination = static_cast<std::uint8_t>((raw >> 4) & 15);
  const auto source = static_cast<std::uint8_t>(raw & 15);
  const bool unary =
      operation == 2 || operation == 4 || operation == 5 || operation == 6 || operation == 10;
  Compute result;
  result.kind = operation == 7 || operation == 15 ? ComputeKind::multiplier : ComputeKind::alu;
  result.opcode = opcodes[operation];
  result.outputs[0] = destination;
  result.inputs[0] = unary ? source : destination;
  result.inputs[1] = unary ? 0 : source;
  result.raw = raw;
  return result;
}

bool single_compute(Compute &result) {
  const auto unit = result.raw >> unit_shift;
  result.opcode = static_cast<std::uint8_t>((result.raw >> opcode_shift) & opcode_mask);
  result.outputs[0] = static_cast<std::uint8_t>((result.raw >> 8) & 15);
  result.inputs[0] = static_cast<std::uint8_t>((result.raw >> 4) & 15);
  result.inputs[1] = static_cast<std::uint8_t>(result.raw & 15);
  if (unit == 0) {
    constexpr unsigned dual_fixed = 0x70;
    constexpr unsigned dual_float = 0xf0;
    const auto high_opcode = result.opcode & 0xf0U;
    if (high_opcode == dual_fixed || high_opcode == dual_float) {
      result.kind = ComputeKind::dual_alu;
      result.outputs[1] = result.opcode & 15;
      result.opcode = static_cast<std::uint8_t>(high_opcode);
      return true;
    }
    result.kind = ComputeKind::alu;
    return contains(alu_codes, result.opcode);
  }
  if (unit == 1) {
    result.kind = ComputeKind::multiplier;
    return multiplier_code(result.opcode);
  }
  result.kind = ComputeKind::shifter;
  return unit == 2 && contains(shifter_codes, result.opcode);
}

bool multiple_compute(Compute &result) {
  constexpr unsigned mr_mask = 0x7e0000;
  constexpr unsigned mr_prefix = 0x400000;
  if ((result.raw & mr_mask) == mr_prefix) {
    constexpr std::array<std::uint8_t, 6> mr_parts{0, 1, 2, 4, 5, 6};
    result.kind = ComputeKind::mr_transfer;
    result.opcode = static_cast<std::uint8_t>((result.raw >> opcode_shift) & 15);
    result.outputs[0] = static_cast<std::uint8_t>((result.raw >> 8) & 15);
    result.inputs[0] = result.outputs[0];
    return contains(mr_parts, result.opcode);
  }
  result.inputs = {static_cast<std::uint8_t>((result.raw >> 6) & 3),
                   static_cast<std::uint8_t>(4 + ((result.raw >> 4) & 3)),
                   static_cast<std::uint8_t>(8 + ((result.raw >> 2) & 3)),
                   static_cast<std::uint8_t>(12 + (result.raw & 3))};
  result.outputs[0] = static_cast<std::uint8_t>((result.raw >> 12) & 15);
  result.outputs[1] = static_cast<std::uint8_t>((result.raw >> 8) & 15);
  if ((result.raw >> unit_shift) >= 6) {
    result.kind = ComputeKind::multiply_dual_alu;
    result.opcode = static_cast<std::uint8_t>(result.raw >> unit_shift);
    result.outputs[2] = static_cast<std::uint8_t>((result.raw >> 16) & 15);
    return true;
  }
  result.kind = ComputeKind::multiply_alu;
  constexpr unsigned multifunction_mask = 0x3f;
  result.opcode = static_cast<std::uint8_t>((result.raw >> multifunc_shift) & multifunction_mask);
  return contains(multifunction_codes, result.opcode);
}

bool immediate_compute(Compute &result, std::uint64_t word) {
  constexpr unsigned opcode_field_mask = 0x3f;
  constexpr unsigned data_extension_shift = 27;
  constexpr unsigned reserved_bit = 22;
  result.kind = ComputeKind::shift_immediate;
  result.opcode = static_cast<std::uint8_t>(((result.raw >> 16) & opcode_field_mask) << 2);
  result.outputs[0] = static_cast<std::uint8_t>((result.raw >> 4) & 15);
  result.inputs[0] = static_cast<std::uint8_t>(result.raw & 15);
  result.immediate = static_cast<std::uint16_t>((((word >> data_extension_shift) & 15) << 8) |
                                                ((result.raw >> 8) & opcode_mask));
  return (result.raw >> reserved_bit) == 0 && contains(shifter_codes, result.opcode);
}
} // namespace

Compute decode_compute(const Instruction &instruction, std::string_view identity) {
  constexpr std::array compute_types{1, 2, 3, 4, 5, 6, 7, 9, 10, 11};
  Compute result;
  if (std::ranges::find(compute_types, instruction.type) == compute_types.end()) { return result; }
  if (instruction.bits == short_instruction_bits) {
    if (instruction.type != 2) { return result; }
    constexpr unsigned short_compute_mask = 0xfff;
    return short_compute(static_cast<std::uint32_t>((instruction.word >> 32) & short_compute_mask));
  }
  if (instruction.bits == wide_instruction_bits && instruction.type != 2) { return result; }
  const auto shift = instruction.bits == wide_instruction_bits ? 16 : 0;
  result.raw = static_cast<std::uint32_t>((instruction.word >> shift) & compute_mask);
  if (result.raw == 0 && instruction.type != 6) { return result; }
  bool valid{};
  if (instruction.type == 6) {
    valid = immediate_compute(result, instruction.word);
  } else if ((result.raw >> unit_shift) < 4) {
    valid = single_compute(result);
  } else {
    valid = multiple_compute(result);
  }
  if (!valid) {
    refuse(FaultReason::invalid_encoding,
           identity,
           instruction.variant,
           instruction.address,
           "reserved COMPUTE opcode or modifier",
           instruction.word,
           instruction.bits);
  }
  return result;
}
} // namespace sharc::detail
