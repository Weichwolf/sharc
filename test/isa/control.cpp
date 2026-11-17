// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "fixed_cases.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
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
  constexpr std::array unsupported_bits{24U, 26U, 38U};
  for (const auto bit : unsupported_bits) {
    const auto word =
        jump(true_condition, fixture::code_address, false) | (std::uint64_t{1} << bit);
    fixture::throws<sharc::PreparationError>(
        [&] { (void)sharc::Core::lower(fixture::image(sharc::InstructionSet::isa, word)); },
        "unimplemented direct-branch modifier was accepted");
  }
}

struct CallBoundary {
  unsigned pc, cycles, depth;
};

constexpr std::array call_boundaries{CallBoundary{.pc = 1, .cycles = 1, .depth = 0},
                                     CallBoundary{.pc = 6, .cycles = 5, .depth = 1},
                                     CallBoundary{.pc = 9, .cycles = 9, .depth = 2},
                                     CallBoundary{.pc = 7, .cycles = 13, .depth = 1},
                                     CallBoundary{.pc = 8, .cycles = 14, .depth = 1},
                                     CallBoundary{.pc = 2, .cycles = 18, .depth = 0},
                                     CallBoundary{.pc = 3, .cycles = 19, .depth = 0},
                                     CallBoundary{.pc = 10, .cycles = 23, .depth = 0},
                                     CallBoundary{.pc = 11, .cycles = 24, .depth = 0}};

sharc::Registers call_state(sharc::Registers initial, std::size_t completed) {
  constexpr std::uint32_t call_tag = 1U << 24;
  constexpr std::uint32_t stack_empty = 1U << 22;
  const auto depth = initial.pc_stack_pointer;
  const bool simd = (initial.mode1 & (1U << simd_bit)) != 0;
  for (std::size_t step = 0; step < completed; ++step) {
    if (step == 0) {
      initial.pe[0].data[0][0] = 5U << 8;
      if (simd) { initial.pe[1].data[0][0] = 5U << 8; }
    }
    if (step == 1) { initial.pc_stack[depth] = (fixture::code_address + 2) | call_tag; }
    if (step == 2) { initial.pc_stack[depth + 1U] = (fixture::code_address + 7) | call_tag; }
    if (step == 4) {
      initial.pe[0].data[0][2] = 7U << 8;
      initial.pe[0].astat = 0;
      if (simd) {
        initial.pe[1].data[0][2] = 9U << 8;
        initial.pe[1].astat = 0;
      }
    }
    if (step == 6) {
      initial.pe[0].data[0][3] = 9U << 8;
      if (simd) { initial.pe[1].data[0][3] = 9U << 8; }
    }
  }
  const auto &boundary = call_boundaries[completed - 1];
  initial.pc_stack_pointer = static_cast<std::uint8_t>(depth + boundary.depth);
  if (completed >= 2) {
    initial.pe[0].stky &= ~stack_empty;
    if (initial.pc_stack_pointer == 0) { initial.pe[0].stky |= stack_empty; }
  }
  initial.pc = fixture::code_address + boundary.pc;
  initial.faddr = initial.pc + 4;
  initial.daddr = initial.pc + 2;
  initial.emuclk += boundary.cycles;
  return initial;
}

