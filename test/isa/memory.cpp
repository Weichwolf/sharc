// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <utility>
#include <vector>

namespace {
using fixture::check;
constexpr std::uint32_t ram_address = 0x90000, ram_id = 11;
constexpr unsigned stride = 7, offset = 1, byte_bits = 8;
constexpr unsigned simd_bit = 21, low_data_bit = 10, high_data_bit = 7;
constexpr unsigned broadcast1_bit = 23, broadcast9_bit = 22;
constexpr unsigned lane_shift = 32, bank_shift = 24;
constexpr std::uint64_t raw_seed = 0x8001020355;
constexpr std::uint64_t forced_long = 1ULL << 39;
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

struct ForcedForm {
  sharc::MemorySpace space;
  bool indexed, odd;
};

constexpr unsigned explicit_register = 5;
constexpr unsigned neighboring_register = explicit_register ^ 1;
constexpr unsigned load_address = 2;
constexpr unsigned store_address = 8;
constexpr unsigned monitor = 1U << 21;
constexpr unsigned unaligned_sticky = 1U << 20;
constexpr unsigned illegal_input = 1U << 2;

void expect_forced(sharc::Registers &state,
                   std::vector<std::byte> &ram,
                   const ForcedForm &form,
                   unsigned count) {
  const auto bank = (state.mode1 >> low_data_bit) & 1U;
  const unsigned first = load_address + static_cast<unsigned>(form.odd);
  const unsigned target = store_address + static_cast<unsigned>(form.odd);
  const auto broadcast_bit =
      form.space == sharc::MemorySpace::data ? broadcast1_bit : broadcast9_bit;
  const bool broadcast = form.indexed && (state.mode1 & (1U << broadcast_bit)) != 0;
  auto &x = state.pe[0].data[bank];
  auto &y = state.pe[1].data[bank];
  x[explicit_register] = std::uint64_t{words[first]} << byte_bits;
  x[neighboring_register] = std::uint64_t{words[first + 1]} << byte_bits;
  if (broadcast) {
    y[explicit_register] = x[explicit_register];
    y[neighboring_register] = x[neighboring_register];
  }
  if (count == 2) {
    put(ram, target, static_cast<std::uint32_t>(y[explicit_register] >> byte_bits));
    put(ram, target + 1, static_cast<std::uint32_t>(y[neighboring_register] >> byte_bits));
  }
  if (form.odd && (state.mode2 & monitor) != 0) {
    state.pe[0].stky |= unaligned_sticky;
    state.irptl |= illegal_input;
  }
  fixture::boundary(state, {.pc = state.pc + count, .clock = count});
}

std::array<std::uint64_t, 2> forced_program(const ForcedForm &form, unsigned index) {
  constexpr unsigned shadow_register = 0x50 + explicit_register;
  const auto first = load_address + static_cast<unsigned>(form.odd);
  const auto target = store_address + static_cast<unsigned>(form.odd);
  if (form.indexed) {
    return {forced_long | encoding(Move{.reg = explicit_register,
                                        .index = index,
                                        .displacement = static_cast<std::int32_t>(first)}),
            forced_long | encoding(Move{.reg = shadow_register,
                                        .index = index,
                                        .write = true,
                                        .displacement = static_cast<std::int32_t>(target)})};
  }
  return {forced_long | encoding(AbsoluteMove{.space = form.space,
                                              .write = false,
                                              .ureg = explicit_register,
                                              .address = ram_address + first}),
          forced_long | encoding(AbsoluteMove{.space = form.space,
                                              .write = true,
                                              .ureg = shadow_register,
                                              .address = ram_address + target})};
}

void forced_transfers(const ForcedForm &form, sharc::ExecutionMode mode) {
  constexpr std::uint32_t configuration_count = 16;
  constexpr unsigned capacity = 128;
  const auto index = form.space == sharc::MemorySpace::data ? 1U : 9U;
  const auto code = forced_program(form, index);
  fixture::Program program(code);
  auto region = fixture::normal_region(ram_address, words.size(), ram_id);
  region.space = form.space;
  region.byte_offset = offset;
  region.byte_stride = stride;
  program.code.variants.front().layout.regions.push_back(region);
  const auto native = fixture::compile(program.code, mode);
  std::vector<std::byte> ram(capacity, padding);
  for (unsigned i = 0; i < words.size(); ++i) { put(ram, i, words[i]); }
  const auto seed = ram;
  for (std::uint32_t flags = 0; flags < configuration_count; ++flags) {
    const auto broadcast_bit =
        form.space == sharc::MemorySpace::data ? broadcast1_bit : broadcast9_bit;
    const auto bank = flags & 1;
    const auto controls = (bank << low_data_bit) | (bank << dag_bank_bits[index]) |
                          (((flags >> 1) & 1) << simd_bit) | (((flags >> 2) & 1) << broadcast_bit);
    auto state = initial({.index = index, .mode = controls});
    state.dag[bank][index].index = ram_address;
    state.mode2 = (flags & 8) != 0 ? monitor : 0;
    sharc::Core core(program.memory(ram_id, ram), mode);
    fixture::install(core, native, state);
    const auto start = core.save();
    for (unsigned count = 1; count <= code.size(); ++count) {
      auto expected = state;
      auto expected_ram = seed;
      expect_forced(expected, expected_ram, form, count);
      for (const bool cycles : {false, true}) {
        core.restore(start);
        std::ranges::copy(seed, ram.begin());
        const auto result = cycles ? core.run_until(count) : core.run(count);
        check(fixture::completed(result, count, count) && core.registers() == expected &&
                  ram == expected_ram,
              "forced long transfer lost its same-PE neighbor, address order or alignment event");
      }
    }
  }
}

struct PredicatedForm {
  std::uint64_t dm_read, program_bit, store_bit;
  unsigned destination, memory_register, explicit_lane;
  int modifier;
  bool postmodify, rotation;
};

constexpr std::uint64_t across_buses = 1ULL << 32;
constexpr std::uint64_t store_direction = 1ULL << 31;
constexpr std::uint64_t imm_program_bit = 1ULL << 40;
constexpr std::uint64_t imm_write_bit = 1ULL << 39;
constexpr std::array predicated_forms{
    PredicatedForm{.dm_read = 0x52c002001612,
                   .program_bit = across_buses,
                   .store_bit = store_direction,
                   .destination = 6,
                   .memory_register = 4,
                   .explicit_lane = 0,
                   .modifier = 2,
                   .postmodify = true,
                   .rotation = false},
    PredicatedForm{.dm_read = 0x42c002001612,
                   .program_bit = across_buses,
                   .store_bit = store_direction,
                   .destination = 6,
                   .memory_register = 4,
                   .explicit_lane = 0,
                   .modifier = -2,
                   .postmodify = false,
                   .rotation = false},
    PredicatedForm{.dm_read = 0x624012001612,
                   .program_bit = imm_program_bit,
                   .store_bit = imm_write_bit,
                   .destination = 6,
                   .memory_register = 4,
                   .explicit_lane = 0,
                   .modifier = 2,
                   .postmodify = true,
                   .rotation = false},
    PredicatedForm{.dm_read = 0x6201f2001612,
                   .program_bit = imm_program_bit,
                   .store_bit = imm_write_bit,
                   .destination = 6,
                   .memory_register = 4,
                   .explicit_lane = 0,
                   .modifier = -2,
                   .postmodify = false,
                   .rotation = false},
    PredicatedForm{.dm_read = 0x82c002020161,
                   .program_bit = across_buses,
                   .store_bit = store_direction,
                   .destination = 6,
                   .memory_register = 4,
                   .explicit_lane = 0,
                   .modifier = 2,
                   .postmodify = true,
                   .rotation = true},
    PredicatedForm{.dm_read = 0x52c02a001412,
                   .program_bit = across_buses,
                   .store_bit = store_direction,
                   .destination = 4,
                   .memory_register = 4,
                   .explicit_lane = 1,
                   .modifier = 2,
                   .postmodify = true,
                   .rotation = false},
};

struct PacketConfiguration {
  unsigned index, flags, predicates;
  bool write;
};

constexpr unsigned packet_index = 5;
constexpr unsigned packet_bank = 1;
constexpr unsigned packet_simd = 2;
constexpr unsigned packet_broadcast = 4;
constexpr unsigned packet_wrap = 8;

sharc::Registers initial_packet(const PredicatedForm &form, const PacketConfiguration &config) {
  const auto bank = config.flags & packet_bank;
  const auto bit = config.index == 1 ? broadcast1_bit : broadcast9_bit;
  const auto mode = (bank << low_data_bit) | (bank << dag_bank_bits[config.index]) |
                    (static_cast<std::uint32_t>((config.flags & packet_simd) != 0) << simd_bit) |
                    (static_cast<std::uint32_t>((config.flags & packet_broadcast) != 0) << bit);
  auto state = initial({.index = config.index, .mode = mode});
  state.dag[bank][config.index].index = ram_address + packet_index;
  state.dag[bank][config.index].base = ram_address;
  if ((config.flags & packet_wrap) != 0) {
    state.dag[bank][config.index].base += packet_index - 1;
    state.dag[bank][config.index].length = 2;
  }
  state.dag[bank][config.index + 2].modifier = static_cast<std::uint32_t>(form.modifier);
  constexpr std::array<std::uint64_t, 2> left{17, 20};
  constexpr std::array<std::uint64_t, 2> right{33, 36};
  for (unsigned lane = 0; lane < state.pe.size(); ++lane) {
    auto &pe = state.pe[lane];
    pe.data[bank][1] = (left[lane] << byte_bits) | 1;
    pe.data[bank][2] = (right[lane] << byte_bits) | 2;
    pe.astat = old_status[lane] | ((config.predicates >> lane) & 1);
  }
  return state;
}

void expect_packet(sharc::Registers &state,
                   std::vector<std::byte> &ram,
                   const PredicatedForm &form,
                   const PacketConfiguration &config) {
  const auto bank = config.flags & packet_bank;
  const bool broadcast = !config.write && (config.flags & packet_broadcast) != 0;
  const bool simd = (config.flags & packet_simd) != 0;
  const std::array original{state.pe[0].data[bank][form.memory_register],
                            state.pe[1].data[bank][form.memory_register]};
  const auto active = config.predicates & (simd ? 3U : 1U);
  auto first = packet_index;
  if (!form.postmodify) { first = static_cast<unsigned>(static_cast<int>(first) + form.modifier); }
  const auto transfer = !simd && broadcast && active != 0 ? 3U : active;
  constexpr std::array<std::uint64_t, 2> sums{50, 56};
  constexpr std::array<std::uint64_t, 2> rotations{34, 40};
  constexpr std::uint32_t alu_flags = 0x43f;
  constexpr std::uint32_t shifter_flags = 0x3800;
  for (unsigned lane = 0; lane < state.pe.size(); ++lane) {
    if ((active & (1U << lane)) == 0) { continue; }
    auto &pe = state.pe[lane];
    pe.data[bank][form.destination] = (form.rotation ? rotations[lane] : sums[lane]) << byte_bits;
    pe.astat &= ~(form.rotation ? shifter_flags : alu_flags);
  }
  for (unsigned lane = 0; lane < state.pe.size(); ++lane) {
    if ((transfer & (1U << lane)) == 0) { continue; }
    const auto address = first + (broadcast ? 0 : lane);
    const auto physical = lane ^ form.explicit_lane;
    if (config.write) {
      put(ram, address, static_cast<std::uint32_t>(original[physical] >> byte_bits));
    } else {
      state.pe[physical].data[bank][form.memory_register] = std::uint64_t{words[address]}
                                                            << byte_bits;
    }
  }
  if (form.postmodify && active != 0 && (config.flags & packet_wrap) == 0) {
    state.dag[bank][config.index].index += static_cast<std::uint32_t>(form.modifier);
  }
  fixture::boundary(state, {.pc = state.pc + 1, .clock = 1});
}

void predicated_packets(const PredicatedForm &form,
                        sharc::MemorySpace space,
                        sharc::ExecutionMode mode,
                        bool write) {
  constexpr unsigned capacity = 128;
  constexpr unsigned configurations = 16;
  constexpr unsigned predicate_count = 4;
  const auto index = space == sharc::MemorySpace::data ? 1U : 9U;
  const auto word = form.dm_read | (space == sharc::MemorySpace::program ? form.program_bit : 0) |
                    (write ? form.store_bit : 0);
  const std::array code{word, std::uint64_t{0}};
  fixture::Program program(code);
  auto region = fixture::normal_region(ram_address, words.size(), ram_id);
  region.space = space;
  region.byte_offset = offset;
  region.byte_stride = stride;
  program.code.variants.front().layout.regions.push_back(region);
  const auto native = fixture::compile(program.code, mode);
  std::vector<std::byte> ram(capacity, padding);
  for (unsigned i = 0; i < words.size(); ++i) { put(ram, i, words[i]); }
  const auto seed = ram;
  sharc::Core core(program.memory(ram_id, ram), mode);
  (void)core.install(native, fixture::Program::variant_id);
  const auto beginning = core.save();
  for (unsigned flags = 0; flags < configurations; ++flags) {
    for (unsigned predicates = 0; predicates < predicate_count; ++predicates) {
      const PacketConfiguration config{
          .index = index, .flags = flags, .predicates = predicates, .write = write};
      auto state = initial_packet(form, config);
      auto expected_ram = seed;
      expect_packet(state, expected_ram, form, config);
      for (const bool cycles : {false, true}) {
        core.restore(beginning);
        core.set_registers(initial_packet(form, config));
        std::ranges::copy(seed, ram.begin());
        const auto count = cycles ? 1U : 2U;
        const auto result = cycles ? core.run_until(count) : core.run(count);
        auto final_state = state;
        fixture::boundary(final_state, {.pc = fixture::code_address + count, .clock = count});
        check(fixture::completed(result, count, count) && core.registers() == final_state &&
                  ram == expected_ram,
              "predicated memory packet lost a lane, old operand, priority, or index update");
      }
    }
  }
}

void predicated_packets(sharc::ExecutionMode mode) {
  constexpr std::array spaces{sharc::MemorySpace::data, sharc::MemorySpace::program};
  constexpr std::array directions{false, true};
  for (const auto &form : predicated_forms) {
    for (const auto space : spaces) {
      for (const bool write : directions) { predicated_packets(form, space, mode, write); }
    }
  }
}

void predicated_short_lane(sharc::ExecutionMode mode) {
  constexpr std::uint64_t load = 0x52c002000000; // IF EQ R4 = DM(I1,M3).
  constexpr unsigned short_sign = 1U << 14;
  constexpr std::uint64_t signed_word = 0xffff9a8000;
  const std::array code{load};
  fixture::Program program(code);
  program.code.variants.front().layout.regions.push_back({.view = sharc::MemoryView::short_word,
                                                          .first = ram_address + 2,
                                                          .count = 1,
                                                          .storage_id = ram_id,
                                                          .byte_stride = 2,
                                                          .initialized = true});
  constexpr std::array contents{std::byte{0x80}, std::byte{0x9a}};
  auto ram = contents;
  const auto native = fixture::compile(program.code, mode);
  sharc::Core core(program.memory(ram_id, ram), mode);
  auto expected = initial({.index = 1, .mode = (1U << simd_bit) | short_sign});
  expected.pe[1].astat |= 1;
  expected.dag[0][1].index = ram_address;
  expected.dag[0][1].length = 0;
  expected.dag[0][3].modifier = 1;
  fixture::install(core, native, expected);
  expected.pe[1].data[0][4] = signed_word;
  ++expected.dag[0][1].index;
  fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 1});
  check(fixture::completed(core.run(1), 1, 1) && core.registers() == expected,
        "only the selected implicit short-word lane should transfer and sign-extend");
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

