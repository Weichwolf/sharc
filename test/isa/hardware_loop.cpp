// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "../support/prefix.hpp"

#include <array>
#include <cstdint>
#include <sharc/core.hpp>
#include <span>
#include <vector>

namespace {
using fixture::check;
using fixture::Prefix;
constexpr auto base = fixture::code_address;
constexpr std::uint64_t do_immediate = 0x0c0000000000;
constexpr std::uint64_t do_r9 = 0x0d0900000000;
constexpr std::uint64_t do_i8 = 0x0d1800000000;
constexpr std::uint64_t do_lce = 0x0e1e00000000;
constexpr std::uint64_t do_eq = 0x0e0000000000;
constexpr std::uint32_t counted_address = 0xef000000;
constexpr std::uint32_t empty = (1U << 22) | (1U << 26);
constexpr std::uint32_t loop_overflow = 1U << 25;
constexpr std::uint32_t old_lcntr = 0x13579bdf;
constexpr unsigned raw_shift = 8;
constexpr unsigned count_shift = 24;

struct Timeline {
  sharc::Registers registers;
  std::vector<Prefix> prefixes;

  void boundary(std::uint32_t pc, unsigned cycles = 1) {
    fixture::boundary(registers, {.pc = pc, .clock = registers.emuclk + cycles});
  }

  void record() { prefixes.push_back({.registers = registers, .cycles = registers.emuclk}); }
};

sharc::Registers starting() {
  sharc::Registers registers;
  registers.pc = base;
  registers.pe[0].stky = empty | loop_overflow;
  registers.lcntr = old_lcntr;
  return registers;
}

struct LoopSetup {
  std::uint32_t address, count;
};

void entered(Timeline &trace, LoopSetup setup) {
  auto &registers = trace.registers;
  const auto begin = registers.pc + 1;
  registers.pc_stack[0] = begin;
  registers.pc_stack_pointer = 1;
  registers.loop_address_stack[0] = setup.address;
  registers.loop_counter_stack[0] = setup.count;
  registers.loop_address_depth = 1;
  registers.loop_counter_depth = 1;
  registers.pe[0].stky &= ~empty;
  trace.boundary(begin);
  trace.record();
}

void popped(sharc::Registers &registers) {
  registers.pc_stack_pointer = 0;
  registers.loop_address_depth = 0;
  registers.loop_counter_depth = 0;
  registers.pe[0].stky |= empty;
}

struct Range {
  std::uint32_t start, end;
};

void tail(Timeline &trace, Range range, bool repeats) {
  const auto [begin, end] = range;
  auto &registers = trace.registers;
  // Table 4-13 shows a new Fetch1 address on every boundary in
  // the four-word tail. Decode sees the redirect two cycles later.
  const std::array fetch{begin, begin + 1, begin + 2, begin + 3};
  const std::array decode{end - 1, end, begin, begin + 1};
  for (unsigned i = 0; i < fetch.size(); ++i) {
    trace.boundary(end - 3 + i);
    if (repeats) {
      registers.faddr = fetch[i];
      registers.daddr = decode[i];
    }
    trace.record();
  }
  trace.boundary(repeats ? begin : end + 1);
  trace.record();
}

void counted_turn(Timeline &trace, unsigned turn, bool repeats) {
  auto &registers = trace.registers;
  registers.pe[0].data[0][0] = std::uint64_t{turn} << raw_shift;
  --registers.loop_counter_stack[0];
  if (!repeats) { popped(registers); }
  tail(trace, {.start = base + 1, .end = base + 5}, repeats);
}

void counted_case(std::uint64_t setup, sharc::Registers registers, std::uint32_t count) {
  constexpr auto end = base + 5;
  const std::array words{setup | 5,
                         fixture::add(0, 0, 1),
                         std::uint64_t{0},
                         std::uint64_t{0},
                         std::uint64_t{0},
                         std::uint64_t{0},
                         std::uint64_t{0}};
  fixture::Program program(words);
  registers.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = registers, .prefixes = {}};
  trace.registers.lcntr = count;
  entered(trace, {.address = counted_address | end, .count = count});
  const auto turns = count == 0 ? 2U : count;
  for (unsigned turn = 1; turn <= turns; ++turn) {
    counted_turn(trace, turn, count == 0 || turn < count);
  }
  if (count != 0) {
    trace.boundary(end + 2);
    trace.record();
  }
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    sharc::Core replay(program.memory(), mode);
    fixture::install(core, native, registers);
    (void)replay.install(native, fixture::Program::variant_id);
    fixture::verify_prefixes(core, replay, trace.prefixes);
  }
}

