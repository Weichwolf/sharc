// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <array>
#include <cstdint>
#include <sharc/core.hpp>
#include <vector>

namespace {
using fixture::check;
constexpr unsigned circular_bit = 24, simd_bit = 21;
constexpr unsigned sticky_bit = 17, interrupt_bit = 20;
constexpr std::array<unsigned, 16> bank_bits{4, 4, 4, 4, 3, 3, 3, 3, 6, 6, 6, 6, 5, 5, 5, 5};
constexpr std::uint64_t raw_seed = 0x81234567ef;
constexpr std::uint32_t primary_base = 0x2000, alternate_base = 0x6000;
constexpr std::uint32_t old_latch = 0x15, old_status = 0x13579bdf, old_sticky = 0x80000400;
constexpr std::uint32_t buffer_length = 17, old_modifier = 0xabcdef00, near_wrap = 0xfffffff8;
constexpr std::array<std::int32_t, 8> steps{1, -1, 16, -16, 7, -7, 0, 1};

struct Modify {
  unsigned source{}, destination{};
  std::int32_t displacement{};
  bool reverse{};
};

std::uint64_t encoding(const Modify &op) {
  constexpr unsigned reverse_shift = 39;
  constexpr unsigned dag_shift = 38;
  constexpr unsigned destination_shift = 35;
  constexpr std::uint64_t opcode = 0x160000000000;
  return opcode | (std::uint64_t{op.reverse} << reverse_shift) |
         (std::uint64_t{op.source / 8} << dag_shift) |
         (std::uint64_t{(op.source ^ op.destination) & 7} << destination_shift) |
         (std::uint64_t{op.source & 7} << 32) | static_cast<std::uint32_t>(op.displacement);
}

std::uint32_t reverse_bits(std::uint32_t value) {
  std::uint32_t result{};
  for (unsigned bit = 0; bit < 32; ++bit) { result = (result << 1) | ((value >> bit) & 1); }
  return result;
}

void apply(sharc::Registers &registers, const Modify &op) {
  const auto source_bank = (registers.mode1 >> bank_bits[op.source]) & 1;
  const auto destination_bank = (registers.mode1 >> bank_bits[op.destination]) & 1;
  const auto source = registers.dag[source_bank][op.source];
  auto result = std::int64_t{source.index} + op.displacement;
  bool overflow{};
  if (source.length != 0) {
    if (result < source.base) {
      result += source.length;
      overflow = true;
    } else if (result >= std::int64_t{source.base} + source.length) {
      result -= source.length;
      overflow = true;
    }
  } else if ((registers.mode1 & (1U << circular_bit)) != 0) {
    const auto word = static_cast<std::uint32_t>(result);
    overflow = op.displacement < 0 ? word < source.base : word >= source.base;
  }
  auto word = static_cast<std::uint32_t>(result);
  if (op.reverse) { word = reverse_bits(word); }
  registers.dag[destination_bank][op.destination].index = word;
  if ((op.source & 7) == 7 && overflow) {
    registers.pe[0].stky |= 1U << (sticky_bit + (op.source / 8));
    registers.irptl |= 1U << (interrupt_bit + (op.source / 8));
  }
  ++registers.pc;
  registers.faddr = registers.pc + 4;
  registers.daddr = registers.pc + 2;
  ++registers.emuclk;
}

struct Input {
  unsigned source{}, flags{}, configuration{};
};

sharc::Registers initial(const Input &input) {
  const auto [source, flags, configuration] = input;
  sharc::Registers registers;
  registers.pc = fixture::code_address;
  registers.irptl = old_latch;
  for (unsigned quarter = 0; quarter < 4; ++quarter) {
    constexpr std::array quarter_bits{4U, 3U, 6U, 5U};
    registers.mode1 |= ((flags >> quarter) & 1) << quarter_bits[quarter];
  }
  registers.mode1 |= ((flags >> 4) & 1) << simd_bit;
  registers.mode1 |= ((flags >> 5) & 1) << circular_bit;
  for (unsigned lane = 0; lane < 2; ++lane) {
    for (unsigned bank = 0; bank < 2; ++bank) {
      registers.pe[lane].data[bank].fill(raw_seed + (std::uint64_t{lane} * 16) + bank);
    }
    registers.pe[lane].astat = old_status;
    registers.pe[lane].stky = old_sticky;
  }
  for (unsigned bank = 0; bank < 2; ++bank) {
    for (unsigned index = 0; index < 16; ++index) {
      auto &dag = registers.dag[bank][index];
      dag.base = (bank == 0 ? primary_base : alternate_base) + (index * 32);
      dag.index = dag.base + (configuration == 2 ? 16U : 0U);
      dag.length = configuration == 0 ? 0U : buffer_length;
      dag.modifier = old_modifier + index;
    }
    auto &dag = registers.dag[bank][source];
    if ((source & 7) == 7) {
      // Enhanced I7/I15 overflow routing is not inferred by these cases.
      dag.length = 4096;
      dag.index = dag.base + 2048;
    } else if (configuration == 0) {
      dag.index = bank == 0 ? near_wrap : 8;
    }
  }
  return registers;
}

void verify(fixture::Program &program,
            const sharc::NativeImage &native,
            sharc::ExecutionMode mode,
            sharc::Registers expected,
            const std::vector<Modify> &operations) {
  sharc::Core core(program.memory(), mode);
  core.set_registers(expected);
  (void)core.install(native, fixture::Program::variant_id);
  const auto saved = core.save();
  std::uint64_t count{};
  for (const auto &op : operations) {
    apply(expected, op);
    ++count;
    for (const bool cycles : {false, true}) {
      core.restore(saved);
      const auto result = cycles ? core.run_until(count) : core.run(count);
      check(!result.has_fault &&
                result.progress == sharc::Progress{.instructions = count, .base_cycles = count} &&
                core.registers() == expected,
            "DAG modify/reverse source, destination, banks, flags or exact prefix differs");
    }
  }
}

void index_matrix(sharc::ExecutionMode mode) {
  for (unsigned source = 0; source < 16; ++source) {
    for (const bool reverse : {false, true}) {
      std::vector<Modify> operations;
      std::vector<std::uint64_t> words;
      for (unsigned i = 0; i < 8; ++i) {
        const Modify op{.source = source,
                        .destination = (source & 8) | ((source + i + 1) & 7),
                        .displacement = steps[i],
                        .reverse = reverse};
        operations.push_back(op);
        words.push_back(encoding(op));
      }
      fixture::Program program(words);
      const auto native = sharc::Core::compile(program.code, {.mode = mode, .callbacks = {}});
      for (unsigned flags = 0; flags < 64; ++flags) {
        for (unsigned configuration = 0; configuration < 3; ++configuration) {
          verify(program,
                 native,
                 mode,
                 initial({.source = source, .flags = flags, .configuration = configuration}),
                 operations);
        }
      }
    }
  }
}

void verify_special(fixture::Program &program,
                    const sharc::NativeImage &native,
                    sharc::ExecutionMode mode,
                    const Modify &op) {
  for (unsigned flags = 0; flags < 64; ++flags) {
    for (const unsigned length : {0U, buffer_length}) {
      auto expected = initial({.source = op.source, .flags = flags, .configuration = 1});
      for (auto &bank : expected.dag) {
        bank[op.source].length = length;
        bank[op.source].index =
            bank[op.source].base + (op.displacement < 0 || length == 0 ? 0U : length - 1);
      }
      verify(program, native, mode, expected, {op});
    }
  }
}

void special_buffers(sharc::ExecutionMode mode) {
  for (const unsigned index : {7U, 15U}) {
    for (const bool reverse : {false, true}) {
      for (const auto displacement : {-1, 1}) {
        const Modify op{.source = index,
                        .destination = index,
                        .displacement = displacement,
                        .reverse = reverse};
        const std::array words{encoding(op)};
        fixture::Program program(words);
        const auto native = sharc::Core::compile(program.code, {.mode = mode, .callbacks = {}});
        verify_special(program, native, mode, op);
      }
    }
  }
}
} // namespace

void fixture::dag_addressing() {
  for (const auto mode : {sharc::ExecutionMode::strict, sharc::ExecutionMode::fast_fp32}) {
    index_matrix(mode);
    special_buffers(mode);
  }
}
