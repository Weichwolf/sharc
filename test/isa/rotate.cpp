// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <array>
#include <cstdint>
#include <sharc/core.hpp>
#include <string_view>
#include <vector>

namespace {
using fixture::check;
constexpr std::uint64_t compute_prefix = 0x010000208000;
constexpr unsigned destination_shift = 8, source_shift = 4;
constexpr unsigned low_bank_bit = 10, high_bank_bit = 7, simd_bit = 21;
constexpr unsigned saturation_bit = 13, rounding_bit = 16;
constexpr std::uint32_t shift_flags = 0x3800, zero_flag = 0x1000;
constexpr std::uint32_t word_mask = 0xffffffff;
constexpr unsigned rotation_mask = 31;
constexpr std::uint64_t source_extension = 0x5a, count_extension = 0xa5;
constexpr std::uint64_t old_raw = 0x81234567ef;
constexpr std::array counts{0U,          1U,          31U,         32U,         33U,
                            63U,         64U,         127U,        128U,        255U,
                            256U,        0xfffffeffU, 0xffffff7fU, 0xffffff80U, 0xffffffbfU,
                            0xffffffc0U, 0xffffffdfU, 0xffffffe0U, 0xffffffe1U, 0xffffffffU,
                            0x80000000U, 0x80000001U, 0x7fffffffU};

// A bit permutation supplies expectations independently of LLVM funnel shifts.
struct Rotation {
  std::uint32_t word{}, count{};
};

std::uint32_t rotate(Rotation input) {
  const auto [word, count] = input;
  constexpr unsigned bits = 32;
  std::uint32_t result{};
  for (unsigned source = 0; source < bits; ++source) {
    if ((word & (1U << source)) != 0) {
      result |= 1U << ((source + (count & rotation_mask)) % bits);
    }
  }
  return result;
}

struct Registers {
  unsigned destination{}, source{}, count{};
};

unsigned bank(const sharc::Registers &state, unsigned reg) {
  return (state.mode1 >> (reg < 8 ? low_bank_bit : high_bank_bit)) & 1;
}

sharc::Registers initial(unsigned configuration) {
  constexpr std::array old_flags{0xa5a53fffU, 0x5a5affffU};
  constexpr std::array old_sticky{0xdeadbeefU, 0x13579bdfU};
  sharc::Registers before;
  before.pc = fixture::code_address;
  before.mode1 =
      ((configuration & 1) << low_bank_bit) | (((configuration >> 1) & 1) << high_bank_bit) |
      (((configuration >> 2) & 1) << simd_bit) | (((configuration >> 5) & 1) << saturation_bit) |
      (((configuration >> 6) & 1) << rounding_bit);
  for (unsigned lane = 0; lane < 2; ++lane) {
    for (auto &physical : before.pe[lane].data) { physical.fill(old_raw + lane); }
    before.pe[lane].astat = (old_flags[lane] & ~1U) | ((configuration >> (3 + lane)) & 1);
    before.pe[lane].stky = old_sticky[lane];
  }
  return before;
}

std::vector<std::uint32_t> bit_patterns() {
  std::vector<std::uint32_t> result{0, word_mask};
  for (unsigned bit = 0; bit < 32; ++bit) { result.push_back(1U << bit); }
  return result;
}

struct PrefixDiagnostics {
  std::string_view instruction, deadline;
};

void verify_prefix(sharc::Core &core,
                   const sharc::Registers &expected,
                   PrefixDiagnostics messages) {
  const auto boundary = core.save();
  check(fixture::completed(core.run(1), 1, 1) && core.registers() == expected,
        messages.instruction);
  core.restore(boundary);
  check(!core.run_until(core.state().progress.base_cycles + 1).has_fault &&
            core.registers() == expected,
        messages.deadline);
}

void apply_rotation(sharc::Registers &expected,
                    Registers registers,
                    const std::array<std::uint32_t, 2> &counts) {
  const auto lanes = ((expected.mode1 >> simd_bit) & 1) != 0 ? 2U : 1U;
  for (unsigned lane = 0; lane < lanes; ++lane) {
    auto &pe = expected.pe[lane];
    if ((pe.astat & 1) == 0) { continue; }
    const auto source = static_cast<std::uint32_t>(
        pe.data[bank(expected, registers.source)][registers.source] >> 8);
    const auto result = rotate({.word = source, .count = counts[lane]});
    pe.data[bank(expected, registers.destination)][registers.destination] = std::uint64_t{result}
                                                                            << 8;
    pe.astat = (pe.astat & ~shift_flags) | (result == 0 ? zero_flag : 0);
  }
}

void exercise_configurations(fixture::Program &program,
                             Registers registers,
                             void (*verify)(sharc::Core &, Registers, unsigned)) {
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    for (unsigned configuration = 0; configuration < 128; ++configuration) {
      verify(core, registers, configuration);
    }
  }
}

