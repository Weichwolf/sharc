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
inline constexpr std::size_t return_loop_tail_slot = loop_call_slot + 1;
inline constexpr unsigned loop_stack_capacity = 6;
inline constexpr std::size_t counted_tail_slot = return_loop_tail_slot + 1;
inline constexpr std::size_t short_counted_tail_slot = counted_tail_slot + loop_stack_capacity;
inline constexpr std::size_t short_parent_tail_slot = short_counted_tail_slot + 1;
inline constexpr std::size_t suspended_call_state_slot = short_parent_tail_slot + 1;
inline constexpr std::size_t interrupt_frame_slot = suspended_call_state_slot + 1;
inline constexpr std::size_t status_depth_slot = interrupt_frame_slot + 1;
inline constexpr std::size_t progress_cycle_slot = status_depth_slot + 1;
inline constexpr std::size_t interrupt_deadline_slot = progress_cycle_slot + 1;
inline constexpr std::size_t accumulator_slot = interrupt_deadline_slot + 1;
inline constexpr unsigned accumulator_banks = 2;
inline constexpr unsigned accumulator_limbs = 3;
inline constexpr std::size_t io_slot =
    accumulator_slot + (std::size_t{2} * accumulator_banks * accumulator_limbs);
inline constexpr std::size_t io_read_slot = io_slot + 1;
inline constexpr std::size_t io_write_slot = io_read_slot + 1;
inline constexpr std::size_t io_owner_slot = io_write_slot + 1;
inline constexpr std::size_t owner_events_slot = io_owner_slot + 1;
inline constexpr std::size_t code_owner_slot = owner_events_slot + 1;
inline constexpr std::size_t code_check_slot = code_owner_slot + 1;
inline constexpr std::size_t fifo_slot = code_check_slot + 1;
inline constexpr std::size_t fifo_pointer_slot = fifo_slot + 2;
inline constexpr std::size_t mode2_slot = fifo_pointer_slot + 2;
inline constexpr std::size_t px_slot = mode2_slot + 1;
inline constexpr std::size_t ustat_slot = px_slot + 1;
inline constexpr unsigned ustat_register_count = 4;
inline constexpr std::size_t mode_mask_slot = ustat_slot + ustat_register_count;
inline constexpr std::size_t flags_slot = mode_mask_slot + 1;
inline constexpr std::size_t interrupt_mask_slot = flags_slot + 1;
inline constexpr std::size_t interrupt_mask_pointer_slot = interrupt_mask_slot + 1;
inline constexpr std::size_t extended_interrupt_slot = interrupt_mask_pointer_slot + 1;
inline constexpr std::size_t astat_effect_slot = extended_interrupt_slot + 1;
inline constexpr std::size_t register_pointer_count = astat_effect_slot + 1;
inline constexpr std::uint8_t owner_yield_event = 1;
inline constexpr std::uint8_t code_write_event = 2;

[[nodiscard]] constexpr std::size_t mr_slot(unsigned lane, unsigned bank, unsigned limb) {
  return accumulator_slot + (((std::size_t{lane} * accumulator_banks) + bank) * accumulator_limbs) +
         limb;
}

// Three short-buffer words, the arithmetic refill, and all counted tails.
inline constexpr unsigned suspended_call_state_words = 4 + loop_stack_capacity + 2;
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
inline constexpr unsigned pc_stack_pointer_bits = 5;
inline constexpr std::uint32_t pc_stack_pointer_mask = (1U << pc_stack_pointer_bits) - 1;
// Bits in the packed native pipeline word, following the DAG and ASTAT history.
inline constexpr unsigned pending_pc_stack_shift = 21;
inline constexpr unsigned prior_pc_stack_shift = pending_pc_stack_shift + pc_stack_pointer_bits + 1;
inline constexpr std::uint32_t pending_pc_stack_valid = 1U << pc_stack_pointer_bits;
inline constexpr std::uint32_t pending_pc_stack_mask =
    (pending_pc_stack_valid | pc_stack_pointer_mask) << pending_pc_stack_shift;