enum class RegisterMemoryForm : std::uint8_t { absolute, immediate, pre, post };

struct RegisterMemoryCase {
  RegisterMemoryForm form;
  sharc::MemorySpace space;
  sharc::MemoryView view;
  unsigned reg, width;
  bool force{};
};

constexpr unsigned dag_cell = 2;
constexpr unsigned dag_capture_count = 8;
constexpr unsigned short_sign_bit = 14;
constexpr std::uint32_t dag_low_word = 0xf0e19a80;
constexpr std::uint32_t dag_high_word = 0x12345678;
constexpr std::uint32_t dag_store_low = 0xbd868ac3;
constexpr std::uint32_t dag_store_high = 0x36bdf0a1;
constexpr unsigned combined_px = 0x6b;
constexpr unsigned short_bits = 16;
constexpr std::uint64_t px48_memory_word = 0x89ab76543210;

std::uint64_t register_memory_encoding(const RegisterMemoryCase &item, unsigned index, bool write) {
  switch (item.form) {
    case RegisterMemoryForm::absolute:
      return (item.force ? forced_long : 0) |
             encoding(AbsoluteMove{.space = item.space,
                                   .write = write,
                                   .ureg = item.reg,
                                   .address = ram_address + dag_cell});
    case RegisterMemoryForm::immediate:
      return (item.force ? forced_long : 0) |
             encoding(
                 Move{.reg = item.reg, .index = index, .write = write, .displacement = dag_cell});
    case RegisterMemoryForm::pre:
    case RegisterMemoryForm::post: {
      // IF EQ ... (I1,M3) or (M3,I1), with I9 and M11 on the program bus.
      constexpr std::uint64_t indexed_transfer = 0x42c000000000;
      constexpr std::uint64_t postmodify = 1ULL << 44;
      constexpr std::uint64_t long_override = 1ULL << 30;
      constexpr unsigned ureg_shift = 23;
      return indexed_transfer | (item.form == RegisterMemoryForm::post ? postmodify : 0) |
             (item.space == sharc::MemorySpace::program ? across_buses : 0) |
             (write ? store_direction : 0) | (item.force ? long_override : 0) |
             (std::uint64_t{item.reg} << ureg_shift);
    }
  }
  std::unreachable();
}

