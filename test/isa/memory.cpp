// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <vector>

namespace {
using fixture::check;
constexpr std::uint32_t ram_address = 0x90000, ram_id = 11;
constexpr unsigned stride = 7, offset = 1, byte_bits = 8;
constexpr unsigned simd_bit = 21, low_data_bit = 10, high_data_bit = 7;
constexpr unsigned broadcast1_bit = 23, broadcast9_bit = 22;
constexpr unsigned lane_shift = 32, bank_shift = 24;
constexpr std::uint64_t raw_seed = 0x8001020355;
constexpr std::array<std::uint32_t, 2> old_status{0x12340000, 0x87650000};
constexpr std::array<std::uint32_t, 2> old_sticky{0xa5000240, 0x95012345};
constexpr std::uint32_t old_modifier = 0x12345678, old_length = 31;
constexpr std::byte padding{0xa5};
constexpr std::array<unsigned, 16> dag_bank_bits{4, 4, 4, 4, 3, 3, 3, 3, 6, 6, 6, 6, 5, 5, 5, 5};
constexpr std::array<std::uint32_t, 16> words{0x80000000,
                                              0x12345678,
                                              0xfedcba98,
                                              0x01234567,
                                              0xffffffff,
                                              0x00000000,
                                              0x87654321,
                                              0x01020304,
                                              0xff00ff00,
                                              0xaabbccdd,
                                              0x20000001,
                                              0x55443322,
                                              0x80808080,
                                              0x10203040,
                                              0xcafebabe,
                                              0xabcdef01};

struct Move {
  unsigned reg{}, index{};
  bool write{};
  std::int32_t displacement{};
};

std::uint64_t encoding(const Move &move) {
  constexpr unsigned space_shift = 44;
  constexpr unsigned index_shift = 41;
  constexpr unsigned direction_shift = 40;
  constexpr unsigned register_shift = 32;
  constexpr std::uint64_t prefix = 0xa00000000000;
  return prefix | (std::uint64_t{move.index / 8} << space_shift) |
         (std::uint64_t{move.index % 8} << index_shift) |
         (std::uint64_t{move.write} << direction_shift) |
         (std::uint64_t{move.reg} << register_shift) |
         static_cast<std::uint32_t>(move.displacement);
}

void put(std::vector<std::byte> &ram, unsigned index, std::uint32_t word) {
  for (unsigned byte = 0; byte < 4; ++byte) {
    ram[offset + (index * stride) + byte] = static_cast<std::byte>(word >> (byte * byte_bits));
  }
}

struct Context {
  unsigned index{};
  std::uint32_t mode{};
};

sharc::Registers initial(const Context &context) {
  const auto index = context.index;
  const auto mode = context.mode;
  sharc::Registers result;
  result.pc = fixture::code_address;
  result.mode1 = mode;
  for (unsigned bank = 0; bank < 2; ++bank) {
    for (unsigned i = 0; i < 16; ++i) {
      result.dag[bank][i] = {.index = ram_address + 3 + bank,
                             .modifier = old_modifier,
                             .length = old_length,
                             .base = ram_address};
      for (unsigned lane = 0; lane < 2; ++lane) {
        result.pe[lane].data[bank][i] = raw_seed + (std::uint64_t{lane} << lane_shift) +
                                        (std::uint64_t{bank} << bank_shift) +
                                        (std::uint64_t{i} << 8);
      }
    }
  }
  result.pe[0].astat = old_status[0];
  result.pe[1].astat = old_status[1];
  result.pe[0].stky = old_sticky[0];
  result.pe[1].stky = old_sticky[1];
  // Circular and reverse modes do not modify the pre-indexed I register.
  constexpr std::uint32_t circular = 1U << 24;
  result.mode1 |= circular;
  if (index == 0) { result.mode1 |= 2; }
  if (index == 8) { result.mode1 |= 1; }
  return result;
}

void expected(sharc::Registers &registers,
              unsigned index,
              std::vector<std::byte> &ram,
              unsigned completed) {
  const auto mode = registers.mode1;
  const unsigned bank = (mode >> dag_bank_bits[index]) & 1;
  const unsigned low = (mode >> low_data_bit) & 1;
  const unsigned high = (mode >> high_data_bit) & 1;
  const bool simd = (mode & (1U << simd_bit)) != 0;
  const bool broadcast = (index == 1 && (mode & (1U << broadcast1_bit)) != 0) ||
                         (index == 9 && (mode & (1U << broadcast9_bit)) != 0);
  const auto base = 3 + bank;
  const auto increment = broadcast ? 0U : 1U;
  const bool paired = simd || broadcast;
  if (completed >= 1) {
    registers.pe[0].data[low][0] = std::uint64_t{words[base - 2]} << byte_bits;
    if (paired) {
      registers.pe[1].data[low][0] = std::uint64_t{words[base - 2 + increment]} << byte_bits;
    }
  }
  if (completed >= 2) {
    registers.pe[1].data[high][8] = std::uint64_t{words[base]} << byte_bits;
    if (paired) {
      registers.pe[0].data[high][8] = std::uint64_t{words[base + increment]} << byte_bits;
    }
  }
  if (completed >= 3) {
    put(ram, base + 4, static_cast<std::uint32_t>(registers.pe[0].data[low][0] >> byte_bits));
    if (simd) {
      put(ram, base + 5, static_cast<std::uint32_t>(registers.pe[1].data[low][0] >> byte_bits));
    }
  }
  if (completed >= 4) {
    put(ram, base + 6, static_cast<std::uint32_t>(registers.pe[1].data[high][8] >> byte_bits));
    if (simd) {
      put(ram, base + 7, static_cast<std::uint32_t>(registers.pe[0].data[high][8] >> byte_bits));
    }
  }
  if (completed >= 5) {
    put(ram, base + 10, static_cast<std::uint32_t>(registers.pe[0].data[low][1] >> byte_bits));
    if (simd) {
      put(ram, base + 11, static_cast<std::uint32_t>(registers.pe[1].data[low][1] >> byte_bits));
    }
  }
  fixture::boundary(registers,
                    {.pc = registers.pc + completed, .clock = registers.emuclk + completed});
}

void memory_index(unsigned index, sharc::ExecutionMode numeric_mode) {
  const std::array code{encoding({.reg = 0, .index = index, .write = false, .displacement = -2}),
                        encoding({.reg = 0x58, .index = index, .write = false, .displacement = 0}),
                        encoding({.reg = 0, .index = index, .write = true, .displacement = 4}),
                        encoding({.reg = 0x58, .index = index, .write = true, .displacement = 6}),
                        encoding({.reg = 1, .index = index, .write = true, .displacement = 10})};
  fixture::Program program(code);
  auto &variant = program.code.variants[0];
  auto region = fixture::normal_region(ram_address, words.size(), ram_id);
  region.space = index < 8 ? sharc::MemorySpace::data : sharc::MemorySpace::program;
  region.byte_offset = offset;
  region.byte_stride = stride;
  variant.layout.regions.push_back(region);
  constexpr unsigned ram_size = 128;
  std::vector<std::byte> ram(ram_size, padding);
  for (unsigned i = 0; i < words.size(); ++i) { put(ram, i, words[i]); }
  const auto original_ram = ram;
  auto memory = program.memory(ram_id, ram);
  const auto native = fixture::compile(program.code, numeric_mode);
  for (unsigned flags = 0; flags < 32; ++flags) {
    const auto mode = ((flags & 1) << low_data_bit) | (((flags >> 1) & 1) << high_data_bit) |
                      (((flags >> 2) & 1) << dag_bank_bits[index]) |
                      (((flags >> 3) & 1) << simd_bit) |
                      (((flags >> 4) & 1) << (index == 9 ? broadcast9_bit : broadcast1_bit));
    sharc::Core core(memory, numeric_mode);
    const auto registers = initial({.index = index, .mode = mode});
    fixture::install(core, native, registers);
    const auto saved = core.save();
    for (unsigned count = 1; count <= code.size(); ++count) {
      auto final_registers = registers;
      auto final_ram = original_ram;
      expected(final_registers, index, final_ram, count);
      for (const bool cycle_bound : {false, true}) {
        core.restore(saved);
        std::ranges::copy(original_ram, ram.begin());
        const auto result = cycle_bound ? core.run_until(count) : core.run(count);
        check(fixture::completed(result, count, count) && core.registers() == final_registers &&
                  ram == final_ram,
              "pre-indexed memory changed data, extension, banks, flags, I or padding");
      }
    }
  }
}

struct AbsoluteMove {
  sharc::MemorySpace space;
  bool write;
  unsigned ureg;
  std::uint32_t address;
};

std::uint64_t encoding(const AbsoluteMove &move) {
  constexpr std::uint64_t prefix = 0x100000000000;
  constexpr unsigned program_shift = 41;
  constexpr unsigned write_shift = 40;
  constexpr unsigned ureg_shift = 32;
  return prefix | (std::uint64_t{move.space == sharc::MemorySpace::program} << program_shift) |
         (std::uint64_t{move.write} << write_shift) | (std::uint64_t{move.ureg} << ureg_shift) |
         move.address;
}

void expect_absolute(sharc::Registers &state, std::vector<std::byte> &ram, unsigned count) {
  const bool simd = (state.mode1 & (1U << simd_bit)) != 0;
  const auto low = (state.mode1 >> low_data_bit) & 1;
  const auto high = (state.mode1 >> high_data_bit) & 1;
  state.pe[0].data[low][0] = std::uint64_t{words[1]} << byte_bits;
  if (simd) { state.pe[1].data[low][0] = std::uint64_t{words[2]} << byte_bits; }
  if (count >= 2) {
    state.pe[1].data[high][8] = std::uint64_t{words[3]} << byte_bits;
    if (simd) { state.pe[0].data[high][8] = std::uint64_t{words[4]} << byte_bits; }
  }
  if (count >= 3) {
    put(ram, 7, words[1]);
    if (simd) { put(ram, 8, words[2]); }
  }
  if (count >= 4) {
    put(ram, 10, words[3]);
    if (simd) { put(ram, 11, words[4]); }
  }
  fixture::boundary(state, {.pc = state.pc + count, .clock = count});
}

void absolute_transfers(sharc::MemorySpace space, sharc::ExecutionMode mode) {
  constexpr unsigned shadow_high = 0x58;
  constexpr unsigned capacity = 128;
  constexpr unsigned selection_count = 32;
  constexpr auto unrelated_index = 0xfffffff0;
  const std::array code{
      encoding(AbsoluteMove{.space = space, .write = false, .ureg = 0, .address = ram_address + 1}),
      encoding(AbsoluteMove{
          .space = space, .write = false, .ureg = shadow_high, .address = ram_address + 3}),
      encoding(AbsoluteMove{.space = space, .write = true, .ureg = 0, .address = ram_address + 7}),
      encoding(AbsoluteMove{
          .space = space, .write = true, .ureg = shadow_high, .address = ram_address + 10})};
  fixture::Program program(code);
  auto region = fixture::normal_region(ram_address, words.size(), ram_id);
  region.space = space;
  region.byte_offset = offset;
  region.byte_stride = stride;
  program.code.variants.front().layout.regions.push_back(region);
  std::vector<std::byte> ram(capacity, padding);
  for (unsigned i = 0; i < words.size(); ++i) { put(ram, i, words[i]); }
  const auto seed = ram;
  const auto native = fixture::compile(program.code, mode);
  for (unsigned flags = 0; flags < selection_count; ++flags) {
    const auto controls = ((flags & 1) << low_data_bit) | (((flags >> 1) & 1) << high_data_bit) |
                          (((flags >> 2) & 1) << simd_bit) |
                          (((flags >> 3) & 1) << broadcast1_bit) |
                          (((flags >> 4) & 1) << broadcast9_bit);
    auto before = initial({.index = 0, .mode = controls});
    before.dag[0][0].index = unrelated_index;
    sharc::Core core(program.memory(ram_id, ram), mode);
    fixture::install(core, native, before);
    const auto start = core.save();
    for (unsigned count = 1; count <= code.size(); ++count) {
      auto expected = before;
      auto expected_ram = seed;
      expect_absolute(expected, expected_ram, count);
      for (const bool cycles : {false, true}) {
        core.restore(start);
        std::ranges::copy(seed, ram.begin());
        const auto result = cycles ? core.run_until(count) : core.run(count);
        check(fixture::completed(result, count, count) && core.registers() == expected &&
                  ram == expected_ram,
              "absolute transfer lost a bus, explicit lane, bank or precise prefix");
      }
    }
  }
}

constexpr std::uint32_t immediate_payload = 0x765432ab;
constexpr unsigned immediate_stride = 11;

std::uint64_t immediate_store(unsigned index) {
  // Store with M6 or M14 from the same DAG as the selected index.
  constexpr std::uint64_t opcode = 0x918000000000;
  constexpr unsigned index_shift = 41;
  constexpr unsigned program_shift = 37;
  return opcode | (std::uint64_t{index % 8} << index_shift) |
         (std::uint64_t{index / 8} << program_shift) | immediate_payload;
}

struct StoreView {
  sharc::MemoryView view;
  unsigned width, implicit_offset;
  std::uint64_t word;
};

void expect_immediate_bytes(std::vector<std::byte> &ram, unsigned index, const StoreView &view) {
  for (unsigned byte = 0; byte < view.width; ++byte) {
    ram[offset + (index * immediate_stride) + byte] =
        static_cast<std::byte>(view.word >> (byte * byte_bits));
  }
}

void immediate_stores(unsigned index, sharc::ExecutionMode mode, const StoreView &view) {
  constexpr unsigned capacity = 64;
  constexpr unsigned region_words = 5;
  constexpr unsigned controls = 8;
  constexpr unsigned circular_bit = 24;
  constexpr unsigned sticky_bit = 17;
  constexpr unsigned interrupt_bit = 20;
  constexpr std::uint32_t negative_two = 0xfffffffe;
  const std::array code{immediate_store(index)};
  fixture::Program program(code);
  program.code.variants.front().layout.regions.push_back(
      {.space = index < 8 ? sharc::MemorySpace::data : sharc::MemorySpace::program,
       .view = view.view,
       .first = ram_address,
       .count = region_words,
       .storage_id = ram_id,
       .byte_offset = offset,
       .byte_stride = immediate_stride,
       .initialized = true});
  const auto native = fixture::compile(program.code, mode);
  for (unsigned flags = 0; flags < controls; ++flags) {
    const auto bank = flags & 1;
    const bool simd = (flags & 2) != 0;
    const bool circular = (flags & 4) != 0;
    auto state =
        initial({.index = index,
                 .mode = (bank << dag_bank_bits[index]) | (std::uint32_t{simd} << simd_bit)});
    if (!circular) { state.mode1 &= ~(1U << circular_bit); }
    auto &selected = state.dag[bank][index];
    selected = {
        .index = ram_address + 1, .modifier = selected.modifier, .length = 2, .base = ram_address};
    state.dag[bank][index - 1].modifier = bank == 0 ? 2 : negative_two;
    std::vector<std::byte> ram(capacity, padding);
    auto expected_ram = ram;
    expect_immediate_bytes(expected_ram, 1, view);
    if (simd && view.implicit_offset != 0) {
      expect_immediate_bytes(expected_ram, 1 + view.implicit_offset, view);
    }
    sharc::Core core(program.memory(ram_id, ram), mode);
    fixture::install(core, native, state);
    if (circular) {
      state.pe[0].stky |= 1U << (sticky_bit + (index / 8));
      state.irptl |= 1U << (interrupt_bit + (index / 8));
    } else {
      selected.index = bank == 0 ? ram_address + 3 : ram_address - 1;
    }
    fixture::boundary(state, {.pc = state.pc + 1, .clock = 1});
    check(fixture::completed(core.run_until(1), 1, 1) && core.registers() == state &&
              ram == expected_ram,
          "immediate store lost a width, lane, physical DAG update or wrap effect");
  }
}
} // namespace

namespace fixture {
void memory_access() {
  for (unsigned index = 0; index < 16; ++index) {
    for (const auto mode : fixture::numeric_modes) { memory_index(index, mode); }
  }
  for (const auto mode : fixture::numeric_modes) {
    absolute_transfers(sharc::MemorySpace::data, mode);
    absolute_transfers(sharc::MemorySpace::program, mode);
    constexpr std::array views{StoreView{.view = sharc::MemoryView::normal_word,
                                         .width = 4,
                                         .implicit_offset = 1,
                                         .word = immediate_payload},
                               StoreView{.view = sharc::MemoryView::short_word,
                                         .width = 2,
                                         .implicit_offset = 2,
                                         .word = 0x32ab},
                               StoreView{.view = sharc::MemoryView::long_word,
                                         .width = 8,
                                         .implicit_offset = 0,
                                         .word = 0x765432ab765432ab},
                               StoreView{.view = sharc::MemoryView::extended,
                                         .width = 5,
                                         .implicit_offset = 0,
                                         .word = 0x765432ab00}};
    for (const auto &view : views) {
      immediate_stores(7, mode, view);
      immediate_stores(15, mode, view);
    }
  }
}
} // namespace fixture