void static_calls() {
  constexpr std::uint64_t call = 1ULL << 39;
  constexpr std::uint64_t rts = 0x0a3e00000000;
  constexpr std::uint32_t stack_empty = 1U << 22;
  constexpr std::uint32_t initial_sticky_x = 0xa5000240;
  constexpr std::uint32_t initial_sticky_y = 0x95012345;
  const auto base = fixture::code_address;
  const std::array words{fixture::immediate(0, 5),
                         jump(true_condition, base + 6, false) | call,
                         fixture::immediate(3, 9),
                         jump(true_condition, base + 10, false),
                         std::uint64_t{0},
                         std::uint64_t{0},
                         jump(true_condition, base + 9, false) | call,
                         fixture::add(2, 0, 1),
                         rts,
                         rts,
                         std::uint64_t{0}};
  fixture::Program program(words);
  for (const auto mode : {sharc::ExecutionMode::strict, sharc::ExecutionMode::fast_fp32}) {
    const auto native = sharc::Core::compile(program.code, {.mode = mode, .callbacks = {}});
    for (const bool simd : {false, true}) {
      for (const unsigned depth : {0U, 2U}) {
        sharc::Core core(program.memory(), mode);
        auto initial = initial_registers();
        initial.mode1 = unsigned{simd} << simd_bit;
        initial.pc_stack_pointer = static_cast<std::uint8_t>(depth);
        initial.pe[0].data[0][1] = 2U << 8;
        initial.pe[1].data[0][1] = 4U << 8;
        initial.pe[0].stky = initial_sticky_x | (depth == 0 ? stack_empty : 0);
        initial.pe[1].stky = initial_sticky_y;
        for (std::size_t i = 0; i < initial.pc_stack.size(); ++i) {
          initial.pc_stack[i] = static_cast<std::uint32_t>(i);
        }
        core.set_registers(initial);
        (void)core.install(native, fixture::Program::variant_id);
        const auto saved = core.save();
        for (std::size_t count = 1; count <= call_boundaries.size(); ++count) {
          core.restore(saved);
          const auto result = core.run(count);
          check(!result.has_fault &&
                    result.progress ==
                        sharc::Progress{.instructions = count,
                                        .base_cycles = call_boundaries[count - 1].cycles} &&
                    core.registers() == call_state(initial, count),
                "nested static CALL/RTS differs at an instruction boundary");
        }
        for (unsigned deadline = 1; deadline <= call_boundaries.back().cycles; ++deadline) {
          core.restore(saved);
          const auto *found = std::ranges::find_if(
              call_boundaries, [deadline](const auto &entry) { return entry.cycles >= deadline; });
          const auto count = static_cast<std::size_t>(found - call_boundaries.begin()) + 1;
          const auto result = core.run_until(deadline);
          check(!result.has_fault && core.registers() == call_state(initial, count),
                "nested static CALL/RTS crossed the wrong cycle boundary");
        }
      }
    }
  }
}

void verify_stack_predicate(const fixture::BranchCase &predicate,
                            fixture::Program &program,
                            const sharc::NativeImage &native,
                            sharc::ExecutionMode mode,
                            bool returning) {
  constexpr std::uint32_t call_tag = 1U << 24;
  constexpr std::uint32_t stack_empty = 1U << 22;
  constexpr std::uint32_t old_sticky = 0xa5000240;
  sharc::Core core(program.memory(), mode);
  auto expected = initial_registers();
  expected.mode1 = unsigned{predicate.simd} << simd_bit;
  expected.pe[0].astat = predicate.astat[0];
  expected.pe[1].astat = predicate.astat[1];
  expected.pe[0].stky = old_sticky | (returning ? 0 : stack_empty);
  expected.pe[1].stky = old_sticky;
  expected.pc_stack_pointer = returning ? 1 : 0;
  if (returning) { expected.pc_stack[0] = (fixture::code_address + 2) | call_tag; }
  core.set_registers(expected);
  (void)core.install(native, fixture::Program::variant_id);
  const auto saved = core.save();
  expected.pc += predicate.taken ? 2U : 1U;
  expected.faddr = expected.pc + 4;
  expected.daddr = expected.pc + 2;
  const sharc::Progress progress{.instructions = 1, .base_cycles = predicate.taken ? 4U : 1U};
  expected.emuclk += progress.base_cycles;
  if (predicate.taken) {
    if (returning) {
      expected.pc_stack_pointer = 0;
      expected.pe[0].stky |= stack_empty;
    } else {
      expected.pc_stack_pointer = 1;
      expected.pc_stack[0] = (fixture::code_address + 1) | call_tag;
      expected.pe[0].stky &= ~stack_empty;
    }
  }
  const auto result = core.run(1);
  check(!result.has_fault && result.progress == progress && core.registers() == expected,
        "CALL/RTS predicate changed the wrong lane, stack, flags or target");
  core.restore(saved);
  const auto bounded = core.run_until(1);
  check(!bounded.has_fault && bounded.progress == progress && core.registers() == expected,
        "CALL/RTS predicate crossed a cycle bound before completing its stack effect");
}

void conditional_stack() {
  constexpr std::uint64_t call = 1ULL << 39;
  constexpr std::uint64_t rts_eq = 0x0a0000000000;
  const std::array operations{
      jump(0, fixture::code_address + 2, false) | call, jump(0, 2, true) | call, rts_eq};
  for (const auto operation : operations) {
    const std::array words{operation, std::uint64_t{0}, std::uint64_t{0}};
    fixture::Program program(words);
    program.code.variants[0].entries.push_back(fixture::code_address + 2);
    for (const auto mode : {sharc::ExecutionMode::strict, sharc::ExecutionMode::fast_fp32}) {
      const auto native = sharc::Core::compile(program.code, {.mode = mode, .callbacks = {}});
      for (const auto &predicate : fixture::branch_cases) {
        if (predicate.condition != 0 || predicate.saturate) { continue; }
        verify_stack_predicate(predicate, program, native, mode, operation == rts_eq);
      }
    }
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
  static_calls();
  conditional_stack();
}
} // namespace fixture
