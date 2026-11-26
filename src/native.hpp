// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "decode.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sharc/core.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sharc::detail {
// Private generated-code ABI: arrays of pointers and numeric control words.
// Public Registers/State object layouts are never embedded in bitcode.
inline constexpr std::size_t physical_data_count = 64;
inline constexpr std::size_t astat_slot = physical_data_count;
inline constexpr std::size_t stky_slot = astat_slot + 2;
inline constexpr std::size_t pc_slot = stky_slot + 2;
inline constexpr std::size_t faddr_slot = pc_slot + 1;
inline constexpr std::size_t daddr_slot = faddr_slot + 1;
inline constexpr std::size_t clock_slot = daddr_slot + 1;
inline constexpr std::size_t pipeline_slot = clock_slot + 1;
inline constexpr std::size_t stack_slot = pipeline_slot + 1;
inline constexpr std::size_t stack_depth_slot = stack_slot + 1;
inline constexpr std::size_t interrupt_latch_slot = stack_depth_slot + 1;
inline constexpr std::size_t dag_index_slot = interrupt_latch_slot + 1;
inline constexpr std::size_t dag_fields = 4;
inline constexpr std::size_t mode_slot = dag_index_slot + (32 * dag_fields);
inline constexpr std::size_t delayed_target_slot = mode_slot + 1;
inline constexpr std::size_t loop_address_slot = delayed_target_slot + 1;
inline constexpr std::size_t loop_counter_slot = loop_address_slot + 1;
inline constexpr std::size_t loop_address_depth_slot = loop_counter_slot + 1;
inline constexpr std::size_t loop_counter_depth_slot = loop_address_depth_slot + 1;
inline constexpr std::size_t lcntr_slot = loop_counter_depth_slot + 1;
inline constexpr std::size_t loop_frames_slot = lcntr_slot + 1;
inline constexpr std::size_t loop_refill_slot = loop_frames_slot + 1;
inline constexpr std::size_t short_loop_slot = loop_refill_slot + 1;
inline constexpr std::size_t loop_call_slot = short_loop_slot + 1;
inline constexpr std::size_t delayed_loop_tail_slot = loop_call_slot + 1;
inline constexpr std::size_t counted_tail_slot = delayed_loop_tail_slot + 1;
inline constexpr std::size_t register_pointer_count = counted_tail_slot + 1;
inline constexpr unsigned loop_stack_capacity = 6;
inline constexpr std::uint32_t loop_pc_index_mask = 31;
inline constexpr std::uint32_t loop_frame_live = 1U << 5;
inline constexpr std::uint32_t loop_repeat_scheduled = 1U << 6;
inline constexpr unsigned loop_fetch_distance = 4;
inline constexpr unsigned loop_decode_distance = 2;
inline constexpr unsigned loop_refill_end_shift = 24;
inline constexpr std::uint64_t loop_refill_live = 1ULL << 48;
inline constexpr std::uint64_t counted_loop_tail = 1ULL << 49;
inline constexpr std::uint64_t loop_call_live = 1ULL << 63;
inline constexpr std::uint32_t pc_call_tag = 1U << 24;
inline constexpr std::uint32_t pc_sequencer_tag = 1U << 25;

enum ShortLoopWord : std::uint8_t {
  short_remaining,
  short_descriptor,
  short_addresses,
  short_fetch_progress,
  short_state_words
};

inline constexpr std::uint64_t short_startup = 1ULL << 24;
inline constexpr std::uint64_t short_compact_count = 1ULL << 25;
inline constexpr std::uint64_t short_unbounded = ~std::uint64_t{0};
inline constexpr unsigned short_decode_shift = 24;
inline constexpr std::uint64_t short_addresses_live = 1ULL << 48;
inline constexpr std::uint32_t before_first_delay_slot = 1U << 25;
inline constexpr std::uint32_t before_second_delay_slot = 1U << 24;
inline constexpr std::uint32_t dag1_high_mode = 1U << 3;
inline constexpr std::uint32_t dag1_low_mode = 1U << 4;
inline constexpr std::uint32_t dag2_high_mode = 1U << 5;
inline constexpr std::uint32_t dag2_low_mode = 1U << 6;
inline constexpr std::uint32_t high_bank_mode = 1U << 7;
inline constexpr std::uint32_t low_bank_mode = 1U << 10;
inline constexpr std::uint32_t saturation_mode = 1U << 13;
inline constexpr std::uint32_t truncation_mode = 1U << 15;
inline constexpr std::uint32_t simd_mode = 1U << 21;
inline constexpr std::uint32_t broadcast9_mode = 1U << 22;
inline constexpr std::uint32_t broadcast1_mode = 1U << 23;
inline constexpr std::uint32_t circular_mode = 1U << 24;

[[nodiscard]] constexpr std::uint32_t dag_bank_mode(unsigned index) {
  if (index < 4) { return dag1_low_mode; }
  if (index < 8) { return dag1_high_mode; }
  if (index < 12) { return dag2_low_mode; }
  return dag2_high_mode;
}

[[nodiscard]] constexpr std::size_t dag_slot(unsigned bank, unsigned index, unsigned field = 0) {
  return dag_index_slot + (((std::size_t{bank} * 16) + index) * dag_fields) + field;
}

enum ControlSlot : std::uint8_t {
  instruction_budget,
  cycle_budget,
  retired_instructions,
  retired_cycles,
  exit_reason,
  memory_address,
  memory_space,
  fault_instruction,
  resume_entry,
  control_count
};
enum class NativeExit : std::uint8_t {
  bound,
  undeclared,
  unsupported_stack,
  indirect,
  unsupported_memory,
  access_fault,
  code_alias,
  returned,
  unsupported_loop_stack,
  unsupported_loop_reentry,
  invalid_loop_abort
};
using NativeEntry = void (*)(void **, std::uint64_t *);
using RegisterPointers = std::vector<void *>;
using ControlWords = std::array<std::uint64_t, control_count>;

struct NativeUnit {
  std::uint32_t variant{}, entry{}, mask{}, context{};
  std::string symbol;
};

struct PreparedImage {
  CodeImage code;
  CompileOptions options;
  std::string identity;
  std::vector<std::byte> bitcode;
  std::vector<std::string> exports;
  std::vector<NativeUnit> units;
};

struct CompiledSymbols {
  NativeSymbols symbols;
  std::size_t native_bytes{};
  std::vector<NativeSymbols::TextRange> text_ranges;
  std::vector<NativeSymbols::Site> sites;
};

[[nodiscard]] std::shared_ptr<const PreparedImage> lower_image(const CodeImage &image,
                                                               const CompileOptions &options);
[[nodiscard]] std::vector<std::byte> serialize_image(const PreparedImage &image);
[[nodiscard]] std::shared_ptr<const PreparedImage> load_image(const CodeImage &code,
                                                              const CompileOptions &options,
                                                              std::span<const std::byte> artifact);
[[nodiscard]] CompiledSymbols compile_image(const PreparedImage &image);
void validate_code_storage(const PreparedImage &image, std::uint32_t variant, const Memory &memory);
void validate_memory_layout(const CodeVariant &variant, std::string_view identity);
[[nodiscard]] std::string_view complete_views_error(const Memory::Views &views);
[[nodiscard]] std::vector<void *>
direct_memory(const PreparedImage &image, std::uint32_t variant, const Memory &memory);
[[nodiscard]] bool aliases_code(const CodeVariant &variant, std::size_t region);
} // namespace sharc::detail