std::uint32_t &dag_operand(sharc::Registers &state, unsigned bank, unsigned reg) {
  constexpr unsigned group_shift = 4;
  constexpr std::array fields{&sharc::AddressGenerator::index,
                              &sharc::AddressGenerator::modifier,
                              &sharc::AddressGenerator::length,
                              &sharc::AddressGenerator::base};
  return state.dag[bank][reg & 15].*fields[(reg >> group_shift) - 1];
}

void put_register_word(std::vector<std::byte> &ram,
                       unsigned index,
                       const RegisterMemoryCase &item,
                       std::uint64_t word) {
  for (unsigned byte = 0; byte < item.width; ++byte) {
    ram[offset + (index * immediate_stride) + byte] =
        static_cast<std::byte>(word >> (byte * byte_bits));
  }
}

bool narrow_px(const RegisterMemoryCase &item) {
  return item.reg == combined_px &&
         (item.view == sharc::MemoryView::short_word || item.view == sharc::MemoryView::packed_48);
}

void put_px48(std::vector<std::byte> &ram, const RegisterMemoryCase &item, std::uint64_t word) {
  constexpr unsigned cells = 3;
  if (item.view == sharc::MemoryView::short_word) {
    for (unsigned cell = 0; cell < cells; ++cell) {
      put_register_word(ram, dag_cell + cell, item, word >> (short_bits * cell));
    }
  } else {
    put_register_word(ram, dag_cell, item, word);
  }
}

