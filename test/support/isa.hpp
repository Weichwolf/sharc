// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fixture {
enum class Width : std::uint8_t { short_word = 16, wide_word = 32, full_word = 48 };
inline constexpr std::uint32_t code_address = 0x100;
void control_flow();
void computed_control_flow();
void memory_access();
void dag_addressing();

// Independently authored numerical encodings. No production decoder is included.
inline void append(sharc::CodeSpan &span, std::uint64_t word, Width width = Width::full_word) {
  const auto bits = static_cast<unsigned>(width);
  if (width != Width::short_word && width != Width::wide_word && width != Width::full_word) {
    throw std::invalid_argument("fixture instruction width must be 16, 32 or 48");
  }
  for (unsigned consumed = 0; consumed < bits && consumed <= 32; consumed += 16) {
    span.parcels.push_back(static_cast<std::uint16_t>(word >> (32 - consumed)));
  }
}

inline std::uint64_t immediate(unsigned reg, std::uint32_t value) {
  constexpr unsigned opcode_shift = 40;
  return (std::uint64_t{0x0f} << opcode_shift) | (std::uint64_t{reg} << 32) | value;
}

inline std::uint64_t add(unsigned destination, unsigned x, unsigned y) {
  // Type 2a; unconditional IF TRUE; ALU opcode 0x01, Rn, Rx, Ry.
  constexpr unsigned opcode_shift = 40;
  constexpr unsigned condition_shift = 33;
  constexpr unsigned true_condition = 31;
  return (std::uint64_t{0x01} << opcode_shift) |
         (std::uint64_t{true_condition} << condition_shift) | (std::uint64_t{1} << 12) |
         (std::uint64_t{destination} << 8) | (std::uint64_t{x} << 4) | y;
}

inline sharc::CodeImage image(sharc::InstructionSet set,
                              std::initializer_list<std::uint16_t> parcels,
                              std::uint32_t address = code_address) {
  sharc::CodeVariant variant;
  variant.id = 7;
  variant.spans.push_back({address, set, parcels});
  variant.entries.push_back(address);
  return {.variants = {std::move(variant)}, .transitions = {}};
}

inline sharc::CodeImage
image(sharc::InstructionSet set, std::uint64_t word, Width width = Width::full_word) {
  auto result = image(set, {});
  append(result.variants.front().spans.front(), word, width);
  return result;
}

struct Program {
  static constexpr std::uint32_t variant_id = 7, storage_id = 9;
  static constexpr unsigned word_bytes = 6, byte_bits = 8;
  sharc::CodeImage code;
  std::vector<std::byte> bytes;

  explicit Program(std::span<const std::uint64_t> words, std::uint32_t address = code_address)
      : code(image(sharc::InstructionSet::isa, {}, address)) {
    auto &variant = code.variants[0];
    for (const auto word : words) {
      append(variant.spans[0], word);
      for (unsigned byte = 0; byte < word_bytes; ++byte) {
        bytes.push_back(static_cast<std::byte>(word >> (byte * byte_bits)));
      }
    }
    variant.layout.regions.push_back({.space = sharc::MemorySpace::program,
                                      .view = sharc::MemoryView::instruction,
                                      .first = address,
                                      .count = words.size(),
                                      .storage_id = storage_id,
                                      .byte_offset = 0,
                                      .byte_stride = word_bytes,
                                      .readable = true,
                                      .writable = false,
                                      .initialized = true});
  }

  sharc::Memory memory() {
    return {.regions = {{.storage_id = storage_id, .bytes = bytes, .generation = 1}}, .io = {}};
  }
};
} // namespace fixture