inline constexpr std::uint32_t prior_pc_stack_mask = pc_stack_pointer_mask << prior_pc_stack_shift;
inline constexpr std::uint32_t interrupt_status_frame = 1U << 8;

enum ShortLoopWord : std::uint8_t {
  short_remaining,
  short_descriptor,
  short_addresses,
  short_fetch_progress,
  short_parent_remaining,
  short_parent_descriptor,
  short_state_words
};

inline constexpr std::uint64_t short_startup = 1ULL << 24;
inline constexpr std::uint64_t short_compact_count = 1ULL << 25;
inline constexpr std::uint64_t short_call_exit = 1ULL << 26;
inline constexpr std::uint64_t short_unbounded = ~std::uint64_t{0};
inline constexpr unsigned short_decode_shift = 24;
inline constexpr std::uint64_t short_addresses_live = 1ULL << 48;
inline constexpr std::uint32_t before_first_delay_slot = 1U << 25;
inline constexpr std::uint32_t before_second_delay_slot = 1U << 24;
inline constexpr std::uint32_t delayed_fetch_live = 1U << 26;
inline constexpr unsigned delayed_fetch_shift = 32;
inline constexpr std::uint32_t reverse8_mode = 1U << 0;
inline constexpr std::uint32_t reverse0_mode = 1U << 1;
inline constexpr std::uint32_t dag1_high_mode = 1U << 3;
inline constexpr std::uint32_t dag1_low_mode = 1U << 4;
inline constexpr std::uint32_t dag2_high_mode = 1U << 5;
inline constexpr std::uint32_t dag2_low_mode = 1U << 6;
inline constexpr std::uint32_t high_bank_mode = 1U << 7;
inline constexpr std::uint32_t low_bank_mode = 1U << 10;
inline constexpr std::uint32_t saturation_mode = 1U << 13;
inline constexpr std::uint32_t multiplier_swap_mode = 1U << 2;
inline constexpr std::uint32_t short_sign_mode = 1U << 14;
inline constexpr std::uint32_t truncation_mode = 1U << 15;
inline constexpr std::uint32_t rounding32_mode = 1U << 16;
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
  io_word,
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
  invalid_loop_abort,
  invalid_short_loop_call,
  sleeping,
  interrupt_return,
  invalid_interrupt_return,
  interrupt_return_slots,
  interrupt_return_redirect,
  invalid_fifo,
  unsupported_memory_width,
  unsupported_pc_stack_write,
  unsupported_pc_stack_pointer_write
};
using NativeEntry = void (*)(void **, std::uint64_t *);
using RegisterPointers = std::vector<void *>;
using ControlWords = std::array<std::uint64_t, control_count>;

struct NativeUnit {
  std::uint32_t variant{}, entry{}, mask{}, context{};
  std::string symbol;
};

struct LoopHandoff {
  std::uint32_t from{}, to{}, address{};
};

struct PreparedImage {
  CodeImage code;
  CompileOptions options;
  std::string identity;
  std::vector<std::byte> bitcode;
  std::vector<std::string> exports;
  std::vector<NativeUnit> units;
  std::vector<LoopHandoff> loop_handoffs;
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
[[nodiscard]] bool code_storage_declared(const CodeVariant &variant);
[[nodiscard]] bool changes_code(const CodeVariant &variant,
                                std::uint64_t payload,
                                MemorySpace space,
                                MemoryView view,
                                std::uint32_t address) noexcept;

[[nodiscard]] constexpr unsigned memory_bytes(MemoryView view) {
  switch (view) {
    case MemoryView::short_word: return 2;
    case MemoryView::normal_word: return 4;
    case MemoryView::long_word: return 8;
    case MemoryView::instruction: return 6;
    case MemoryView::extended: return 5;
    case MemoryView::packed_48: return 6;
  }
  return 0;
}
} // namespace sharc::detail