std::vector<std::byte> register_memory_seed(const RegisterMemoryCase &item) {
  std::vector<std::byte> ram(offset + (dag_capture_count * immediate_stride), padding);
  if (narrow_px(item)) {
    put_px48(ram, item, px48_memory_word);
  } else if (item.view == sharc::MemoryView::long_word) {
    constexpr std::uint64_t word = (std::uint64_t{dag_high_word} << 32) | dag_low_word;
    put_register_word(ram, dag_cell, item, word);
  } else if (item.view == sharc::MemoryView::extended) {
    constexpr std::uint64_t word = (std::uint64_t{dag_low_word} << byte_bits) | 0xaf;
    put_register_word(ram, dag_cell, item, word);
  } else {
    put_register_word(ram, dag_cell, item, dag_low_word);
    const auto step = item.view == sharc::MemoryView::short_word ? 2U : 1U;
    put_register_word(ram, dag_cell + step, item, dag_high_word);
  }
  return ram;
}

void expect_dag_store(std::vector<std::byte> &ram,
                      const RegisterMemoryCase &item,
                      unsigned active) {
  if (active == 0) { return; }
  if (item.view == sharc::MemoryView::long_word) {
    constexpr std::uint64_t word = (std::uint64_t{dag_store_high} << 32) | dag_store_low;
    put_register_word(ram, dag_cell, item, word);
  } else if (item.force) {
    put_register_word(ram, dag_cell, item, dag_store_low);
    put_register_word(ram, dag_cell + 1, item, dag_store_high);
  } else if (item.view == sharc::MemoryView::extended) {
    put_register_word(ram, dag_cell, item, std::uint64_t{dag_store_low} << byte_bits);
  } else {
    const auto step = item.view == sharc::MemoryView::short_word ? 2U : 1U;
    if ((active & 1) != 0) { put_register_word(ram, dag_cell, item, dag_store_low); }
    if ((active & 2) != 0) { put_register_word(ram, dag_cell + step, item, dag_store_low); }
  }
}

