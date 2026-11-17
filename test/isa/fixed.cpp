// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "fixed_cases.hpp"
#include "sharc/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <sharc/core.hpp>
#include <string>
#include <tuple>
#include <utility>

namespace {
constexpr std::uint32_t variant_id = 7;
constexpr std::uint32_t storage_id = 9;
constexpr std::size_t instruction_bytes = 6;
constexpr unsigned extension_bits = 8;
constexpr unsigned opcode_shift = 12;
constexpr unsigned destination_shift = 8;
constexpr unsigned source_shift = 4;
constexpr std::uint64_t compute_header = 0x013e00000000;
constexpr std::uint64_t raw_seed = 0x81234567ef;
constexpr unsigned high_bank_bit = 7;
constexpr unsigned low_bank_bit = 10;
constexpr unsigned saturation_bit = 13;
constexpr unsigned truncation_bit = 15;
constexpr unsigned rounding_bit = 16;
constexpr unsigned simd_bit = 21;
constexpr std::uint64_t initial_clock = 123;

struct Program {
  sharc::CodeImage image;
  std::array<std::byte, instruction_bytes * 4> bytes{};
  unsigned destination, x, y;

  Program(unsigned opcode, bool high)
      : destination(high ? 9U : 1U), x(high ? 10U : 2U), y(high ? 11U : 3U) {
    const auto word = compute_header | (std::uint64_t{opcode} << opcode_shift) |
                      (std::uint64_t{destination} << destination_shift) |
                      (std::uint64_t{x} << source_shift) | y;
    image = fixture::image(sharc::InstructionSet::isa, word);
    for (unsigned i = 0; i < 3; ++i) { fixture::append(image.variants[0].spans[0], 0); }
    image.variants[0].layout.regions.push_back({.space = sharc::MemorySpace::program,
                                                .view = sharc::MemoryView::instruction,
                                                .first = fixture::code_address,
                                                .count = 4,
                                                .storage_id = storage_id,
                                                .byte_offset = 0,
                                                .byte_stride = instruction_bytes,
                                                .readable = true,
                                                .writable = false,
                                                .initialized = true});
    for (unsigned byte = 0; byte < instruction_bytes; ++byte) {
      bytes[byte] = static_cast<std::byte>(word >> (byte * extension_bits));
    }
  }

  sharc::Memory memory() {
    return {.regions = {{.storage_id = storage_id, .bytes = bytes, .generation = 1}}, .io = {}};
  }
};

void verify(const fixture::FixedCase &expected,
            Program &program,
            const sharc::NativeImage &native,
            sharc::ExecutionMode mode,
            unsigned banks,
            bool simd) {
  using fixture::check;
  sharc::Core core(program.memory(), mode);
  sharc::Registers registers;
  const unsigned low_bank = banks & 1U;
  const unsigned high_bank = (banks >> 1) & 1U;
  const auto selected_bank = program.destination < 8 ? low_bank : high_bank;
  registers.mode1 = (low_bank << low_bank_bit) | (high_bank << high_bank_bit) |
                    (unsigned{expected.saturate} << saturation_bit) |
                    (unsigned{expected.truncate} << truncation_bit) | (1U << rounding_bit) |
                    (unsigned{simd} << simd_bit);
  registers.pc = fixture::code_address;
  registers.emuclk = initial_clock;
  for (unsigned lane = 0; lane < 2; ++lane) {
    for (auto &bank : registers.pe[lane].data) { bank.fill(raw_seed); }
    registers.pe[lane].data[selected_bank][program.x] = expected.left[lane];
    registers.pe[lane].data[selected_bank][program.y] = expected.right[lane];
    registers.pe[lane].astat = expected.old_astat[lane];
    registers.pe[lane].stky = expected.old_stky[lane];
  }
  core.set_registers(registers);
  (void)core.install(native, variant_id);
  const auto snapshot = core.save();
  const auto partial = core.run(1);
  check(!partial.has_fault &&
            partial.progress == sharc::Progress{.instructions = 1, .base_cycles = 1},
        expected.name);
  auto result = registers;
  for (unsigned lane = 0; lane < (simd ? 2U : 1U); ++lane) {
    if (expected.writes) {
      result.pe[lane].data[selected_bank][program.destination] = expected.raw[lane];
    }
    result.pe[lane].astat = expected.astat[lane];
    result.pe[lane].stky = expected.stky[lane];
  }
  result.pc = fixture::code_address + 1;
  result.faddr = result.pc + 4;
  result.daddr = result.pc + 2;
  result.emuclk = initial_clock + 1;
  check(core.registers() == result, std::string(expected.name) + ": partial native result/state");
  core.restore(snapshot);
  const auto complete = core.run(4);
  check(!complete.has_fault &&
            complete.progress == sharc::Progress{.instructions = 4, .base_cycles = 4},
        expected.name);
  result.pc += 3;
  result.faddr += 3;
  result.daddr += 3;
  result.emuclk += 3;
  check(core.registers() == result, std::string(expected.name) + ": admitted native result/state");
}
} // namespace

int main() try {
  fixture::control_flow();
  fixture::memory_access();
  fixture::dag_addressing();
  using Key = std::tuple<unsigned, bool, sharc::ExecutionMode>;
  std::map<Key, sharc::NativeImage> cache;
  for (const auto &expected : fixture::fixed_cases) {
    for (const auto high : {false, true}) {
      Program program(expected.opcode, high);
      for (const auto mode : {sharc::ExecutionMode::strict, sharc::ExecutionMode::fast_fp32}) {
        const Key key{expected.opcode, high, mode};
        auto found = cache.find(key);
        if (found == cache.end()) {
          found = cache
                      .emplace(key,
                               sharc::Core::compile(program.image, {.mode = mode, .callbacks = {}}))
                      .first;
        }
        for (unsigned banks = 0; banks < 4; ++banks) {
          for (const auto simd : {false, true}) {
            verify(expected, program, found->second, mode, banks, simd);
          }
        }
      }
    }
  }
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