void counted_forms() {
  constexpr auto count = 3U;
  counted_case(do_immediate | (std::uint64_t{count} << count_shift), starting(), count);
  counted_case(do_immediate, starting(), 0);
  auto registers = starting();
  registers.lcntr = count;
  counted_case(do_lce, registers, count);
  constexpr auto high_bank = 1U << 7;
  constexpr auto raw_extension = 0xa5;
  registers.mode1 = high_bank;
  registers.pe[0].data[1][9] = (count << raw_shift) | raw_extension;
  counted_case(do_r9, registers, count);
  registers = starting();
  constexpr auto alternate_dag2 = 1U << 6;
  registers.mode1 = alternate_dag2;
  registers.dag[1][8].index = count;
  counted_case(do_i8, registers, count);
}

Timeline arithmetic_prefix(sharc::Registers registers, bool both_zero) {
  constexpr auto begin = base + 1;
  constexpr auto end = base + 6;
  constexpr std::uint32_t az = 1;
  Timeline trace{.registers = registers, .prefixes = {}};
  entered(trace, {.address = end, .count = old_lcntr});
  auto &state = trace.registers;
  trace.boundary(begin + 1);
  trace.record();
  trace.boundary(begin + 2);
  state.faddr = begin;
  trace.record();
  state.pe[0].astat = az;
  state.pe[1].astat = both_zero ? az : 0;
  state.pe[1].data[0][0] = both_zero ? 0 : 1U << raw_shift;
  trace.boundary(begin + 3);
  state.faddr = begin + 1;
  trace.record();
  // The condition uses the flags already available when E-2 executes.
  // E-2 clears AZ and neither that overwrite nor either remaining word
  // can suppress a termination that was already selected.
  state.pe[0].astat = 0;
  state.pe[1].astat = 0;
  for (auto &lane : state.pe) { lane.data[0][0] = 2U << raw_shift; }
  trace.boundary(end - 1);
  state.faddr = both_zero ? end + 1 : begin + 2;
  state.daddr = begin;
  if (both_zero) { popped(state); }
  trace.record();
  trace.boundary(end);
  state.faddr = both_zero ? end + 2 : begin + 3;
  state.daddr = begin + 1;
  trace.record();
  state.pe[0].astat = az;
  state.pe[1].astat = both_zero ? az : 0;
  state.pe[1].data[0][4] = both_zero ? 0 : 1U << raw_shift;
  trace.boundary(both_zero ? end + 1 : begin, both_zero ? 3U : 1U);
  trace.record();
  if (both_zero) {
    trace.boundary(end + 2, 4);
    trace.record();
  }
  return trace;
}

void arithmetic_do() {
  constexpr auto simd = 1U << 21;
  constexpr std::uint64_t jump_if_zero = 0x060000000000 | (base + 8);
  const std::array words{do_eq | 6,
                         std::uint64_t{0},
                         std::uint64_t{0},
                         fixture::add(0, 1, 2),
                         fixture::add(0, 3, 3),
                         std::uint64_t{0},
                         fixture::add(4, 1, 2),
                         jump_if_zero,
                         std::uint64_t{0}};
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    for (const bool both_zero : {false, true}) {
      auto registers = starting();
      registers.mode1 = simd;
      registers.pe[1].data[0][2] = both_zero ? 0 : 1U << raw_shift;
      for (auto &lane : registers.pe) { lane.data[0][3] = 1U << raw_shift; }
      sharc::Core core(program.memory(), mode);
      sharc::Core replay(program.memory(), mode);
      fixture::install(core, native, registers);
      (void)replay.install(native, fixture::Program::variant_id);
      const auto trace = arithmetic_prefix(registers, both_zero);
      fixture::verify_prefixes(core, replay, trace.prefixes);
    }
  }
}

void forever_do() {
  constexpr auto do_forever = 0x0e3e00000005;
  constexpr auto forever_address = 0x1f000000;
  const std::array words{std::uint64_t{do_forever},
                         fixture::add(0, 0, 1),
                         std::uint64_t{0},
                         std::uint64_t{0},
                         std::uint64_t{0},
                         std::uint64_t{0}};
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  sharc::Core replay(program.memory());
  auto registers = starting();
  registers.pe[0].data[0][1] = 1U << raw_shift;
  fixture::install(core, native, registers);
  (void)replay.install(native, fixture::Program::variant_id);
  Timeline trace{.registers = registers, .prefixes = {}};
  entered(trace, {.address = forever_address | (base + 5), .count = old_lcntr});
  for (unsigned turn = 1; turn <= 3; ++turn) {
    trace.registers.pe[0].data[0][0] = std::uint64_t{turn} << raw_shift;
    tail(trace, {.start = base + 1, .end = base + 5}, true);
  }
  fixture::verify_prefixes(core, replay, trace.prefixes);
}