void expect_dag_load(sharc::Registers &state, const RegisterMemoryCase &item, unsigned bank) {
  auto loaded = dag_low_word;
  if (item.view == sharc::MemoryView::short_word) {
    constexpr std::uint32_t short_signed = 0xffff9a80;
    constexpr std::uint32_t short_unsigned = 0x9a80;
    loaded = (state.mode1 & (1U << short_sign_bit)) != 0 ? short_signed : short_unsigned;
  }
  dag_operand(state, bank, item.reg) = loaded;
  if (item.force || item.view == sharc::MemoryView::long_word) {
    dag_operand(state, bank, item.reg ^ 1) = dag_high_word;
  }
}

struct RegisterMemoryControls {
  unsigned index, flags, predicates;
  bool write;
};

sharc::Registers dag_memory_state(const RegisterMemoryCase &item,
                                  const RegisterMemoryControls &controls) {
  const auto address_bank = controls.flags & 1;
  const auto register_bank = (controls.flags >> 1) & 1;
  const auto simd = (controls.flags >> 2) & 1;
  const auto modifiers = (controls.flags >> 3) & 1;
  const auto broadcast =
      item.space == sharc::MemorySpace::program ? broadcast9_bit : broadcast1_bit;
  const auto mode = (address_bank << dag_bank_bits[controls.index]) |
                    (register_bank << dag_bank_bits[item.reg & 15]) | (simd << simd_bit) |
                    (modifiers << broadcast) | (modifiers << short_sign_bit);
  auto state = initial({.index = controls.index, .mode = mode});
  if (controls.write) { state.pc += 2; }
  auto &address = state.dag[address_bank][controls.index];
  address.index = ram_address + (item.form == RegisterMemoryForm::post ? dag_cell : 0);
  address.length = 0;
  state.dag[address_bank][controls.index + 2].modifier = dag_cell;
  state.pe[0].astat = old_status[0] | (controls.predicates & 1);
  state.pe[1].astat = old_status[1] | (controls.predicates >> 1);
  dag_operand(state, register_bank, item.reg) = dag_store_low;
  dag_operand(state, register_bank, item.reg ^ 1) = dag_store_high;
  return state;
}

void verify_dag_memory_case(sharc::Core &core,
                            std::vector<std::byte> &ram,
                            const sharc::Snapshot &beginning,
                            const RegisterMemoryCase &item,
                            const RegisterMemoryControls &controls) {
  const auto before = dag_memory_state(item, controls);
  const auto lanes = (before.mode1 & (1U << simd_bit)) != 0 ? 3U : 1U;
  const auto active = controls.predicates & lanes;
  auto expected = before;
  const auto seed = register_memory_seed(item);
  auto expected_ram = seed;
  if (controls.write) {
    expect_dag_store(expected_ram, item, active);
  } else if (active != 0) {
    expect_dag_load(expected, item, (controls.flags >> 1) & 1);
  }
  if (item.form == RegisterMemoryForm::post && active != 0) {
    expected.dag[controls.flags & 1][controls.index].index += dag_cell;
  }
  for (const bool bound : {false, true}) {
    core.restore(beginning);
    core.set_registers(before);
    std::ranges::copy(seed, ram.begin());
    const auto count = bound ? 1U : 2U;
    const auto report = bound ? core.run_until(count) : core.run(count);
    fixture::boundary(expected, {.pc = before.pc + count, .clock = count});
    check(fixture::completed(report, count, count) && core.registers() == expected &&
              ram == expected_ram,
          "DAG memory transfer disagreed on bank, addressing, predicate, width or register pair");
  }
}

