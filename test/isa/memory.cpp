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
} // namespace

namespace fixture {
void memory_access() {
  for (unsigned index = 0; index < 16; ++index) {
    for (const auto mode : fixture::numeric_modes) { memory_index(index, mode); }
  }
}
} // namespace fixture
