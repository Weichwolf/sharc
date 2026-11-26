// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "fixed_cases.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <sharc/core.hpp>
#include <string>
#include <tuple>
#include <utility>

namespace {
constexpr std::uint32_t variant_id = 7;
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
  unsigned destination, x, y;
  fixture::Program code;

  Program(unsigned opcode, bool high)
      : destination(high ? 9U : 1U),
        x(high ? 10U : 2U),
        y(high ? 11U : 3U),
        code(std::array{compute_header | (std::uint64_t{opcode} << opcode_shift) |
                            (std::uint64_t{destination} << destination_shift) |
                            (std::uint64_t{x} << source_shift) | y,
                        std::uint64_t{0},
                        std::uint64_t{0},
                        std::uint64_t{0}}) {}
};

void verify(const fixture::FixedCase &expected,
            Program &program,
            const sharc::NativeImage &native,
            sharc::ExecutionMode mode,
            unsigned banks,
            bool simd) {
  using fixture::check;
  sharc::Core core(program.code.memory(), mode);
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
  check(fixture::completed(partial, 1, 1), expected.name);
  auto result = registers;
  for (unsigned lane = 0; lane < (simd ? 2U : 1U); ++lane) {
    if (expected.writes) {
      result.pe[lane].data[selected_bank][program.destination] = expected.raw[lane];
    }
    result.pe[lane].astat = expected.astat[lane];
    result.pe[lane].stky = expected.stky[lane];
  }
  fixture::boundary(result, {.pc = fixture::code_address + 1, .clock = initial_clock + 1});
  check(core.registers() == result, std::string(expected.name) + ": partial native result/state");
  core.restore(snapshot);
  const auto complete = core.run(4);
  check(fixture::completed(complete, 4, 4), expected.name);
  fixture::boundary(result, {.pc = result.pc + 3, .clock = result.emuclk + 3});
  check(core.registers() == result, std::string(expected.name) + ": admitted native result/state");
}
} // namespace

int main() {
  return fixture::run_test([] {
    fixture::rotations();
    fixture::register_transfers();
    fixture::control_flow();
    fixture::computed_control_flow();
    fixture::delayed_control_flow();
    fixture::hardware_loop_control_flow();
    fixture::memory_access();
    fixture::dag_addressing();
    using Key = std::tuple<unsigned, bool, sharc::ExecutionMode>;
    std::map<Key, sharc::NativeImage> cache;
    for (const auto &expected : fixture::fixed_cases) {
      for (const auto high : {false, true}) {
        Program program(expected.opcode, high);
        for (const auto mode : fixture::numeric_modes) {
          const Key key{expected.opcode, high, mode};
          auto found = cache.find(key);
          if (found == cache.end()) {
            found = cache.emplace(key, fixture::compile(program.code.code, mode)).first;
          }
          for (unsigned banks = 0; banks < 4; ++banks) {
            for (const auto simd : {false, true}) {
              verify(expected, program, found->second, mode, banks, simd);
            }
          }
        }
      }
    }
  });
}