void dag_memory_access(const RegisterMemoryCase &item, sharc::ExecutionMode mode) {
  const auto index = item.space == sharc::MemorySpace::program ? 9U : 1U;
  const std::array program_words{register_memory_encoding(item, index, false),
                                 std::uint64_t{0},
                                 register_memory_encoding(item, index, true),
                                 std::uint64_t{0}};
  fixture::Program program(program_words);
  auto &variant = program.code.variants.front();
  variant.entries.push_back(fixture::code_address + 2);
  variant.layout.regions.push_back({.space = item.space,
                                    .view = item.view,
                                    .first = ram_address,
                                    .count = dag_capture_count,
                                    .storage_id = ram_id,
                                    .byte_offset = offset,
                                    .byte_stride = immediate_stride,
                                    .initialized = true});
  const auto native = fixture::compile(program.code, mode);
  auto ram = register_memory_seed(item);
  sharc::Core core(program.memory(ram_id, ram), mode);
  fixture::install(
      core,
      native,
      dag_memory_state(item, {.index = index, .flags = 0, .predicates = 3, .write = false}));
  const auto beginning = core.save();
  const bool conditional =
      item.form == RegisterMemoryForm::pre || item.form == RegisterMemoryForm::post;
  for (unsigned flags = 0; flags < 16; ++flags) {
    for (unsigned predicates = conditional ? 0U : 3U; predicates < 4; ++predicates) {
      for (const bool write : {false, true}) {
        verify_dag_memory_case(
            core,
            ram,
            beginning,
            item,
            {.index = index, .flags = flags, .predicates = predicates, .write = write});
      }
    }
  }
}

void dag_memory_access(sharc::ExecutionMode mode) {
  using enum RegisterMemoryForm;
  using enum sharc::MemorySpace;
  using enum sharc::MemoryView;
  constexpr std::array cases{
      RegisterMemoryCase{
          .form = absolute, .space = data, .view = normal_word, .reg = 0x1d, .width = 4},
      RegisterMemoryCase{
          .form = absolute, .space = program, .view = extended, .reg = 0x4d, .width = 5},
      RegisterMemoryCase{
          .form = immediate, .space = data, .view = short_word, .reg = 0x3d, .width = 2},
      RegisterMemoryCase{
          .form = immediate, .space = program, .view = long_word, .reg = 0x45, .width = 8},
      RegisterMemoryCase{.form = pre, .space = data, .view = normal_word, .reg = 0x2d, .width = 4},
      RegisterMemoryCase{
          .form = pre, .space = program, .view = short_word, .reg = 0x35, .width = 2},
      RegisterMemoryCase{
          .form = post, .space = data, .view = normal_word, .reg = 0x4c, .width = 4, .force = true},
      RegisterMemoryCase{
          .form = post, .space = program, .view = long_word, .reg = 0x15, .width = 8},
  };
  for (const auto &item : cases) { dag_memory_access(item, mode); }
}

constexpr unsigned first_px_half = 0x6c, second_px_half = 0x6d;
constexpr std::array user_status_ports{0x70U, 0x71U, 0x7eU, 0x7fU};
constexpr std::uint64_t px_word_mask = 0xffffffff;
constexpr std::array<std::uint32_t, 4> system_status_seed{
    0x1357de31, 0xd8b6eb90, 0xfca7b821, 0x233467f0};

std::uint32_t system_word(const sharc::Registers &state, unsigned reg) {
  if (reg == first_px_half) { return static_cast<std::uint32_t>(state.px); }
  if (reg == second_px_half) { return static_cast<std::uint32_t>(state.px >> 32); }
  const auto *const entry = std::ranges::find(user_status_ports, reg);
  return state.ustat[static_cast<std::size_t>(entry - user_status_ports.begin())];
}

struct SystemWord {
  unsigned reg;
  std::uint32_t word;
};

void set_system_word(sharc::Registers &state, SystemWord transfer) {
  const auto [reg, word] = transfer;
  constexpr unsigned bits = 32;
  if (reg == first_px_half || reg == second_px_half) {
    const auto shift = reg == first_px_half ? 0U : bits;
    state.px = (state.px & ~(px_word_mask << shift)) | (std::uint64_t{word} << shift);
    return;
  }
  const auto *const entry = std::ranges::find(user_status_ports, reg);
  state.ustat[static_cast<std::size_t>(entry - user_status_ports.begin())] = word;
}

