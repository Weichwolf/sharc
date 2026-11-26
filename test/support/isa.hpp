// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "check.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fixture {
enum class Width : std::uint8_t { short_word = 16, wide_word = 32, full_word = 48 };
inline constexpr std::array numeric_modes{sharc::ExecutionMode::strict,
                                          sharc::ExecutionMode::fast_fp32};
inline constexpr std::uint32_t code_address = 0x100;
void control_flow();
void computed_control_flow();
void register_transfers();
void rotations();
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

constexpr std::uint64_t immediate(unsigned reg, std::uint32_t value) {
  constexpr unsigned opcode_shift = 40;
  return (std::uint64_t{0x0f} << opcode_shift) | (std::uint64_t{reg} << 32) | value;
}

constexpr std::uint64_t add(unsigned destination, unsigned x, unsigned y) {
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

inline sharc::Fault refusal(const sharc::CodeImage &image, std::string_view message) {
  try {
    (void)sharc::Core::lower(image);
  } catch (const sharc::PreparationError &error) { return error.fault(); }
  throw std::runtime_error(std::string(message));
}

inline void verify_branch_fields(std::uint64_t opcode, std::span<const unsigned> reserved) {
  for (const auto bit : reserved) {
    const auto word = opcode | (std::uint64_t{1} << bit);
    const auto fault =
        refusal(image(sharc::InstructionSet::isa, word), "reserved branch bit was accepted");
    check(fault.reason == sharc::FaultReason::invalid_encoding && fault.instruction == word &&
              fault.address == code_address,
          "reserved branch lost fault reason, instruction or location");
  }
  for (const auto bit : {24U, 26U, 38U}) {
    const auto word = opcode | (std::uint64_t{1} << bit);
    (void)refusal(image(sharc::InstructionSet::isa, word),
                  "unimplemented branch modifier was accepted");
  }
}

inline sharc::RegionLayout
normal_region(std::uint32_t first, std::uint64_t words, std::uint32_t storage_id) {
  return {.first = first,
          .count = words,
          .storage_id = storage_id,
          .byte_stride = 4,
          .initialized = true};
}

struct Program {
  static constexpr std::uint32_t variant_id = 7, storage_id = 9;
  static constexpr unsigned word_bytes = 6, byte_bits = 8;
  sharc::CodeImage code;
  std::vector<std::byte> bytes;

  explicit Program(std::span<const std::uint64_t> words, std::uint32_t address = code_address) {
    auto &variant = code.variants.emplace_back();
    variant.id = variant_id;
    variant.entries.push_back(address);
    add_span(address, words);
  }

  void add_span(std::uint32_t address, std::span<const std::uint64_t> words) {
    auto &variant = code.variants.front();
    const auto offset = bytes.size();
    auto &span = variant.spans.emplace_back(
        address, sharc::InstructionSet::isa, std::vector<std::uint16_t>{});
    for (const auto word : words) {
      append(span, word);
      for (unsigned byte = 0; byte < word_bytes; ++byte) {
        bytes.push_back(static_cast<std::byte>(word >> (byte * byte_bits)));
      }
    }
    variant.layout.regions.push_back({.space = sharc::MemorySpace::program,
                                      .view = sharc::MemoryView::instruction,
                                      .first = address,
                                      .count = words.size(),
                                      .storage_id = storage_id,
                                      .byte_offset = offset,
                                      .byte_stride = word_bytes,
                                      .readable = true,
                                      .writable = false,
                                      .initialized = true});
  }

  sharc::Memory memory() {
    return {.regions = {{.storage_id = storage_id, .bytes = bytes, .generation = 1}}, .io = {}};
  }

  sharc::Memory memory(std::uint32_t ram_id, std::span<std::byte> ram) {
    auto result = memory();
    result.regions.push_back({.storage_id = ram_id, .bytes = ram, .generation = 1});
    return result;
  }
};

inline void fetch_addresses(sharc::Registers &state) {
  state.faddr = state.pc + 4;
  state.daddr = state.pc + 2;
}

struct Boundary {
  std::uint32_t pc;
  std::uint64_t clock;
};

inline void boundary(sharc::Registers &state, Boundary expected) {
  state.pc = expected.pc;
  fetch_addresses(state);
  state.emuclk = expected.clock;
}

inline sharc::NativeImage
compile(const sharc::CodeImage &image,
        sharc::ExecutionMode mode,
        sharc::CompileOptions::MemoryMode memory = sharc::CompileOptions::MemoryMode::exact) {
  return sharc::Core::compile(image, {.mode = mode, .memory_mode = memory, .callbacks = {}});
}

inline void install(sharc::Core &core,
                    const sharc::NativeImage &image,
                    const sharc::Registers &registers,
                    std::uint32_t variant = Program::variant_id) {
  core.set_registers(registers);
  (void)core.install(image, variant);
}

inline bool
completed(const sharc::RunResult &result, std::uint64_t instructions, std::uint64_t cycles) {
  return !result.has_fault &&
         result.progress == sharc::Progress{.instructions = instructions, .base_cycles = cycles};
}
} // namespace fixture