void verify(sharc::Core &core, Registers registers, unsigned configuration) {
  auto before = initial(configuration);
  for (const auto word : bit_patterns()) {
    for (const auto count : counts) {
      const std::array values{word, word ^ word_mask};
      const std::array amounts{count, ~count};
      for (unsigned lane = 0; lane < 2; ++lane) {
        before.pe[lane].data[bank(before, registers.source)][registers.source] =
            (std::uint64_t{values[lane]} << 8) | source_extension;
        before.pe[lane].data[bank(before, registers.count)][registers.count] =
            (std::uint64_t{amounts[lane]} << 8) | count_extension;
      }
      auto expected = before;
      apply_rotation(expected, registers, amounts);
      fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 1});
      core.set_registers(before);
      verify_prefix(
          core,
          expected,
          {.instruction =
               "ROT changed raw extensions, flags, signed count, physical bank or predicate lane",
           .deadline = "ROT differs under an exact cycle deadline"});
    }
  }
}

void branch_interlock(bool immediate) {
  constexpr std::uint64_t rotate_r2_r0_r6 = 0x013e00208206;
  constexpr std::uint64_t rotate_r2_r0_minus_one = 0x023e0002ff20;
  constexpr std::uint64_t jump_sz_next = 0x071000000001;
  const std::array words{
      immediate ? rotate_r2_r0_minus_one : rotate_r2_r0_r6, jump_sz_next, std::uint64_t{0}};
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    for (const bool simd : {false, true}) {
      sharc::Core core(program.memory(), mode);
      sharc::Core resumed(program.memory(), mode);
      sharc::Registers expected;
      expected.pc = fixture::code_address;
      expected.mode1 = unsigned{simd} << simd_bit;
      expected.pe[0].data[0][6] = std::uint64_t{word_mask} << 8;
      fixture::install(core, native, expected);
      (void)resumed.install(native, fixture::Program::variant_id);
      check(!core.run(1).has_fault, "ROT prefix before a shifter-condition branch failed");
      resumed.restore(core.save());
      const auto result = resumed.run_until(resumed.state().progress.base_cycles + 1);
      expected.pe[0].astat = zero_flag;
      if (simd) { expected.pe[1].astat = zero_flag; }
      fixture::boundary(expected, {.pc = expected.pc + 2, .clock = 6});
      check(fixture::completed(result, 1, 5) && resumed.registers() == expected,
            "ROT shifter flags or branch interlock did not survive a cross-Core snapshot");
    }
  }
}

void immediate_verify(sharc::Core &core, Registers registers, unsigned configuration) {
  constexpr unsigned count_values = 256;
  for (const auto input : bit_patterns()) {
    auto expected = initial(configuration);
    const std::array sources{input, input ^ word_mask};
    for (unsigned lane = 0; lane < 2; ++lane) {
      expected.pe[lane].data[bank(expected, registers.source)][registers.source] =
          (std::uint64_t{sources[lane]} << 8) | source_extension;
    }
    core.set_registers(expected);
    const auto start = core.save();
    for (unsigned count = 0; count < count_values; ++count) {
      apply_rotation(expected, registers, {count, count});
      fixture::boundary(expected, {.pc = expected.pc + 1, .clock = expected.emuclk + 1});
      verify_prefix(core,
                    expected,
                    {.instruction = "immediate ROT lost its count, source alias, predicate, bank, "
                                    "flags or raw extension",
                     .deadline = "immediate ROT differs at an instruction or cycle prefix"});
    }
    core.restore(start);
    const auto granted = core.run(count_values);
    check(fixture::completed(granted, count_values, count_values) && core.registers() == expected,
          "immediate ROT whole-unit execution differs from exact prefixes");
  }
}

void immediate_rotations() {
  constexpr unsigned count_values = 256;
  constexpr std::uint64_t conditional_eq = 0x020000020000;
  constexpr std::array refused{
      0x803e0002ff20ULL, 0x023e0802ff20ULL, 0x023e0082ff20ULL, 0x023e0001ff20ULL};
  for (const auto word : refused) {
    fixture::throws<sharc::PreparationError>(
        [&] { (void)sharc::Core::lower(fixture::image(sharc::InstructionSet::isa, word)); },
        "unqualified parallel, extension, unused field or immediate operation was admitted");
  }
  constexpr std::array cases{Registers{.destination = 2, .source = 2, .count = 0},
                             Registers{.destination = 11, .source = 3, .count = 0}};
  for (const auto &registers : cases) {
    std::array<std::uint64_t, count_values> words{};
    for (unsigned count = 0; count < count_values; ++count) {
      words[count] = conditional_eq | (std::uint64_t{count} << 8) |
                     (std::uint64_t{registers.destination} << 4) | registers.source;
    }
    fixture::Program program(words);
    exercise_configurations(program, registers, immediate_verify);
  }
}

} // namespace

namespace fixture {
void rotations() {
  branch_interlock(false);
  branch_interlock(true);
  immediate_rotations();
  constexpr std::array cases{Registers{.destination = 2, .source = 2, .count = 3},
                             Registers{.destination = 11, .source = 10, .count = 11}};
  for (const auto &registers : cases) {
    const std::array words{compute_prefix |
                           (std::uint64_t{registers.destination} << destination_shift) |
                           (std::uint64_t{registers.source} << source_shift) | registers.count};
    Program program(words);
    exercise_configurations(program, registers, verify);
  }
}
} // namespace fixture