bool system_memory_wide(const RegisterMemoryCase &item) {
  return item.force || item.view == sharc::MemoryView::long_word;
}

void expect_system_store(const sharc::Registers &state,
                         std::vector<std::byte> &ram,
                         const RegisterMemoryCase &item,
                         unsigned active) {
  if (active == 0) { return; }
  if (narrow_px(item)) {
    put_px48(ram, item, state.px >> short_bits);
    return;
  }
  const auto explicit_reg = item.reg == combined_px ? first_px_half : item.reg;
  const auto first = system_word(state, explicit_reg);
  const auto second = system_word(state, explicit_reg ^ 1);
  if (item.view == sharc::MemoryView::long_word) {
    put_register_word(ram, dag_cell, item, (std::uint64_t{second} << 32) | first);
  } else if (item.force) {
    put_register_word(ram, dag_cell, item, first);
    put_register_word(ram, dag_cell + 1, item, second);
  } else {
    const auto step = item.view == sharc::MemoryView::short_word ? 2U : 1U;
    if ((active & 1) != 0) { put_register_word(ram, dag_cell, item, first); }
    if ((active & 2) != 0) { put_register_word(ram, dag_cell + step, item, second); }
  }
}

void expect_system_load(sharc::Registers &state,
                        const RegisterMemoryCase &item,
                        unsigned active,
                        bool broadcast) {
  if (narrow_px(item)) {
    if (broadcast ? active != 0 : (active & 1) != 0) { state.px = px48_memory_word << short_bits; }
    return;
  }
  const auto explicit_reg = item.reg == combined_px ? first_px_half : item.reg;
  if (system_memory_wide(item)) {
    if (item.form == RegisterMemoryForm::absolute && item.reg != combined_px) {
      set_system_word(state, {.reg = explicit_reg, .word = dag_high_word});
    } else if ((active & 2) != 0 && broadcast) {
      set_system_word(state, {.reg = explicit_reg, .word = dag_high_word});
      set_system_word(state, {.reg = explicit_reg ^ 1, .word = dag_low_word});
    } else if ((active & 1) != 0) {
      set_system_word(state, {.reg = explicit_reg, .word = dag_low_word});
      set_system_word(state, {.reg = explicit_reg ^ 1, .word = dag_high_word});
    }
    return;
  }
  auto first = dag_low_word;
  auto second = broadcast ? dag_low_word : dag_high_word;
  if (item.view == sharc::MemoryView::short_word) {
    constexpr std::uint32_t short_word_mask = 0xffff;
    first &= short_word_mask;
    second &= short_word_mask;
  }
  if ((active & 1) != 0) { set_system_word(state, {.reg = explicit_reg, .word = first}); }
  if ((active & 2) != 0) { set_system_word(state, {.reg = explicit_reg ^ 1, .word = second}); }
}

sharc::Registers system_memory_state(const RegisterMemoryCase &item,
                                     const RegisterMemoryControls &controls) {
  constexpr std::uint64_t px_seed = 0x89abcdefdeadbeef;
  const auto broadcast =
      item.space == sharc::MemorySpace::program ? broadcast9_bit : broadcast1_bit;
  const auto mode = ((controls.flags & 1) << simd_bit) | (((controls.flags >> 1) & 1) << broadcast);
  auto state = initial({.index = controls.index, .mode = mode});
  if (controls.write) { state.pc += 2; }
  state.px = px_seed;
  state.ustat = system_status_seed;
  auto &address = state.dag[0][controls.index];
  address.index = ram_address + (item.form == RegisterMemoryForm::post ? dag_cell : 0);
  address.length = 0;
  state.dag[0][controls.index + 2].modifier = dag_cell;
  state.pe[0].astat = old_status[0] | (controls.predicates & 1);
  state.pe[1].astat = old_status[1] | (controls.predicates >> 1);
  return state;
}

void verify_system_memory_case(sharc::Core &core,
                               std::vector<std::byte> &ram,
                               const RegisterMemoryCase &item,
                               const RegisterMemoryControls &controls) {
  const auto before = system_memory_state(item, controls);
  const auto beginning = core.save();
  const bool simd = (controls.flags & 1) != 0;
  const auto active = controls.predicates & (simd ? 3U : 1U);
  const bool broadcast = (controls.flags & 2) != 0 && item.form != RegisterMemoryForm::absolute;
  auto expected = before;
  const auto seed = register_memory_seed(item);
  auto expected_ram = seed;
  if (controls.write) {
    expect_system_store(before, expected_ram, item, active);
  } else {
    const auto read = broadcast && !simd && active != 0 ? 3U : active;
    expect_system_load(expected, item, read, broadcast);
  }
  if (item.form == RegisterMemoryForm::post && active != 0) {
    expected.dag[0][controls.index].index += dag_cell;
  }
  const auto now = core.state().progress.base_cycles;
  for (const bool bounded : {false, true}) {
    core.restore(beginning);
    core.set_registers(before);
    std::ranges::copy(seed, ram.begin());
    const auto count = bounded ? 1U : 2U;
    const auto report = bounded ? core.run_until(now + count) : core.run(count);
    fixture::boundary(expected, {.pc = before.pc + count, .clock = count});
    check(fixture::completed(report, count, count) && core.registers() == expected &&
              ram == expected_ram,
          "system-register memory transfer lost a lane, register half, or memory footprint");
  }
}

