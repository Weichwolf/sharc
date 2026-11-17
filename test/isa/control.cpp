// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "fixed_cases.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <sharc/core.hpp>
#include <tuple>

namespace {
using fixture::check;
constexpr unsigned condition_shift = 33, opcode_shift = 40;
constexpr unsigned saturation_bit = 13, simd_bit = 21;
constexpr std::uint32_t true_condition = 31, ne_condition = 16;
constexpr std::uint32_t address_mask = 0xffffff;
constexpr std::uint64_t raw_seed = 0x81234567ef;
constexpr std::uint64_t initial_clock = 123;
constexpr std::uint32_t final_marker = 17;
constexpr std::uint64_t loop_cycles = 23;

std::uint64_t jump(unsigned condition, std::uint32_t target, bool relative) {
  return (std::uint64_t{relative ? 7U : 6U} << opcode_shift) |
         (std::uint64_t{condition} << condition_shift) | target;
}

sharc::Registers initial_registers() {
  sharc::Registers result;
  result.pc = fixture::code_address;
  result.emuclk = initial_clock;
  for (auto &lane : result.pe) {
    for (auto &bank : lane.data) { bank.fill(raw_seed); }
  }
  return result;
}

void verify_predicate(const fixture::BranchCase &expected,
                      fixture::Program &program,
                      const sharc::NativeImage &native,
                      sharc::ExecutionMode mode) {
  sharc::Core core(program.memory(), mode);
  auto registers = initial_registers();
  registers.mode1 =
      (unsigned{expected.saturate} << saturation_bit) | (unsigned{expected.simd} << simd_bit);
  for (unsigned lane = 0; lane < 2; ++lane) { registers.pe[lane].astat = expected.astat[lane]; }
  core.set_registers(registers);
  (void)core.install(native, fixture::Program::variant_id);
  const auto snapshot = core.save();
  const sharc::Progress progress{.instructions = 1, .base_cycles = expected.taken ? 4U : 1U};
  registers.pc += expected.taken ? 4U : 1U;
  registers.faddr = registers.pc + 4;
  registers.daddr = registers.pc + 2;
  registers.emuclk += progress.base_cycles;
  const auto fast = core.run(1);
  check(!fast.has_fault && fast.progress == progress && core.registers() == registers,
        "direct branch predicate, lane AND, target or state differs");
  core.restore(snapshot);
  const auto partial = core.run_until(1);
  check(!partial.has_fault && partial.progress == progress && core.registers() == registers,
        "branch deadline did not stop at the first complete boundary");
}

void predicates() {
  using Key = std::tuple<unsigned, bool, sharc::ExecutionMode>;
  std::map<Key, sharc::NativeImage> images;
  for (const auto &expected : fixture::branch_cases) {
    for (const auto relative : {false, true}) {
      const std::array words{
          jump(expected.condition, relative ? 4U : fixture::code_address + 4, relative),
          std::uint64_t{0},
          std::uint64_t{0},
          std::uint64_t{0},
          std::uint64_t{0}};
      fixture::Program program(words);
      for (const auto mode : {sharc::ExecutionMode::strict, sharc::ExecutionMode::fast_fp32}) {
        const Key key{expected.condition, relative, mode};
        auto found = images.find(key);
        if (found == images.end()) {
          found =
              images
                  .emplace(key, sharc::Core::compile(program.code, {.mode = mode, .callbacks = {}}))
                  .first;
        }
        verify_predicate(expected, program, found->second, mode);
      }
    }
  }
}

void counted_control_flow() {
  // R0 accumulates three increments; R1 counts down; NE returns to the ADD.
  constexpr std::uint64_t decrement_r1 = 0x013e0002a110;
  const std::array words{fixture::immediate(0, 0),
                         fixture::immediate(1, 3),
                         fixture::immediate(2, 1),
                         fixture::add(0, 0, 2),
                         decrement_r1,
                         jump(ne_condition, address_mask - 1, true),
                         fixture::immediate(3, final_marker),
                         std::uint64_t{0}};
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  sharc::Registers registers;
  registers.pc = fixture::code_address;
  core.set_registers(registers);
  (void)core.install(native, fixture::Program::variant_id);
  const auto complete = core.run(14);
  registers.pe[0].data[0][0] = 3U << 8;
  registers.pe[0].data[0][2] = 1U << 8;
  registers.pe[0].data[0][3] = final_marker << 8;
  registers.pe[0].astat = 9;
  registers.pc += static_cast<std::uint32_t>(words.size());
  registers.faddr = registers.pc + 4;
  registers.daddr = registers.pc + 2;
  registers.emuclk = loop_cycles;
  check(!complete.has_fault &&
            complete.progress == sharc::Progress{.instructions = 14, .base_cycles = loop_cycles} &&
            core.registers() == registers,
        "native backward branch lost loop-carried state or cycles");
}

void wrapped_target() {
  const std::array wrap_words{jump(true_condition, 1, true)};
  fixture::Program wrap(wrap_words, address_mask);
  const auto wrap_native = sharc::Core::compile(wrap.code);
  sharc::Core wrapped(wrap.memory());
  auto registers = initial_registers();
  registers.pc = address_mask;
  wrapped.set_registers(registers);
  (void)wrapped.install(wrap_native, fixture::Program::variant_id);
  check(!wrapped.run(1).has_fault && wrapped.registers().pc == 0,
        "relative branch did not wrap its 24-bit target");
}

void long_native_loop() {
  constexpr std::uint64_t iterations = 1'000'000;
  constexpr std::uint64_t clock_before_wrap = 0xfffffff0;
  const std::array words{jump(true_condition, 0, true)};
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  auto registers = initial_registers();
  registers.emuclk = clock_before_wrap;
  core.set_registers(registers);
  (void)core.install(native, fixture::Program::variant_id);
  const auto result = core.run(iterations);
  registers.faddr = registers.pc + 4;
  registers.daddr = registers.pc + 2;
  registers.emuclk += iterations * 4;
  check(!result.has_fault &&
            result.progress ==
                sharc::Progress{.instructions = iterations, .base_cycles = iterations * 4} &&
            core.registers() == registers,
        "native control loop lost progress, clock carry or bounded host stack");
}

void rejected_branch_forms() {
  constexpr std::array reserved_bits{25U, 27U, 28U, 29U, 30U, 31U, 32U};
  for (const auto bit : reserved_bits) {
    const auto word =
        jump(true_condition, fixture::code_address, false) | (std::uint64_t{1} << bit);
    const auto image = fixture::image(sharc::InstructionSet::isa, word);
    try {
      (void)sharc::Core::lower(image);
      check(false, "reserved direct-branch bit was accepted");
    } catch (const sharc::PreparationError &error) {
      check(error.fault().reason == sharc::FaultReason::invalid_encoding &&
                error.fault().instruction == word && error.fault().address == fixture::code_address,
            "reserved branch lost fault identity or location");
    }
  }
  constexpr std::array unsupported_bits{24U, 26U, 38U, 39U};
  for (const auto bit : unsupported_bits) {
    const auto word =
        jump(true_condition, fixture::code_address, false) | (std::uint64_t{1} << bit);
    fixture::throws<sharc::PreparationError>(
        [&] { (void)sharc::Core::lower(fixture::image(sharc::InstructionSet::isa, word)); },
        "unimplemented direct-branch modifier was accepted");
  }
}
} // namespace

namespace fixture {
void control_flow() {
  predicates();
  counted_control_flow();
  wrapped_target();
  long_native_loop();
  rejected_branch_forms();
}
} // namespace fixture