void nested_pass(Timeline &trace, unsigned outer_turn) {
  constexpr Range outer{.start = base + 1, .end = base + 8};
  constexpr Range inner{.start = base + 3, .end = base + 7};
  auto &state = trace.registers;
  auto &data = state.pe[0].data[0];
  data[4] = std::uint64_t{outer_turn} << raw_shift;
  trace.boundary(base + 2);
  trace.record();
  state.loop_address_depth = state.loop_counter_depth = state.pc_stack_pointer = 2;
  state.pc_stack[1] = inner.start;
  state.loop_address_stack[1] = counted_address | inner.end;
  state.loop_counter_stack[1] = 2;
  trace.boundary(inner.start);
  trace.record();
  data[2] += 1U << raw_shift;
  --state.loop_counter_stack[1];
  tail(trace, inner, true);
  data[2] += 1U << raw_shift;
  --state.loop_counter_stack[1];
  state.loop_address_depth = state.loop_counter_depth = state.pc_stack_pointer = 1;
  trace.boundary(inner.start + 1);
  trace.record();
  --state.loop_counter_stack[0];
  const auto repeats = outer_turn == 1;
  if (!repeats) { popped(state); }
  tail(trace, outer, repeats);
}

void nested_do() {
  constexpr auto two_iterations = do_immediate | (std::uint64_t{2} << count_shift);
  constexpr std::uint64_t nop = 0;
  const std::array words{two_iterations | 8,
                         fixture::add(4, 4, 1),
                         two_iterations | 5,
                         fixture::add(2, 2, 1),
                         nop,
                         nop,
                         nop,
                         nop,
                         nop,
                         nop};
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  sharc::Core replay(program.memory());
  auto registers = starting();
  registers.pe[0].data[0][1] = 1U << raw_shift;
  fixture::install(core, native, registers);
  (void)replay.install(native, fixture::Program::variant_id);
  Timeline trace{.registers = registers, .prefixes = {}};
  trace.registers.lcntr = 2;
  entered(trace, {.address = counted_address | (base + 8), .count = 2});
  nested_pass(trace, 1);
  nested_pass(trace, 2);
  fixture::verify_prefixes(core, replay, trace.prefixes);
}

void refused_do() {
  constexpr auto setup = do_immediate | (std::uint64_t{1} << count_shift) | 5;
  const std::array words{setup,
                         std::uint64_t{0},
                         std::uint64_t{0},
                         std::uint64_t{0},
                         std::uint64_t{0},
                         std::uint64_t{0}};
  const fixture::Program truncated(std::span(words).first(5));
  const auto missing = fixture::refusal(truncated.code, "truncated DO body was prepared");
  check(missing.reason == sharc::FaultReason::undeclared_entry && missing.address == base &&
            missing.instruction == setup,
        "truncated loop fault lost the original setup");
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  auto registers = starting();
  registers.loop_address_depth = 6;
  registers.loop_counter_depth = 6;
  fixture::install(core, native, registers);
  const auto fault = core.run(1);
  check(fault.has_fault && fault.fault.reason == sharc::FaultReason::unsupported_feature &&
            fault.fault.address == base && fault.fault.instruction == setup &&
            fault.progress == sharc::Progress{} && core.registers() == registers,
        "exhausted DO stack changed registers or retired the setup");
}

void skipped_fetch_boundary() {
  constexpr unsigned length = 7;
  constexpr unsigned instruction_bits = 48;
  constexpr auto setup = do_immediate | (std::uint64_t{2} << count_shift) | length;
  constexpr auto end = base + length;
  constexpr std::uint64_t jump = 0x063e00000000 | end;
  std::array<std::uint64_t, 1 + length> words{};
  words[0] = setup;
  words[1] = jump;
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  auto registers = starting();
  fixture::install(core, native, registers);
  Timeline trace{.registers = registers, .prefixes = {}};
  trace.registers.lcntr = 2;
  entered(trace, {.address = counted_address | end, .count = 2});
  trace.boundary(end, 4);
  const auto fault = core.run(3);
  check(fault.has_fault && fault.fault.reason == sharc::FaultReason::unsupported_feature &&
            fault.fault.address == end && fault.fault.instruction_bits == instruction_bits &&
            fault.progress == sharc::Progress{.instructions = 2, .base_cycles = 5} &&
            core.registers() == trace.registers,
        "unqualified jump across the fetch decision retired the endpoint");
}
} // namespace

void fixture::hardware_loop_control_flow() {
  counted_forms();
  arithmetic_do();
  forever_do();
  nested_do();
  refused_do();
  skipped_fetch_boundary();
}