void system_memory_access(const RegisterMemoryCase &item, sharc::ExecutionMode mode) {
  const auto index = item.space == sharc::MemorySpace::program ? 9U : 1U;
  const std::array words{register_memory_encoding(item, index, false),
                         std::uint64_t{0},
                         register_memory_encoding(item, index, true),
                         std::uint64_t{0}};
  fixture::Program program(words);
  auto &variant = program.code.variants.front();
  variant.entries.push_back(fixture::code_address + 2);
  if (item.view == sharc::MemoryView::packed_48) {
    variant.layout.regions.push_back({.space = item.space,
                                      .first = ram_address,
                                      .count = dag_capture_count,
                                      .storage_id = ram_id,
                                      .byte_stride = immediate_stride,
                                      .initialized = true});
  }
  variant.layout.regions.push_back({.space = item.space,
                                    .view = item.view,
                                    .first = ram_address,
                                    .count = dag_capture_count,
                                    .storage_id = ram_id,
                                    .byte_offset = offset,
                                    .byte_stride = immediate_stride,
                                    .initialized = true});
  const auto native = fixture::compile(program.code, mode);
  auto ram = register_memory_seed(item);
  sharc::Core core(program.memory(ram_id, ram), mode);
  (void)core.install(native, fixture::Program::variant_id);
  const bool conditional =
      item.form == RegisterMemoryForm::pre || item.form == RegisterMemoryForm::post;
  for (unsigned flags = 0; flags < 4; ++flags) {
    for (unsigned predicates = conditional ? 0U : 3U; predicates < 4; ++predicates) {
      for (const bool write : {false, true}) {
        verify_system_memory_case(
            core,
            ram,
            item,
            {.index = index, .flags = flags, .predicates = predicates, .write = write});
      }
    }
  }
}

void system_memory_access(sharc::ExecutionMode mode) {
  using enum RegisterMemoryForm;
  using enum sharc::MemorySpace;
  using enum sharc::MemoryView;
  constexpr std::array cases{
      RegisterMemoryCase{.form = absolute,
                         .space = data,
                         .view = long_word,
                         .reg = user_status_ports[0],
                         .width = 8},
      RegisterMemoryCase{.form = absolute,
                         .space = program,
                         .view = normal_word,
                         .reg = second_px_half,
                         .width = 4,
                         .force = true},
      RegisterMemoryCase{
          .form = absolute, .space = program, .view = long_word, .reg = combined_px, .width = 8},
      RegisterMemoryCase{
          .form = immediate, .space = data, .view = normal_word, .reg = first_px_half, .width = 4},
      RegisterMemoryCase{.form = immediate,
                         .space = program,
                         .view = long_word,
                         .reg = user_status_ports[2],
                         .width = 8},
      RegisterMemoryCase{
          .form = pre, .space = program, .view = short_word, .reg = second_px_half, .width = 2},
      RegisterMemoryCase{.form = post,
                         .space = data,
                         .view = normal_word,
                         .reg = user_status_ports[1],
                         .width = 4},
      RegisterMemoryCase{.form = post,
                         .space = data,
                         .view = normal_word,
                         .reg = user_status_ports[3],
                         .width = 4,
                         .force = true},
      RegisterMemoryCase{.form = post,
                         .space = program,
                         .view = normal_word,
                         .reg = combined_px,
                         .width = 4,
                         .force = true},
  };
  for (const auto &item : cases) { system_memory_access(item, mode); }
  for (const auto bus : {data, program}) {
    for (const auto form : {absolute, immediate, pre, post}) {
      system_memory_access(
          {.form = form, .space = bus, .view = short_word, .reg = combined_px, .width = 2}, mode);
      system_memory_access(
          {.form = form, .space = bus, .view = packed_48, .reg = combined_px, .width = 6}, mode);
    }
    system_memory_access({.form = post,
                          .space = bus,
                          .view = short_word,
                          .reg = combined_px,
                          .width = 2,
                          .force = true},
                         mode);
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
    predicated_packets(mode);
    predicated_short_lane(mode);
    dag_memory_access(mode);
    system_memory_access(mode);
    for (const auto space : {sharc::MemorySpace::data, sharc::MemorySpace::program}) {
      for (const bool indexed : {false, true}) {
        for (const bool odd : {false, true}) {
          forced_transfers({.space = space, .indexed = indexed, .odd = odd}, mode);
        }
      }
    }
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
