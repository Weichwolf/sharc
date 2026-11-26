// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "../support/prefix.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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
constexpr std::uint64_t call_direct = 0x06be00000000, call_dag2 = 0x08be00000000;
constexpr std::uint64_t jump_direct = 0x063e00000000, rts = 0x0a3e00000000;
constexpr std::uint64_t lr = 1ULL << 24, db = 1ULL << 26;
constexpr std::uint64_t la = 1ULL << 38;
constexpr std::uint32_t call_tag = 1U << 24, termination_tag = 1U << 25;
constexpr std::uint32_t pc_empty = 1U << 22, loop_empty = 1U << 26;
constexpr std::uint32_t counted_address = 0xef000000;
constexpr std::uint32_t single_word_counted_address = 0x4f000000;
constexpr std::uint32_t empty = pc_empty | loop_empty;
constexpr std::uint32_t loop_overflow = 1U << 25;
constexpr std::uint32_t old_lcntr = 0x13579bdf;
constexpr unsigned raw_shift = 8;
constexpr unsigned count_shift = 24;
constexpr std::uint32_t az = 1, an = 4, ac = 8;

constexpr std::uint64_t if_not_lce(std::uint64_t op) {
  constexpr auto field = 31ULL << 33;
  constexpr auto not_lce = 15ULL << 33;
  return (op & ~field) | not_lce;
}

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

struct CallTransfer {
  std::uint32_t target, raw_return;
};

void entered_call(Timeline &trace, CallTransfer call, unsigned cycles = 4) {
  auto &state = trace.registers;
  state.pc_stack[state.pc_stack_pointer++] = call.raw_return;
  state.pe[0].stky &= ~pc_empty;
  trace.boundary(call.target, cycles);
  trace.record();
}

void popped_call(sharc::Registers &state) {
  if (--state.pc_stack_pointer == 0) { state.pe[0].stky |= pc_empty; }
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

struct ShortBoundary {
  unsigned pc, fetch, decode;
  unsigned cycles{1};
  std::uint32_t count;
  bool frame_live{};
};

void short_counted_case(unsigned length,
                        std::uint32_t count,
                        std::span<const ShortBoundary> boundaries,
                        std::uint64_t successor = 0,
                        std::span<const bool> enabled = {}) {
  constexpr std::array address_types{
      single_word_counted_address, 0x8f000000U, 0xcf000000U, counted_address};
  constexpr std::array setup_fetch{1U, 2U, 2U, 4U};
  constexpr std::array setup_decode{1U, 2U, 3U, 2U};
  std::vector<std::uint64_t> words(length + 2, fixture::add(0, 0, 1));
  words.front() = do_immediate | (std::uint64_t{count} << count_shift) | length;
  words.back() = successor;
  if (!enabled.empty()) {
    for (unsigned pc = 2; pc <= length; ++pc) { words[pc] = if_not_lce(words[pc]); }
  }
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  trace.registers.lcntr = count;
  entered(trace, {.address = address_types[length - 1] | (base + length), .count = count});
  trace.registers.faddr = base + setup_fetch[length - 1];
  trace.registers.daddr = base + setup_decode[length - 1];
  trace.prefixes.back().registers = trace.registers;
  for (std::size_t i = 0; i < boundaries.size(); ++i) {
    const auto &expected = boundaries[i];
    if (trace.registers.pc != base + length + 1 && (enabled.empty() || enabled[i])) {
      trace.registers.pe[0].data[0][0] += 1U << raw_shift;
    }
    trace.boundary(base + expected.pc, expected.cycles);
    trace.registers.faddr = base + expected.fetch;
    trace.registers.daddr = base + expected.decode;
    trace.registers.loop_counter_stack[0] = expected.count;
    if (!expected.frame_live) { popped(trace.registers); }
    trace.record();
  }
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    sharc::Core replay(program.memory(), mode);
    fixture::install(core, native, initial);
    (void)replay.install(native, fixture::Program::variant_id);
    fixture::verify_prefixes(core, replay, trace.prefixes);
  }
}

void single_word_loops() {
  // Table 4-17: Fetch1 can leave the body four retirements before Execute.
  constexpr std::array five{
      ShortBoundary{.pc = 1, .fetch = 2, .decode = 1, .cycles = 2, .count = 3},
      ShortBoundary{.pc = 1, .fetch = 3, .decode = 1, .count = 3},
      ShortBoundary{.pc = 1, .fetch = 4, .decode = 2, .count = 3},
      ShortBoundary{.pc = 1, .fetch = 5, .decode = 3, .count = 3},
      ShortBoundary{.pc = 2, .fetch = 6, .decode = 4, .count = 3}};
  short_counted_case(1, 5, five);
  constexpr std::array four{
      ShortBoundary{.pc = 1, .fetch = 3, .decode = 1, .cycles = 2, .count = 3},
      five[2],
      five[3],
      five[4]};
  short_counted_case(1, 4, four);
  // Grants enter after the fill and stop before the fetch that pops the frame.
  constexpr std::array nine{
      ShortBoundary{.pc = 1, .fetch = 1, .decode = 1, .cycles = 2, .count = 7, .frame_live = true},
      ShortBoundary{.pc = 1, .fetch = 1, .decode = 1, .count = 6, .frame_live = true},
      ShortBoundary{.pc = 1, .fetch = 1, .decode = 1, .count = 5, .frame_live = true},
      ShortBoundary{.pc = 1, .fetch = 1, .decode = 1, .count = 4, .frame_live = true},
      ShortBoundary{.pc = 1, .fetch = 2, .decode = 1, .count = 3},
      five[1],
      five[2],
      five[3],
      five[4]};
  short_counted_case(1, 9, nine);
  // Tables 4-19 to 4-21: shorter counts drain through three empty clocks.
  constexpr std::array three{
      ShortBoundary{.pc = 1, .fetch = 1, .decode = 1, .cycles = 2, .count = 1, .frame_live = true},
      ShortBoundary{.pc = 1, .fetch = 2, .decode = 1, .count = 0},
      ShortBoundary{.pc = 2, .fetch = 6, .decode = 4, .cycles = 4, .count = 0}};
  constexpr std::array two{ShortBoundary{.pc = 1, .fetch = 2, .decode = 1, .cycles = 2, .count = 0},
                           three.back()};
  constexpr std::array one{
      ShortBoundary{.pc = 2, .fetch = 6, .decode = 4, .cycles = 5, .count = 0}};
  constexpr std::array zero{
      ShortBoundary{
          .pc = 1, .fetch = 1, .decode = 1, .cycles = 2, .count = 0xfffffffe, .frame_live = true},
      ShortBoundary{.pc = 1, .fetch = 1, .decode = 1, .count = 0xfffffffd, .frame_live = true}};
  short_counted_case(1, 3, three);
  short_counted_case(1, 2, two);
  short_counted_case(1, 1, one);
  short_counted_case(1, 0, zero);
}

void multiword_short_loops() {
  // Tables 4-22 to 4-24: after the fill one word comes from the loop buffer.
  constexpr std::array two_three{
      ShortBoundary{.pc = 2, .fetch = 2, .decode = 2, .cycles = 2, .count = 2, .frame_live = true},
      ShortBoundary{.pc = 1, .fetch = 3, .decode = 1, .count = 1},
      ShortBoundary{.pc = 2, .fetch = 4, .decode = 2, .count = 1},
      ShortBoundary{.pc = 1, .fetch = 5, .decode = 3, .count = 1},
      ShortBoundary{.pc = 2, .fetch = 6, .decode = 4, .count = 1},
      ShortBoundary{.pc = 3, .fetch = 7, .decode = 5, .count = 1}};
  constexpr std::array two_two{
      ShortBoundary{.pc = 2, .fetch = 4, .decode = 2, .cycles = 2, .count = 1},
      two_three[3],
      two_three[4],
      two_three[5]};
  constexpr std::array two_one{
      ShortBoundary{.pc = 2, .fetch = 4, .decode = 2, .cycles = 2, .count = 0},
      ShortBoundary{.pc = 3, .fetch = 7, .decode = 5, .cycles = 3, .count = 0}};
  short_counted_case(2, 3, two_three);
  short_counted_case(2, 2, two_two);
  short_counted_case(2, 1, two_one);
  // A held startup sample is already one clock older at the second instruction.
  constexpr std::array two_three_enabled{true, true, true, false, true, false};
  constexpr std::array two_two_enabled{true, false, true, false};
  constexpr std::array two_one_enabled{true, false};
  short_counted_case(2, 3, two_three, 0, two_three_enabled);
  short_counted_case(2, 2, two_two, 0, two_two_enabled);
  short_counted_case(2, 1, two_one, 0, two_one_enabled);

  // Tables 4-25 and 4-26: a three-word body does not stall on entry.
  constexpr std::array three_two{ShortBoundary{.pc = 2, .fetch = 3, .decode = 1, .count = 1},
                                 ShortBoundary{.pc = 3, .fetch = 4, .decode = 2, .count = 1},
                                 ShortBoundary{.pc = 1, .fetch = 5, .decode = 3, .count = 1},
                                 ShortBoundary{.pc = 2, .fetch = 6, .decode = 4, .count = 1},
                                 ShortBoundary{.pc = 3, .fetch = 7, .decode = 5, .count = 1},
                                 ShortBoundary{.pc = 4, .fetch = 8, .decode = 6, .count = 1}};
  constexpr std::array three_one{
      ShortBoundary{.pc = 2, .fetch = 3, .decode = 3, .count = 1, .frame_live = true},
      ShortBoundary{.pc = 3, .fetch = 4, .decode = 3, .count = 0},
      ShortBoundary{.pc = 4, .fetch = 8, .decode = 6, .cycles = 4, .count = 0}};
  short_counted_case(3, 2, three_two);
  short_counted_case(3, 1, three_one);
  constexpr std::array three_two_enabled{true, true, false, true, false, false};
  constexpr std::array three_one_enabled{true, true, true};
  short_counted_case(3, 2, three_two, 0, three_two_enabled);
  short_counted_case(3, 1, three_one, 0, three_one_enabled);

  // Table 4-27 carries the held decode address through the last body word.
  constexpr std::array four_one{
      ShortBoundary{.pc = 2, .fetch = 6, .decode = 3, .cycles = 2, .count = 0},
      ShortBoundary{.pc = 3, .fetch = 7, .decode = 4, .count = 0},
      ShortBoundary{.pc = 4, .fetch = 8, .decode = 5, .count = 0},
      ShortBoundary{.pc = 5, .fetch = 9, .decode = 6, .count = 0},
      ShortBoundary{.pc = 6, .fetch = 10, .decode = 8, .count = 0}};
  short_counted_case(4, 1, four_one);
  constexpr std::array four_two{
      ShortBoundary{.pc = 2, .fetch = 2, .decode = 3, .cycles = 2, .count = 1, .frame_live = true},
      ShortBoundary{.pc = 3, .fetch = 3, .decode = 4, .count = 1, .frame_live = true},
      ShortBoundary{.pc = 4, .fetch = 4, .decode = 1, .count = 1, .frame_live = true},
      ShortBoundary{.pc = 1, .fetch = 5, .decode = 2, .count = 0},
      ShortBoundary{.pc = 2, .fetch = 6, .decode = 3, .count = 0},
      four_one[1],
      four_one[2],
      four_one[3]};
  short_counted_case(4, 2, four_two);
  constexpr std::array four_one_enabled{true, false, false, false, false};
  constexpr std::array four_two_enabled{true, true, true, true, true, false, false, false};
  short_counted_case(4, 1, four_one, 0, four_one_enabled);
  short_counted_case(4, 2, four_two, 0, four_two_enabled);
  auto after_jump = four_one;
  after_jump.back() = ShortBoundary{.pc = 5, .fetch = 9, .decode = 7, .cycles = 4, .count = 0};
  constexpr auto jump_to_successor = std::uint64_t{0x063e00000000} | (base + 5);
  short_counted_case(4, 1, after_jump, jump_to_successor);
}

struct ShortAddress {
  unsigned pc, fetch, decode;
};

void short_boundary(Timeline &trace, ShortAddress address, unsigned cycles = 1) {
  trace.boundary(base + address.pc, cycles);
  trace.registers.faddr = base + address.fetch;
  trace.registers.daddr = base + address.decode;
  trace.record();
}

void entered_arithmetic(Timeline &trace, ShortAddress address, std::uint32_t loop_address) {
  entered(trace, {.address = loop_address, .count = old_lcntr});
  trace.registers.faddr = base + address.fetch;
  trace.registers.daddr = base + address.decode;
  trace.prefixes.back().registers = trace.registers;
}

void verify_timeline(fixture::Program &program,
                     const sharc::Registers &initial,
                     const Timeline &trace) {
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    sharc::Core replay(program.memory(), mode);
    fixture::install(core, native, initial);
    (void)replay.install(native, fixture::Program::variant_id);
    fixture::verify_prefixes(core, replay, trace.prefixes);
  }
}

void single_word_arithmetic() {
  constexpr std::array words{do_eq | 1, fixture::add(0, 0, 1), std::uint64_t{0}};
  constexpr auto arithmetic_type = 1U << 29;
  constexpr auto minus_one = std::numeric_limits<std::uint32_t>::max();
  fixture::Program program(words);
  for (const auto initial_count : {1U, 2U}) {
    auto initial = starting();
    initial.pe[0].data[0][0] = std::uint64_t{initial_count} << raw_shift;
    initial.pe[0].data[0][1] = std::uint64_t{minus_one} << raw_shift;
    Timeline trace{.registers = initial, .prefixes = {}};
    auto &lane = trace.registers.pe[0];
    entered_arithmetic(trace, {.pc = 1, .fetch = 1, .decode = 1}, arithmetic_type | (base + 1));
    if (initial_count == 2) {
      lane.data[0][0] = 1U << raw_shift;
      lane.astat = ac;
      short_boundary(trace, {.pc = 1, .fetch = 1, .decode = 1}, 2);
      lane.data[0][0] = 0;
      lane.astat = az | ac;
      short_boundary(trace, {.pc = 1, .fetch = 1, .decode = 1});
    }
    // The one-word form samples every clock. A result from its first word
    // can be sampled on the fill bubble before any buffered turn retires.
    lane.data[0][0] = initial_count == 1 ? 0 : std::uint64_t{minus_one} << raw_shift;
    lane.astat = initial_count == 1 ? az | ac : an;
    popped(trace.registers);
    short_boundary(trace, {.pc = 1, .fetch = 1, .decode = 1}, initial_count == 1 ? 2U : 1U);
    const auto first_buffered = initial_count == 1 ? minus_one : minus_one - 1;
    lane.astat = initial_count == 1 ? an : an | ac;
    lane.data[0][0] = std::uint64_t{first_buffered} << raw_shift;
    short_boundary(trace, {.pc = 1, .fetch = 2, .decode = 1});
    lane.data[0][0] = std::uint64_t{first_buffered - 1} << raw_shift;
    lane.astat = an | ac;
    short_boundary(trace, {.pc = 1, .fetch = 3, .decode = 1});
    lane.data[0][0] = std::uint64_t{first_buffered - 2} << raw_shift;
    short_boundary(trace, {.pc = 2, .fetch = 6, .decode = 4}, 3);
    verify_timeline(program, initial, trace);
  }
}

void single_word_simd_condition() {
  constexpr std::array words{do_eq | 1, std::uint64_t{0}, std::uint64_t{0}};
  constexpr auto simd = 1U << 21;
  constexpr auto arithmetic_type = 1U << 29;
  fixture::Program program(words);
  auto initial = starting();
  initial.mode1 = simd;
  initial.pe[0].astat = az;
  Timeline trace{.registers = initial, .prefixes = {}};
  entered_arithmetic(trace, {.pc = 1, .fetch = 1, .decode = 1}, arithmetic_type | (base + 1));
  short_boundary(trace, {.pc = 1, .fetch = 1, .decode = 1}, 2);
  short_boundary(trace, {.pc = 1, .fetch = 1, .decode = 1});
  short_boundary(trace, {.pc = 1, .fetch = 1, .decode = 1});
  verify_timeline(program, initial, trace);
}

void two_word_arithmetic() {
  const auto zero = fixture::add(0, 3, 3);
  const auto nonzero = fixture::add(2, 1, 1);
  for (const bool first_sets_zero : {true, false}) {
    const std::array words{do_eq | 2,
                           first_sets_zero ? zero : std::uint64_t{0},
                           first_sets_zero ? nonzero : zero,
                           std::uint64_t{0}};
    fixture::Program program(words);
    auto initial = starting();
    initial.pe[0].data[0][0] = initial.pe[0].data[0][1] = 1U << raw_shift;
    Timeline trace{.registers = initial, .prefixes = {}};
    auto &lane = trace.registers.pe[0];
    entered_arithmetic(trace, {.pc = 1, .fetch = 2, .decode = 2}, base + 2);
    if (first_sets_zero) {
      lane.data[0][0] = 0;
      lane.astat = az;
    }
    short_boundary(trace, {.pc = 2, .fetch = 2, .decode = 2}, 2);
    if (first_sets_zero) {
      lane.data[0][2] = 2U << raw_shift;
      lane.astat = 0;
    } else {
      // The fresh flags of the last word miss this sample. A full pass
      // finishes before the next sample pops the stacks.
      lane.data[0][0] = 0;
      lane.astat = az;
      short_boundary(trace, {.pc = 1, .fetch = 1, .decode = 1});
      short_boundary(trace, {.pc = 2, .fetch = 2, .decode = 2});
    }
    popped(trace.registers);
    short_boundary(trace, {.pc = 1, .fetch = 3, .decode = 1});
    if (first_sets_zero) { lane.astat = az; }
    short_boundary(trace, {.pc = 2, .fetch = 4, .decode = 2});
    if (first_sets_zero) { lane.astat = 0; }
    short_boundary(trace, {.pc = 3, .fetch = 7, .decode = 5}, 3);
    verify_timeline(program, initial, trace);
  }
}

void three_word_arithmetic() {
  constexpr std::array words{
      do_eq | 3, fixture::add(2, 1, 1), std::uint64_t{0}, fixture::add(0, 3, 3), std::uint64_t{0}};
  constexpr auto simd = 1U << 21;
  fixture::Program program(words);
  for (const bool both_zero : {true, false}) {
    auto initial = starting();
    initial.mode1 = simd;
    for (auto &lane : initial.pe) { lane.data[0][1] = 1U << raw_shift; }
    initial.pe[1].data[0][3] = both_zero ? 0 : 1U << raw_shift;
    Timeline trace{.registers = initial, .prefixes = {}};
    auto &state = trace.registers;
    entered_arithmetic(trace, {.pc = 1, .fetch = 2, .decode = 3}, base + 3);
    for (auto &lane : state.pe) { lane.data[0][2] = 2U << raw_shift; }
    short_boundary(trace, {.pc = 2, .fetch = 3, .decode = 1});
    short_boundary(trace, {.pc = 3, .fetch = 1, .decode = 2});
    state.pe[0].astat = az;
    state.pe[1].astat = both_zero ? az : 0;
    state.pe[1].data[0][0] = both_zero ? 0 : 2U << raw_shift;
    short_boundary(trace, {.pc = 1, .fetch = 2, .decode = 3});
    // Fetch1 names B+1 while B executes. B's new flags cannot undo
    // the decision, and the rest of this pass and one further pass retire.
    for (auto &lane : state.pe) { lane.astat = 0; }
    if (both_zero) { popped(state); }
    short_boundary(trace, {.pc = 2, .fetch = 3, .decode = 1});
    short_boundary(trace, {.pc = 3, .fetch = 1, .decode = 2});
    state.pe[0].astat = az;
    state.pe[1].astat = both_zero ? az : 0;
    short_boundary(trace, {.pc = 1, .fetch = 2, .decode = 3});
    if (both_zero) {
      for (auto &lane : state.pe) { lane.astat = 0; }
      short_boundary(trace, {.pc = 2, .fetch = 4, .decode = 1});
      short_boundary(trace, {.pc = 3, .fetch = 5, .decode = 2});
      for (auto &lane : state.pe) { lane.astat = az; }
      short_boundary(trace, {.pc = 4, .fetch = 8, .decode = 6}, 3);
    }
    verify_timeline(program, initial, trace);
  }
}

void four_word_arithmetic() {
  constexpr std::array words{do_eq | 4,
                             fixture::add(0, 3, 3),
                             fixture::add(2, 1, 1),
                             std::uint64_t{0},
                             std::uint64_t{0},
                             std::uint64_t{0}};
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &lane = trace.registers.pe[0];
  entered_arithmetic(trace, {.pc = 1, .fetch = 1, .decode = 3}, base + 4);
  lane.astat = az;
  short_boundary(trace, {.pc = 2, .fetch = 2, .decode = 4});
  lane.astat = 0;
  lane.data[0][2] = 2U << raw_shift;
  popped(trace.registers);
  short_boundary(trace, {.pc = 3, .fetch = 5, .decode = 1});
  short_boundary(trace, {.pc = 4, .fetch = 6, .decode = 2});
  short_boundary(trace, {.pc = 5, .fetch = 9, .decode = 7}, 3);
  verify_timeline(program, initial, trace);
}

void short_forever_do() {
  constexpr std::uint64_t setup = 0x0e3e00000002;
  constexpr std::uint32_t forever_address = 0x1f000000;
  constexpr std::array words{setup, fixture::add(0, 0, 1), std::uint64_t{0}, std::uint64_t{0}};
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  entered_arithmetic(trace, {.pc = 1, .fetch = 2, .decode = 2}, forever_address | (base + 2));
  for (unsigned turn = 1; turn <= 3; ++turn) {
    trace.registers.pe[0].data[0][0] = std::uint64_t{turn} << raw_shift;
    short_boundary(trace, {.pc = 2, .fetch = 2, .decode = 2}, turn == 1 ? 2U : 1U);
    short_boundary(trace, {.pc = 1, .fetch = 1, .decode = 1});
  }
  verify_timeline(program, initial, trace);
}

Timeline arithmetic_prefix(sharc::Registers registers, bool both_zero) {
  constexpr auto begin = base + 1;
  constexpr auto end = base + 6;
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

void enter_arithmetic_child(Timeline &trace) {
  auto &state = trace.registers;
  state.loop_address_depth = state.loop_counter_depth = state.pc_stack_pointer = 2;
  state.pc_stack[1] = base + 3;
  state.loop_address_stack[1] = base + 7;
  state.loop_counter_stack[1] = state.lcntr;
  trace.boundary(base + 3);
  trace.record();
}

void leave_nested_child(sharc::Registers &state) {
  state.loop_address_depth = state.loop_counter_depth = state.pc_stack_pointer = 1;
}

void child_arithmetic_pass(Timeline &trace, bool repeats) {
  auto &state = trace.registers;
  state.pe[0].data[0][0] = repeats ? 1U << raw_shift : 0;
  state.pe[0].astat = repeats ? ac : ac | az;
  short_boundary(trace, {.pc = 4, .fetch = 3, .decode = 6});
  short_boundary(trace, {.pc = 5, .fetch = 4, .decode = 7});
  if (repeats) {
    short_boundary(trace, {.pc = 6, .fetch = 5, .decode = 3});
    short_boundary(trace, {.pc = 7, .fetch = 6, .decode = 4});
    trace.boundary(base + 3);
    trace.record();
  } else {
    leave_nested_child(state);
    short_boundary(trace, {.pc = 6, .fetch = 8, .decode = 3});
  }
}

void parent_count_sample(Timeline &trace, bool repeats) {
  --trace.registers.loop_counter_stack[0];
  if (!repeats) { popped(trace.registers); }
}

constexpr unsigned first_parent_word = 8;

void parent_lce_add(Timeline &trace, unsigned pc) {
  auto &lane = trace.registers.pe[0];
  lane.data[0][pc - first_parent_word + 2] += 1U << raw_shift;
  lane.astat = 0;
}

enum class OverlapTail : std::uint8_t { ordinary, endpoint_call, not_lce };

void parent_count_tail(Timeline &trace, unsigned end, bool repeats, OverlapTail tail) {
  for (unsigned pc = first_parent_word; pc <= end; ++pc) {
    if (tail == OverlapTail::not_lce && pc > first_parent_word &&
        (repeats || (end == first_parent_word + 3 && pc == first_parent_word + 1))) {
      // At gap four the first parent compute still sees the old condition.
      parent_lce_add(trace, pc - 1);
    }
    trace.boundary(base + pc, pc == first_parent_word ? 3U : 1U);
    if (repeats) {
      constexpr std::array fetch{4U, 3U, 2U, 1U};
      const auto distance = end - pc;
      trace.registers.faddr = base + fetch[distance];
      if (distance < 2) { trace.registers.daddr = base + 2 - distance; }
    }
    trace.record();
  }
}

constexpr auto overlap_subroutine = base + 11;

void finish_overlapping_outer(Timeline &trace, std::uint32_t next, bool calls) {
  if (calls) {
    entered_call(trace, {.target = overlap_subroutine, .raw_return = call_tag | next});
    trace.boundary(overlap_subroutine + 1);
    trace.record();
    popped_call(trace.registers);
  }
  trace.boundary(next, calls ? 4U : 1U);
  trace.record();
}

void counted_arithmetic_turn(Timeline &trace, unsigned end, OverlapTail tail, bool repeats) {
  trace.registers.pe[0].data[0][0] = 2U << raw_shift;
  trace.boundary(base + 2);
  trace.record();
  enter_arithmetic_child(trace);
  child_arithmetic_pass(trace, true);
  child_arithmetic_pass(trace, false);
  // After termination the child's Fetch1 reaches +8 while +6 executes,
  // +9 while +7 executes, then +10/+11 on the two bubbles.
  const bool early = end == first_parent_word;
  if (early) { parent_count_sample(trace, repeats); }
  short_boundary(trace, {.pc = 7, .fetch = early && repeats ? 1U : 9U, .decode = 4});
  if (!early) { parent_count_sample(trace, repeats); }
  parent_count_tail(trace, end, repeats, tail);
  if (tail == OverlapTail::not_lce && repeats) { parent_lce_add(trace, end); }
  finish_overlapping_outer(
      trace, base + (repeats ? 1U : end + 1), tail == OverlapTail::endpoint_call);
}

void counted_arithmetic_overlap() {
  constexpr unsigned child_end = 7;
  constexpr auto minus_one = std::numeric_limits<std::uint32_t>::max();
  constexpr auto counted_two = do_immediate | (std::uint64_t{2} << count_shift);

  struct OverlapCase {
    unsigned gap;
    OverlapTail tail{OverlapTail::ordinary};
  };

  constexpr std::array cases{OverlapCase{.gap = 1},
                             OverlapCase{.gap = 2},
                             OverlapCase{.gap = 3},
                             OverlapCase{.gap = 4},
                             OverlapCase{.gap = 1, .tail = OverlapTail::endpoint_call},
                             OverlapCase{.gap = 1, .tail = OverlapTail::not_lce},
                             OverlapCase{.gap = 2, .tail = OverlapTail::not_lce},
                             OverlapCase{.gap = 3, .tail = OverlapTail::not_lce},
                             OverlapCase{.gap = 4, .tail = OverlapTail::not_lce}};
  for (const auto &[gap, tail] : cases) {
    const auto end = child_end + gap;
    std::array<std::uint64_t, 13> words{};
    words[0] = counted_two | end;
    words[1] = fixture::immediate(0, 2);
    words[2] = do_eq | 5;
    words[3] = fixture::add(0, 0, 1);
    if (tail == OverlapTail::endpoint_call) {
      words[end] = call_direct | overlap_subroutine;
      words[overlap_subroutine - base + 1] = rts | lr;
    }
    if (tail == OverlapTail::not_lce) {
      for (unsigned pc = first_parent_word; pc <= end; ++pc) {
        const auto reg = pc - first_parent_word + 2;
        words[pc] = if_not_lce(fixture::add(reg, reg, 7));
      }
    }
    fixture::Program program(words);
    auto initial = starting();
    initial.pe[0].data[0][1] = std::uint64_t{minus_one} << raw_shift;
    initial.pe[0].data[0][7] = 1U << raw_shift;
    Timeline trace{.registers = initial, .prefixes = {}};
    trace.registers.lcntr = 2;
    entered(trace, {.address = counted_address | (base + end), .count = 2});
    for (const bool repeats : {true, false}) { counted_arithmetic_turn(trace, end, tail, repeats); }
    verify_timeline(program, initial, trace);
  }
}

void arithmetic_refill_overlap(bool ending) {
  constexpr auto end = base + 9;
  std::array<std::uint64_t, 11> words{};
  words[0] = do_eq | 9;
  words[2] = do_eq | 5;
  words[3] = fixture::add(0, 0, 0);
  words[6] = fixture::add(2, 1, 1);
  words[7] = ending ? fixture::add(3, 4, 4) : fixture::add(3, 1, 1);
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  entered(trace, {.address = end, .count = old_lcntr});
  trace.boundary(base + 2);
  trace.record();
  enter_arithmetic_child(trace);
  state.pe[0].astat = az;
  short_boundary(trace, {.pc = 4, .fetch = 3, .decode = 6});
  short_boundary(trace, {.pc = 5, .fetch = 4, .decode = 7});
  leave_nested_child(state);
  short_boundary(trace, {.pc = 6, .fetch = 8, .decode = 3});
  state.pe[0].astat = 0;
  state.pe[0].data[0][2] = 2U << raw_shift;
  short_boundary(trace, {.pc = 7, .fetch = 9, .decode = 4});
  // The parent's condition is tested during refill with the flags from
  // the last child instruction, which are not available on entry to +7.
  state.pe[0].astat = ending ? az : 0;
  state.pe[0].data[0][3] = ending ? 0 : 2U << raw_shift;
  if (ending) { popped(state); }
  short_boundary(trace, {.pc = 8, .fetch = ending ? 10U : 3U, .decode = 1}, 3);
  short_boundary(trace, {.pc = 9, .fetch = ending ? 11U : 4U, .decode = 2});
  trace.boundary(ending ? end + 1 : base + 1, ending ? 3U : 1U);
  trace.record();
  verify_timeline(program, initial, trace);
}

constexpr unsigned short_setup_pc = 4;
constexpr std::array short_counted_types{
    single_word_counted_address, 0x8f000000U, 0xcf000000U, counted_address};

void enter_nested_short(Timeline &trace, const ShortBoundary &entry, std::uint32_t address) {
  auto &state = trace.registers;
  const auto depth = state.loop_address_depth;
  state.pc_stack[depth] = base + short_setup_pc + 1;
  state.loop_address_stack[depth] = address;
  state.loop_counter_stack[depth] = entry.count;
  ++state.pc_stack_pointer;
  ++state.loop_address_depth;
  ++state.loop_counter_depth;
  state.lcntr = entry.count;
  state.pe[0].stky &= ~empty;
  short_boundary(trace,
                 {.pc = short_setup_pc + entry.pc,
                  .fetch = short_setup_pc + entry.fetch,
                  .decode = short_setup_pc + entry.decode});
}

struct NestedShortBoundary {
  ShortBoundary child;
  bool parent_sample{};
};

struct NestedShortCase {
  unsigned length, gap;
  ShortBoundary setup;
  std::span<const NestedShortBoundary> body;
};

struct NestedShortLce {
  std::span<const bool> child;
  std::span<const bool> parent;
  bool parent_call{};
};

void finish_counted_short_overlap(Timeline &trace, Range parent_tail, const NestedShortLce &lce) {
  auto &state = trace.registers;
  for (auto pc = parent_tail.start; pc <= parent_tail.end; ++pc) {
    unsigned cycles = 1;
    if (!lce.parent.empty() && lce.parent[pc - parent_tail.start]) {
      if (lce.parent_call && pc == parent_tail.start) {
        const auto tags =
            call_tag | (parent_tail.end - parent_tail.start == 3U ? termination_tag : 0);
        entered_call(trace, {.target = parent_tail.end + 2, .raw_return = tags | (pc + 1)});
        popped_call(state);
        cycles = 4;
      } else {
        state.pe[0].data[0][pc - parent_tail.start + 2] += 1U << raw_shift;
      }
    }
    trace.boundary(pc + 1, cycles);
    trace.record();
  }
}

void counted_short_overlap(const NestedShortCase &sample, const NestedShortLce &lce = {}) {
  constexpr auto outer_once = do_immediate | (std::uint64_t{1} << count_shift);
  const auto [length, gap, setup, body] = sample;
  const auto child_end = short_setup_pc + length;
  const auto parent_end = child_end + gap;
  std::vector<std::uint64_t> words(parent_end + (lce.parent_call ? 3U : 2U));
  words[0] = outer_once | parent_end;
  words[short_setup_pc] = do_immediate | (std::uint64_t{setup.count} << count_shift) | length;
  for (unsigned pc = short_setup_pc + 1; pc <= child_end; ++pc) {
    words[pc] = fixture::add(0, 0, 1);
    if (!lce.child.empty() && pc > short_setup_pc + 1) { words[pc] = if_not_lce(words[pc]); }
  }
  if (!lce.parent.empty()) {
    for (unsigned i = 0; i < gap; ++i) {
      words[child_end + i + 1] = if_not_lce(fixture::add(i + 2, i + 2, 1));
    }
  }
  if (lce.parent_call) {
    const auto callee = parent_end + 2;
    words[child_end + 1] = if_not_lce(call_direct | (base + callee));
    words[callee] = rts | lr;
  }
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  state.lcntr = 1;
  entered(trace, {.address = counted_address | (base + parent_end), .count = 1});
  for (unsigned pc = 2; pc <= short_setup_pc; ++pc) {
    trace.boundary(base + pc);
    trace.record();
  }
  enter_nested_short(trace, setup, short_counted_types[length - 1] | (base + child_end));
  for (std::size_t i = 0; i < body.size(); ++i) {
    const auto &[child, parent_sample] = body[i];
    if (lce.child.empty() || lce.child[i]) { state.pe[0].data[0][0] += 1U << raw_shift; }
    state.loop_counter_stack[1] = child.count;
    if (!child.frame_live && state.loop_address_depth == 2) { leave_nested_child(state); }
    if (parent_sample) {
      state.loop_counter_stack[0] = 0;
      popped(state);
    }
    short_boundary(trace,
                   {.pc = short_setup_pc + child.pc,
                    .fetch = short_setup_pc + child.fetch,
                    .decode = short_setup_pc + child.decode},
                   child.cycles);
  }
  finish_counted_short_overlap(
      trace, {.start = base + child_end + 1, .end = base + parent_end}, lce);
  verify_timeline(program, initial, trace);
}

void compact_counted_short_overlap() {
  constexpr std::array one{
      NestedShortBoundary{
          .child = {.pc = 1, .fetch = 1, .decode = 1, .cycles = 2, .count = 1, .frame_live = true}},
      NestedShortBoundary{.child = {.pc = 1, .fetch = 2, .decode = 1, .count = 0}},
      NestedShortBoundary{.child = {.pc = 2, .fetch = 6, .decode = 4, .cycles = 4, .count = 0},
                          .parent_sample = true}};
  counted_short_overlap({.length = 1,
                         .gap = 3,
                         .setup = {.pc = 1, .fetch = 1, .decode = 1, .count = 3},
                         .body = one});

  constexpr std::array two{
      NestedShortBoundary{.child = {.pc = 2, .fetch = 4, .decode = 2, .cycles = 2, .count = 0}},
      NestedShortBoundary{.child = {.pc = 3, .fetch = 7, .decode = 5, .cycles = 3, .count = 0},
                          .parent_sample = true}};
  counted_short_overlap({.length = 2,
                         .gap = 2,
                         .setup = {.pc = 1, .fetch = 2, .decode = 2, .count = 1},
                         .body = two});
  constexpr std::array compact_child{true, false};
  constexpr std::array gap_two{false, false};
  constexpr std::array gap_three{false, false, false};
  constexpr std::array gap_four{true, false, false, false};
  constexpr ShortBoundary two_setup{.pc = 1, .fetch = 2, .decode = 2, .count = 1};
  counted_short_overlap({.length = 2, .gap = 2, .setup = two_setup, .body = two},
                        {.child = compact_child, .parent = gap_two});
  counted_short_overlap({.length = 2, .gap = 3, .setup = two_setup, .body = two},
                        {.child = compact_child, .parent = gap_three});
  counted_short_overlap({.length = 2, .gap = 4, .setup = two_setup, .body = two},
                        {.child = compact_child, .parent = gap_four});
  // The branch still sees the old condition when the sample is one clock later.
  constexpr std::array gap_three_branch{true, false, false};
  counted_short_overlap({.length = 2, .gap = 2, .setup = two_setup, .body = two},
                        {.child = compact_child, .parent = gap_two, .parent_call = true});
  counted_short_overlap({.length = 2, .gap = 3, .setup = two_setup, .body = two},
                        {.child = compact_child, .parent = gap_three_branch, .parent_call = true});
  counted_short_overlap({.length = 2, .gap = 4, .setup = two_setup, .body = two},
                        {.child = compact_child, .parent = gap_four, .parent_call = true});

  constexpr std::array three{
      NestedShortBoundary{
          .child = {.pc = 2, .fetch = 3, .decode = 3, .count = 1, .frame_live = true}},
      NestedShortBoundary{.child = {.pc = 3, .fetch = 4, .decode = 3, .count = 0}},
      NestedShortBoundary{.child = {.pc = 4, .fetch = 8, .decode = 6, .cycles = 4, .count = 0},
                          .parent_sample = true}};
  counted_short_overlap({.length = 3,
                         .gap = 1,
                         .setup = {.pc = 1, .fetch = 2, .decode = 3, .count = 1},
                         .body = three});

  constexpr std::array four{
      NestedShortBoundary{.child = {.pc = 2, .fetch = 6, .decode = 3, .cycles = 2, .count = 0},
                          .parent_sample = true},
      NestedShortBoundary{.child = {.pc = 3, .fetch = 7, .decode = 4, .count = 0}},
      NestedShortBoundary{.child = {.pc = 4, .fetch = 8, .decode = 5, .count = 0}},
      NestedShortBoundary{.child = {.pc = 5, .fetch = 9, .decode = 6, .count = 0}}};
  counted_short_overlap({.length = 4,
                         .gap = 1,
                         .setup = {.pc = 1, .fetch = 4, .decode = 2, .count = 1},
                         .body = four});
  // Both stacks pop while the child enters its second word. Its remaining
  // predicates still see the short expiration, and the parent's sees its own.
  constexpr std::array four_child{true, false, false, false};
  constexpr std::array four_parent{false};
  counted_short_overlap({.length = 4,
                         .gap = 1,
                         .setup = {.pc = 1, .fetch = 4, .decode = 2, .count = 1},
                         .body = four},
                        {.child = four_child, .parent = four_parent});
}

void short_counted_parent_arithmetic(bool ending) {
  constexpr auto child_end = short_setup_pc + 2;
  constexpr auto parent_end = child_end + 2;
  std::array<std::uint64_t, parent_end + 2> words{};
  words[0] = do_eq | parent_end;
  words[short_setup_pc] = do_immediate | (std::uint64_t{1} << count_shift) | 2;
  words[child_end] = ending ? fixture::add(0, 0, 0) : fixture::add(0, 1, 1);
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  entered(trace, {.address = base + parent_end, .count = old_lcntr});
  for (unsigned pc = 2; pc <= short_setup_pc; ++pc) {
    trace.boundary(base + pc);
    trace.record();
  }
  enter_nested_short(trace,
                     {.pc = 1, .fetch = 2, .decode = 2, .count = 1},
                     short_counted_types[1] | (base + child_end));
  state.loop_counter_stack[1] = 0;
  leave_nested_child(state);
  short_boundary(trace, {.pc = child_end, .fetch = parent_end, .decode = child_end}, 2);
  // The outer test sees the flag set by the last inner word in the refill.
  state.pe[0].astat = ending ? az : 0;
  state.pe[0].data[0][0] = ending ? 0 : 2U << raw_shift;
  if (ending) { popped(state); }
  short_boundary(
      trace, {.pc = child_end + 1, .fetch = ending ? parent_end + 1 : 3U, .decode = 1}, 3);
  short_boundary(trace, {.pc = parent_end, .fetch = ending ? parent_end + 2 : 4U, .decode = 2});
  trace.boundary(base + (ending ? parent_end + 1 : 1), ending ? 3U : 1U);
  trace.record();
  verify_timeline(program, initial, trace);
}

void steady_counted_short_overlap() {
  constexpr auto child_end = short_setup_pc + 1;
  constexpr auto parent_end = child_end + 3;
  std::array<std::uint64_t, parent_end + 2> words{};
  words[0] = do_immediate | (std::uint64_t{2} << count_shift) | parent_end;
  words[short_setup_pc] = do_immediate | (std::uint64_t{5} << count_shift) | 1;
  words[child_end] = fixture::add(0, 0, 1);
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  state.lcntr = 2;
  entered(trace, {.address = counted_address | (base + parent_end), .count = 2});
  for (const bool repeats : {true, false}) {
    for (unsigned pc = 2; pc <= short_setup_pc; ++pc) {
      trace.boundary(base + pc);
      trace.record();
    }
    enter_nested_short(trace,
                       {.pc = 1, .fetch = 1, .decode = 1, .count = 5},
                       single_word_counted_address | (base + child_end));
    auto &sum = state.pe[0].data[0][0];
    sum += 1U << raw_shift;
    state.loop_counter_stack[1] = 3;
    leave_nested_child(state);
    short_boundary(trace, {.pc = child_end, .fetch = 6, .decode = 5}, 2);
    sum += 1U << raw_shift;
    short_boundary(trace, {.pc = child_end, .fetch = 7, .decode = 5});
    sum += 1U << raw_shift;
    short_boundary(trace, {.pc = child_end, .fetch = 8, .decode = 6});
    sum += 1U << raw_shift;
    parent_count_sample(trace, repeats);
    // The parent redirect is visible before the last child word executes.
    short_boundary(trace, {.pc = child_end, .fetch = repeats ? 1U : 9U, .decode = 7});
    sum += 1U << raw_shift;
    short_boundary(trace, {.pc = 6, .fetch = repeats ? 2U : 10U, .decode = 8});
    short_boundary(trace, {.pc = 7, .fetch = repeats ? 3U : 11U, .decode = repeats ? 1U : 9U});
    short_boundary(trace, {.pc = 8, .fetch = repeats ? 4U : 12U, .decode = repeats ? 2U : 10U});
    trace.boundary(base + (repeats ? 1U : parent_end + 1));
    trace.record();
  }
  verify_timeline(program, initial, trace);
}

void short_arithmetic_child_overlap() {
  constexpr auto child_end = short_setup_pc + 2;
  constexpr auto parent_end = child_end + 2;
  std::array<std::uint64_t, parent_end + 2> words{};
  words[0] = do_immediate | (std::uint64_t{2} << count_shift) | parent_end;
  words[short_setup_pc] = do_eq | 2;
  words[short_setup_pc + 1] = fixture::add(0, 0, 0);
  fixture::Program program(words);
  const auto initial = starting();
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  state.lcntr = 2;
  entered(trace, {.address = counted_address | (base + parent_end), .count = 2});
  for (const bool repeats : {true, false}) {
    for (unsigned pc = 2; pc <= short_setup_pc; ++pc) {
      trace.boundary(base + pc);
      trace.record();
    }
    enter_nested_short(trace, {.pc = 1, .fetch = 2, .decode = 2, .count = 2}, base + child_end);
    state.pe[0].astat = az;
    short_boundary(trace, {.pc = child_end, .fetch = child_end, .decode = child_end}, 2);
    leave_nested_child(state);
    short_boundary(trace, {.pc = 5, .fetch = 7, .decode = 5});
    short_boundary(trace, {.pc = child_end, .fetch = parent_end, .decode = child_end});
    parent_count_sample(trace, repeats);
    short_boundary(trace, {.pc = 7, .fetch = repeats ? 3U : 11U, .decode = repeats ? 1U : 9U}, 3);
    short_boundary(trace, {.pc = 8, .fetch = repeats ? 4U : 12U, .decode = repeats ? 2U : 10U});
    trace.boundary(base + (repeats ? 1U : parent_end + 1));
    trace.record();
  }
  verify_timeline(program, initial, trace);
}

struct ShortArithmeticRefill {
  unsigned length, gap;
  std::size_t pop_after;
  std::span<const ShortAddress> drain;

  [[nodiscard]] unsigned child_end() const { return short_setup_pc + length; }

  [[nodiscard]] unsigned parent_end() const { return child_end() + gap; }

  [[nodiscard]] unsigned nominal_sample() const { return parent_end() - 4; }

  [[nodiscard]] bool sampled_before_child() const { return nominal_sample() < short_setup_pc; }
};

void short_arithmetic_parent_boundary(Timeline &trace,
                                      const ShortArithmeticRefill &body,
                                      unsigned pc,
                                      bool repeats,
                                      unsigned cycles = 1) {
  trace.boundary(base + pc, cycles);
  if (repeats) {
    const auto distance = body.parent_end() - pc;
    trace.registers.faddr = base + 4 - distance;
    if (distance < 2) { trace.registers.daddr = base + 2 - distance; }
  }
  trace.record();
}

void short_arithmetic_lce_turn(Timeline &trace, const ShortArithmeticRefill &body, bool repeats) {
  auto &state = trace.registers;
  const bool early = body.sampled_before_child();
  for (unsigned pc = 1; pc < short_setup_pc; ++pc) {
    if (early && pc == body.nominal_sample()) { parent_count_sample(trace, repeats); }
    const bool redirected = early && repeats && pc >= body.nominal_sample();
    short_arithmetic_parent_boundary(trace, body, pc + 1, redirected);
  }
  constexpr std::array fetch{1U, 2U, 2U};
  constexpr auto single_word_type = 1U << 29;
  const auto loop_address = (base + body.child_end()) | (body.length == 1 ? single_word_type : 0);
  enter_nested_short(trace,
                     {.pc = 1, .fetch = fetch[body.length - 1], .decode = body.length, .count = 2},
                     loop_address);
  state.pe[0].astat = az;
  for (std::size_t i = 0; i < body.drain.size(); ++i) {
    const auto &next = body.drain[i];
    if (i == body.pop_after) {
      if (state.loop_address_depth == 2) {
        leave_nested_child(state);
      } else {
        popped(state);
      }
    }
    const bool last_buffered = i + 1 == body.drain.size();
    if (last_buffered && !early && body.gap == 1) { parent_count_sample(trace, repeats); }
    const auto fetch_address =
        last_buffered && repeats && body.gap == 1 ? 1U : short_setup_pc + next.fetch;
    short_boundary(trace,
                   {.pc = short_setup_pc + next.pc,
                    .fetch = fetch_address,
                    .decode = short_setup_pc + next.decode},
                   i == 0 && body.length != 3 ? 2U : 1U);
  }
  if (!early && body.gap != 1) { parent_count_sample(trace, repeats); }
  short_arithmetic_parent_boundary(trace, body, body.child_end() + 1, repeats, 3);
}

void short_arithmetic_not_lce(const ShortArithmeticRefill &body) {
  const auto end = body.parent_end();
  std::vector<std::uint64_t> words(end + 2);
  words[0] = do_immediate | (std::uint64_t{2} << count_shift) | end;
  words[short_setup_pc] = do_eq | body.length;
  words[short_setup_pc + 1] = fixture::add(0, 0, 0);
  for (unsigned i = 0; i < body.gap; ++i) {
    words[body.child_end() + i + 1] = if_not_lce(fixture::add(i + 2, i + 2, 1));
  }
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  trace.registers.lcntr = 2;
  entered(trace, {.address = counted_address | (base + end), .count = 2});
  for (const bool repeats : {true, false}) {
    short_arithmetic_lce_turn(trace, body, repeats);
    for (unsigned i = 0; i < body.gap; ++i) {
      // Only the second refill clock leaves a fresh sample at the first parent word.
      if (repeats || (i == 0 && body.gap == 4)) {
        auto &lane = trace.registers.pe[0];
        lane.data[0][i + 2] += 1U << raw_shift;
        lane.astat = 0;
      }
      const auto pc = body.child_end() + i + 1;
      if (pc == end && repeats) {
        trace.boundary(base + 1);
        trace.record();
      } else {
        short_arithmetic_parent_boundary(trace, body, pc + 1, repeats);
      }
    }
  }
  verify_timeline(program, initial, trace);
}

void short_arithmetic_refill_not_lce() {
  constexpr std::array one{ShortAddress{.pc = 1, .fetch = 1, .decode = 1},
                           ShortAddress{.pc = 1, .fetch = 2, .decode = 1},
                           ShortAddress{.pc = 1, .fetch = 3, .decode = 1}};
  constexpr std::array two{ShortAddress{.pc = 2, .fetch = 2, .decode = 2},
                           ShortAddress{.pc = 1, .fetch = 3, .decode = 1},
                           ShortAddress{.pc = 2, .fetch = 4, .decode = 2}};
  constexpr ShortAddress three_second{.pc = 2, .fetch = 3, .decode = 1};
  constexpr ShortAddress three_last{.pc = 3, .fetch = 1, .decode = 2};
  constexpr ShortAddress three_header{.pc = 1, .fetch = 2, .decode = 3};
  constexpr std::array three{three_second,
                             three_last,
                             three_header,
                             three_second,
                             three_last,
                             three_header,
                             ShortAddress{.pc = 2, .fetch = 4, .decode = 1},
                             ShortAddress{.pc = 3, .fetch = 5, .decode = 2}};
  for (const unsigned gap : {1U, 2U, 3U, 4U}) {
    short_arithmetic_not_lce({.length = 1, .gap = gap, .pop_after = 0, .drain = one});
    short_arithmetic_not_lce({.length = 2, .gap = gap, .pop_after = 1, .drain = two});
    short_arithmetic_not_lce({.length = 3, .gap = gap, .pop_after = 3, .drain = three});
  }
}

void short_child_after_parent_fetch() {
  constexpr auto child_end = short_setup_pc + 1;
  constexpr auto parent_end = child_end + 1;
  std::array<std::uint64_t, parent_end + 2> words{};
  words[0] = do_immediate | (std::uint64_t{2} << count_shift) | parent_end;
  words[short_setup_pc] = do_immediate | (std::uint64_t{1} << count_shift) | 1;
  fixture::Program program(words);
  const auto initial = starting();
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  state.lcntr = 2;
  entered(trace, {.address = counted_address | (base + parent_end), .count = 2});
  for (const bool repeats : {true, false}) {
    trace.boundary(base + 2);
    trace.record();
    // The parent's end is in Fetch1 before the short DO reaches Address.
    parent_count_sample(trace, repeats);
    short_boundary(trace, {.pc = 3, .fetch = repeats ? 1U : 7U, .decode = 5});
    short_boundary(trace, {.pc = 4, .fetch = repeats ? 2U : 8U, .decode = 6});
    enter_nested_short(trace,
                       {.pc = 1, .fetch = 1, .decode = 1, .count = 1},
                       single_word_counted_address | (base + child_end));
    const auto child_slot = state.loop_address_depth - 1U;
    state.loop_counter_stack[child_slot] = 0;
    if (repeats) {
      leave_nested_child(state);
    } else {
      popped(state);
    }
    short_boundary(
        trace, {.pc = parent_end, .fetch = repeats ? 4U : 10U, .decode = repeats ? 2U : 8U}, 5);
    trace.boundary(base + (repeats ? 1U : parent_end + 1));
    trace.record();
  }
  verify_timeline(program, initial, trace);
}

void nested_short_do() {
  constexpr Range outer{.start = base + 1, .end = base + 7};
  constexpr auto inner_word = base + 2;
  constexpr auto outer_do = do_immediate | (std::uint64_t{2} << count_shift) | 7;
  constexpr auto inner_do = do_immediate | (std::uint64_t{3} << count_shift) | 1;
  std::array<std::uint64_t, 9> words{};
  words[0] = outer_do;
  words[1] = inner_do;
  words[2] = fixture::add(0, 0, 1);
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  sharc::Core replay(program.memory());
  auto registers = starting();
  registers.pe[0].data[0][1] = 1U << raw_shift;
  fixture::install(core, native, registers);
  (void)replay.install(native, fixture::Program::variant_id);
  Timeline trace{.registers = registers, .prefixes = {}};
  auto &state = trace.registers;
  state.lcntr = 2;
  entered(trace, {.address = counted_address | outer.end, .count = 2});
  for (const bool repeats : {true, false}) {
    state.lcntr = 3;
    state.loop_address_depth = state.loop_counter_depth = state.pc_stack_pointer = 2;
    state.pc_stack[1] = inner_word;
    state.loop_address_stack[1] = single_word_counted_address | inner_word;
    state.loop_counter_stack[1] = 3;
    trace.boundary(inner_word);
    state.faddr = state.daddr = inner_word;
    trace.record();

    auto &sum = state.pe[0].data[0][0];
    sum += 1U << raw_shift;
    state.loop_counter_stack[1] = 1;
    trace.boundary(inner_word, 2);
    state.faddr = state.daddr = inner_word;
    trace.record();
    sum += 1U << raw_shift;
    state.loop_counter_stack[1] = 0;
    state.loop_address_depth = state.loop_counter_depth = state.pc_stack_pointer = 1;
    trace.boundary(inner_word);
    state.faddr = inner_word + 1;
    state.daddr = inner_word;
    trace.record();
    sum += 1U << raw_shift;
    trace.boundary(inner_word + 1, 4);
    trace.record();

    --state.loop_counter_stack[0];
    if (!repeats) { popped(state); }
    tail(trace, outer, repeats);
  }
  fixture::verify_prefixes(core, replay, trace.prefixes);
}

void counted_call_boundary(Timeline &trace, unsigned pc, bool repeats, unsigned cycles) {
  if (repeats && pc >= 2 && pc <= 5) {
    constexpr std::array decode{4U, 5U, 1U, 2U};
    short_boundary(trace, {.pc = pc, .fetch = pc - 1, .decode = decode[pc - 2]}, cycles);
  } else {
    trace.boundary(base + pc, cycles);
    trace.record();
  }
}

void counted_call_turn(Timeline &trace, unsigned call_word, bool delayed_return, bool repeats) {
  constexpr unsigned subroutine = 8;
  constexpr auto return_word = subroutine + 1;
  auto &state = trace.registers;
  --state.loop_counter_stack[0];
  if (!repeats) { popped(state); }
  counted_call_boundary(trace, 2, repeats, 1);
  for (unsigned pc = 2; pc <= 5; ++pc) {
    if (pc != call_word) {
      const auto next = pc == 5 && repeats ? 1U : pc + 1;
      counted_call_boundary(trace, next, repeats, 1);
      continue;
    }
    // The third-to-last CALL is in Address in the final counter sample cycle.
    const auto tags = call_tag | (!repeats && pc == 2 ? termination_tag : 0);
    // Tables 4-13 and 4-14 show the instruction behind the endpoint CALL.
    const auto return_pc = pc == 5 && repeats ? 1U : pc + 1;
    entered_call(trace, {.target = base + subroutine, .raw_return = tags | (base + return_pc)});
    state.pe[0].data[0][0] += 1U << raw_shift;
    trace.boundary(base + return_word);
    trace.record();
    popped_call(state);
    if (delayed_return) {
      short_boundary(trace,
                     {.pc = return_word + 1, .fetch = return_pc + 1, .decode = return_word + 3});
      state.pe[0].data[0][2] += 1U << raw_shift;
      short_boundary(trace, {.pc = return_word + 2, .fetch = return_pc + 2, .decode = return_pc});
      state.pe[0].data[0][2] += 1U << raw_shift;
    }
    counted_call_boundary(trace, return_pc, repeats, delayed_return ? 2U : 4U);
  }
}

void counted_call_reentry() {
  constexpr auto setup = do_immediate | (std::uint64_t{2} << count_shift) | 5;
  constexpr auto sub = base + 8;

  struct CallSite {
    unsigned word;
    bool dag2{};
  };

  constexpr std::array sites{CallSite{.word = 4},
                             CallSite{.word = 2, .dag2 = true},
                             CallSite{.word = 5},
                             CallSite{.word = 5, .dag2 = true}};
  for (const auto &[call_word, dag2] : sites) {
    std::array<std::uint64_t, 14> words{};
    words[0] = setup;
    words[call_word] = dag2 ? call_dag2 : call_direct | sub;
    words[6] = jump_direct | (base + 13);
    words[8] = fixture::add(0, 0, 1);
    words[9] = rts | lr | (dag2 ? db : 0);
    words[10] = fixture::add(2, 2, 1);
    words[11] = fixture::add(2, 2, 1);
    fixture::Program program(words);
    program.code.variants[0].entries.push_back(sub);
    auto initial = starting();
    initial.dag[0][8].index = sub;
    initial.pe[0].data[0][1] = 1U << raw_shift;
    Timeline trace{.registers = initial, .prefixes = {}};
    trace.registers.lcntr = 2;
    entered(trace, {.address = counted_address | (base + 5), .count = 2});
    counted_call_turn(trace, call_word, dag2, true);
    counted_call_turn(trace, call_word, dag2, false);
    verify_timeline(program, initial, trace);
  }
}

void sample_return_slots(Timeline &trace, unsigned resume) {
  short_boundary(trace, {.pc = 10, .fetch = resume + 1, .decode = 12});
  trace.registers.pe[0].data[0][2] = 7U << raw_shift;
  short_boundary(trace, {.pc = 11, .fetch = resume + 2, .decode = resume});
}

enum class CountedSampleCall : std::uint8_t { plain, direct, delayed, predicated };

void counted_sample_call(CountedSampleCall form) {
  constexpr auto sub = base + 8;
  constexpr std::uint64_t call_dag2_eq = 0x088000000000;
  const bool late_return =
      form == CountedSampleCall::delayed || form == CountedSampleCall::predicated;
  const bool conditional = form == CountedSampleCall::predicated;
  const auto indirect = conditional ? call_dag2_eq : call_dag2;
  std::array<std::uint64_t, 14> words{};
  words[0] = do_immediate | (std::uint64_t{2} << count_shift) | 5;
  words[1] = late_return ? indirect : call_direct | sub;
  words[6] = jump_direct | (base + 13);
  words[8] = fixture::add(0, 0, 1);
  words[9] = rts | (form == CountedSampleCall::plain ? 0 : lr) | (late_return ? db : 0);
  words[10] = fixture::immediate(2, 7);
  fixture::Program program(words);
  program.code.variants[0].entries.push_back(sub);
  auto initial = starting();
  initial.dag[0][8].index = sub;
  initial.pe[0].data[0][1] = 1U << raw_shift;
  initial.pe[0].astat = conditional ? az : 0;
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  state.lcntr = 2;
  entered(trace, {.address = counted_address | (base + 5), .count = 2});
  for (const bool repeats : {true, false}) {
    const bool calls = !conditional || repeats;
    unsigned clocks = 1;
    if (calls) {
      // At E-4 a taken CALL redirects before the pending end fetch. The
      // count stays resident throughout the subroutine and any return slots.
      entered_call(trace, {.target = sub, .raw_return = call_tag | (base + 2)});
      state.pe[0].data[0][0] += 1U << raw_shift;
      state.pe[0].astat = 0;
      trace.boundary(sub + 1);
      trace.record();
      popped_call(state);
      if (late_return) { sample_return_slots(trace, 2); }
      clocks = late_return ? 2U : 4U;
    }
    parent_count_sample(trace, repeats);
    counted_call_boundary(trace, 2, repeats, clocks);
    for (unsigned pc = 3; pc <= 5; ++pc) { counted_call_boundary(trace, pc, repeats, 1); }
    trace.boundary(base + (repeats ? 1U : 6U));
    trace.record();
  }
  verify_timeline(program, initial, trace);
}

void counted_sample_before_second_call() {
  constexpr auto sub = base + 8;
  std::array<std::uint64_t, 10> words{};
  words[0] = do_immediate | (std::uint64_t{1} << count_shift) | 5;
  words[1] = call_direct | sub;
  words[2] = call_direct | sub;
  words[6] = jump_direct | (base + 9);
  words[8] = rts | lr;
  fixture::Program program(words);
  const auto initial = starting();
  Timeline trace{.registers = initial, .prefixes = {}};
  trace.registers.lcntr = 1;
  entered(trace, {.address = counted_address | (base + 5), .count = 1});
  entered_call(trace, {.target = sub, .raw_return = call_tag | (base + 2)});
  popped_call(trace.registers);
  parent_count_sample(trace, false);
  counted_call_boundary(trace, 2, false, 4);
  // The deferred exit leaves the same tag as an end fetch completed in place.
  entered_call(trace, {.target = sub, .raw_return = call_tag | termination_tag | (base + 3)});
  popped_call(trace.registers);
  for (unsigned pc = 3; pc <= 6; ++pc) {
    counted_call_boundary(trace, pc, false, pc == 3 ? 4U : 1U);
  }
  verify_timeline(program, initial, trace);
}

constexpr auto short_call_sub = base + 8;

void counted_not_lce_compute() {
  std::array<std::uint64_t, 7> words{};
  words[0] = do_immediate | (std::uint64_t{2} << count_shift) | 5;
  for (unsigned pc = 2; pc <= 5; ++pc) { words[pc] = if_not_lce(fixture::add(pc, pc, 1)); }
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  trace.registers.lcntr = 2;
  entered(trace, {.address = counted_address | (base + 5), .count = 2});
  for (const bool repeats : {true, false}) {
    parent_count_sample(trace, repeats);
    counted_call_boundary(trace, 2, repeats, 1);
    for (unsigned pc = 2; pc <= 5; ++pc) {
      // The first compute still sees the old condition. The second sees expiry.
      if (repeats || pc == 2) { trace.registers.pe[0].data[0][pc] += 1U << raw_shift; }
      const auto next = pc == 5 && repeats ? 1U : pc + 1;
      counted_call_boundary(trace, next, repeats, 1);
    }
  }
  verify_timeline(program, initial, trace);
}

constexpr Range inner_counted_tail{.start = base + 3, .end = base + 7};
constexpr unsigned nested_tail_call = 5;
constexpr unsigned nested_tail_sub = 14;

enum class NestedTailReturn : std::uint8_t { none, immediate, delayed };

void nested_counted_boundary(
    Timeline &trace, Range loop, std::uint32_t pc, bool repeats, unsigned cycles = 1) {
  trace.boundary(pc == loop.end + 1 && repeats ? loop.start : pc, cycles);
  const auto distance = loop.end - pc;
  if (repeats && distance < 4) { trace.registers.faddr = loop.start + 3 - distance; }
  if (repeats && distance < 2) { trace.registers.daddr = loop.start + 1 - distance; }
  trace.record();
}

void nested_tail_add(Timeline &trace, unsigned pc) {
  trace.registers.pe[0].data[0][pc] += 1U << raw_shift;
}

unsigned nested_tail_call_and_return(Timeline &trace, NestedTailReturn form) {
  constexpr auto resume = nested_tail_call + 1;
  entered_call(trace, {.target = base + nested_tail_sub, .raw_return = call_tag | (base + resume)});
  popped_call(trace.registers);
  if (form == NestedTailReturn::immediate) { return 4; }
  short_boundary(trace,
                 {.pc = nested_tail_sub + 1, .fetch = resume + 1, .decode = nested_tail_sub + 3});
  nested_tail_add(trace, 0);
  short_boundary(trace, {.pc = nested_tail_sub + 2, .fetch = resume + 2, .decode = resume});
  nested_tail_add(trace, 0);
  return 2;
}

void nested_counted_child(Timeline &trace,
                          Range parent,
                          bool parent_repeats,
                          NestedTailReturn form) {
  auto &state = trace.registers;
  constexpr auto child = inner_counted_tail;
  state.loop_address_depth = state.loop_counter_depth = state.pc_stack_pointer = 2;
  state.pc_stack[1] = child.start;
  state.loop_address_stack[1] = counted_address | child.end;
  state.loop_counter_stack[1] = 2;
  trace.boundary(child.start);
  trace.record();

  for (const bool repeats : {true, false}) {
    --state.loop_counter_stack[1];
    if (!repeats) { leave_nested_child(state); }
    nested_counted_boundary(trace, child, child.start + 1, repeats);
    for (auto pc = child.start + 1; pc <= child.end; ++pc) {
      if (!repeats && pc == parent.end - 4) { parent_count_sample(trace, parent_repeats); }
      unsigned cycles = 1;
      if (form != NestedTailReturn::none && pc == base + nested_tail_call) {
        cycles = nested_tail_call_and_return(trace, form);
      } else if (repeats || pc == child.start + 1) {
        nested_tail_add(trace, pc - base);
      }
      nested_counted_boundary(
          trace, repeats ? child : parent, pc + 1, repeats || parent_repeats, cycles);
    }
  }
}

void nested_counted_not_lce(unsigned gap, NestedTailReturn form = NestedTailReturn::none) {
  constexpr auto child = inner_counted_tail;
  constexpr auto two_iterations = do_immediate | (std::uint64_t{2} << count_shift);
  const Range parent{.start = base + 1, .end = child.end + gap};
  const bool call = form != NestedTailReturn::none;
  std::vector<std::uint64_t> words(call ? nested_tail_sub + 3 : parent.end - base + 2);
  words[0] = two_iterations | (parent.end - base);
  words[2] = two_iterations | (child.end - child.start + 1);
  for (auto pc = child.start + 1 - base; pc <= parent.end - base; ++pc) {
    words[pc] = if_not_lce(fixture::add(pc, pc, 1));
  }
  if (call) {
    // Address-stage predicates still see the pre-sample value on this word.
    words[nested_tail_call] = if_not_lce(call_direct | (base + nested_tail_sub));
    words[nested_tail_sub] = rts | lr;
    if (form == NestedTailReturn::delayed) {
      words[nested_tail_sub] |= db;
      words[nested_tail_sub + 1] = fixture::add(0, 0, 1);
      words[nested_tail_sub + 2] = fixture::add(0, 0, 1);
    }
  }
  fixture::Program program(words);
  if (call) { program.code.variants[0].entries.push_back(base + nested_tail_sub); }
  auto initial = starting();
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  trace.registers.lcntr = 2;
  entered(trace, {.address = counted_address | parent.end, .count = 2});
  for (const bool repeats : {true, false}) {
    trace.boundary(child.start - 1);
    trace.record();
    nested_counted_child(trace, parent, repeats, form);
    for (auto pc = child.end + 1; pc <= parent.end; ++pc) {
      if (repeats || pc == parent.end - 3) { nested_tail_add(trace, pc - base); }
      nested_counted_boundary(trace, parent, pc + 1, repeats);
    }
  }
  verify_timeline(program, initial, trace);
}

struct NotLceCall {
  unsigned site;
  bool stall;
};

void not_lce_call_turn(Timeline &trace, NotLceCall call, bool repeats) {
  auto &state = trace.registers;
  state.pe[0].data[0][7] = 2U << raw_shift;
  parent_count_sample(trace, repeats);
  counted_call_boundary(trace, 2, repeats, 1);
  // Both early branch positions see the previous condition without an interlock.
  const bool branches = repeats || (call.site <= 3 && !call.stall);
  const auto tags = call_tag | (!repeats && call.site == 2 ? termination_tag : 0);
  for (unsigned pc = 2; pc <= 5; ++pc) {
    if (call.stall && pc == call.site - 1) { state.dag[0][8].index = short_call_sub; }
    if (pc == call.site + 1 && repeats) { state.pe[0].data[0][6] += 1U << raw_shift; }
    const bool taken = pc == call.site && branches;
    const unsigned interlock = call.stall && pc == call.site ? 2U : 0U;
    const auto next = pc == 5 && repeats ? 1U : pc + 1;
    if (taken) {
      entered_call(
          trace, {.target = short_call_sub, .raw_return = tags | (base + next)}, 4 + interlock);
      popped_call(state);
    }
    counted_call_boundary(trace, next, repeats, taken ? 4U : 1 + interlock);
  }
}

void counted_not_lce_call(unsigned site, bool stall = false) {
  constexpr unsigned index_register = 0x18;
  std::array<std::uint64_t, 9> words{};
  words[0] = do_immediate | (std::uint64_t{2} << count_shift) | 5;
  words[1] = fixture::add(7, 1, 1);
  if (stall) { words[site - 1] = fixture::immediate(index_register, short_call_sub); }
  words[site] = if_not_lce(stall ? call_dag2 : call_direct | short_call_sub);
  words[site + 1] = if_not_lce(fixture::add(6, 6, 1));
  words[8] = rts | lr;
  fixture::Program program(words);
  program.code.variants.front().entries.push_back(short_call_sub);
  auto initial = starting();
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  trace.registers.lcntr = 2;
  entered(trace, {.address = counted_address | (base + 5), .count = 2});
  for (const bool repeats : {true, false}) {
    not_lce_call_turn(trace, {.site = site, .stall = stall}, repeats);
  }
  verify_timeline(program, initial, trace);
}

void refused_not_lce_without_loop() {
  const std::array words{if_not_lce(fixture::add(0, 0, 1))};
  const fixture::Program program(words);
  const auto fault = fixture::refusal(program.code, "NOT LCE without a counted DO was prepared");
  check(fault.reason == sharc::FaultReason::unsupported_instruction && fault.address == base &&
            fault.instruction == words.front(),
        "unqualified NOT LCE fault lost the instruction");
}

void callee_short_loop(Timeline &trace) {
  auto &state = trace.registers;
  const auto depth = state.loop_address_depth;
  const auto pc_slot = state.pc_stack_pointer++;
  state.pc_stack[pc_slot] = short_call_sub + 1;
  state.loop_address_stack[depth] = single_word_counted_address | (short_call_sub + 1);
  state.loop_counter_stack[depth] = 1;
  ++state.loop_address_depth;
  ++state.loop_counter_depth;
  state.lcntr = 1;
  state.pe[0].stky &= ~empty;
  short_boundary(trace, {.pc = 9, .fetch = 9, .decode = 9});
  state.pe[0].data[0][0] += 1U << raw_shift;
  state.pe[0].astat = 0;
  state.loop_counter_stack[depth] = 0;
  --state.loop_address_depth;
  --state.loop_counter_depth;
  if (depth == 0) { state.pe[0].stky |= loop_empty; }
  popped_call(state);
  trace.boundary(short_call_sub + 2, 5);
  trace.record();
}

unsigned short_call_round_trip(Timeline &trace,
                               unsigned resume,
                               bool delayed,
                               std::uint32_t tags = call_tag) {
  entered_call(trace, {.target = short_call_sub, .raw_return = tags | (base + resume)});
  callee_short_loop(trace);
  popped_call(trace.registers);
  if (delayed) {
    short_boundary(trace, {.pc = 11, .fetch = resume + 1, .decode = 13});
    trace.registers.pe[0].data[0][2] = 7U << raw_shift;
    short_boundary(trace, {.pc = 12, .fetch = resume + 2, .decode = resume});
  }
  return delayed ? 2U : 4U;
}

enum class FourWordCall : std::uint8_t { direct, delayed, predicated, full_count };

void four_word_loop_call(FourWordCall form, unsigned site = 3) {
  constexpr std::uint64_t call_eq = 0x068000000000;
  const bool delayed = form == FourWordCall::delayed;
  const bool predicated = form == FourWordCall::predicated;
  const bool full_count = form == FourWordCall::full_count;
  const std::uint32_t count = full_count ? 0 : 2;
  std::array<std::uint64_t, 15> words{};
  words[0] = do_immediate | (std::uint64_t{count} << count_shift) | 4;
  words[site] = delayed ? call_dag2 : (predicated ? call_eq : call_direct) | short_call_sub;
  words[5] = jump_direct | (base + 14);
  words[8] = do_immediate | (std::uint64_t{1} << count_shift) | 1;
  words[9] = fixture::add(0, 0, 1);
  words[10] = rts | lr | (delayed ? db : 0);
  words[11] = fixture::immediate(2, 7);
  fixture::Program program(words);
  program.code.variants.front().entries.push_back(short_call_sub);
  auto initial = starting();
  initial.dag[0][8].index = short_call_sub;
  initial.pe[0].data[0][1] = 1U << raw_shift;
  initial.pe[0].astat = predicated ? az : 0;
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  state.lcntr = count;
  entered(trace, {.address = counted_address | (base + 4), .count = count});
  state.faddr = base + 4;
  state.daddr = base + 2;
  trace.prefixes.back().registers = state;
  --state.loop_counter_stack[0];
  short_boundary(trace, {.pc = 2, .fetch = 2, .decode = 3}, 2);
  for (const bool repeats : {true, false}) {
    const bool take_call = !predicated || repeats;
    const auto third = site == 2 && take_call ? short_call_round_trip(trace, 3, delayed) : 1U;
    short_boundary(trace, {.pc = 3, .fetch = repeats ? 3U : 7U, .decode = 4}, third);
    const auto cycles = site == 3 && take_call ? short_call_round_trip(trace, 4, delayed) : 1U;
    short_boundary(
        trace, {.pc = 4, .fetch = repeats ? 4U : 8U, .decode = repeats ? 1U : 5U}, cycles);
    if (full_count) {
      parent_count_sample(trace, true);
      short_boundary(trace, {.pc = 1, .fetch = 1, .decode = 2});
      break;
    }
    if (repeats) {
      // The last word is still buffered after the final counter check.
      parent_count_sample(trace, false);
      short_boundary(trace, {.pc = 1, .fetch = 5, .decode = 2});
      short_boundary(trace, {.pc = 2, .fetch = 6, .decode = 3});
    } else {
      short_boundary(trace, {.pc = 5, .fetch = 9, .decode = 6});
    }
  }
  verify_timeline(program, initial, trace);
}

void four_word_endpoint_turn(Timeline &trace, FourWordCall form, unsigned turn) {
  const bool final = turn == 2;
  const bool takes_call = form != FourWordCall::predicated || turn == 0;
  short_boundary(trace, {.pc = 2, .fetch = final ? 6U : 2U, .decode = 3}, turn == 0 ? 2U : 1U);
  short_boundary(trace, {.pc = 3, .fetch = final ? 7U : 3U, .decode = 4});
  short_boundary(trace, {.pc = 4, .fetch = final ? 8U : 4U, .decode = final ? 5U : 1U});
  const auto resume = final ? 5U : 1U;
  const auto cycles =
      takes_call ? short_call_round_trip(trace, resume, form == FourWordCall::delayed) : 1U;
  if (final) {
    short_boundary(trace, {.pc = 5, .fetch = 9, .decode = takes_call ? 7U : 6U}, cycles);
    return;
  }
  // The count stays unchanged in the callee and both delayed slots.
  // The repeat's end reaches Fetch1 only as the return admits the header.
  parent_count_sample(trace, turn == 0);
  short_boundary(trace, {.pc = 1, .fetch = turn == 0 ? 1U : 5U, .decode = 2}, cycles);
}

void four_word_endpoint_call(FourWordCall form) {
  constexpr std::uint64_t call_eq = 0x068000000000;
  const bool delayed = form == FourWordCall::delayed;
  const bool predicated = form == FourWordCall::predicated;
  std::array<std::uint64_t, 15> words{};
  words[0] = do_immediate | (std::uint64_t{3} << count_shift) | 4;
  words[4] = delayed ? call_dag2 : (predicated ? call_eq : call_direct) | short_call_sub;
  words[5] = jump_direct | (base + 14);
  words[8] = do_immediate | (std::uint64_t{1} << count_shift) | 1;
  words[9] = fixture::add(0, 0, 1);
  words[10] = rts | lr | (delayed ? db : 0);
  words[11] = fixture::immediate(2, 7);
  fixture::Program program(words);
  program.code.variants.front().entries.push_back(short_call_sub);
  auto initial = starting();
  initial.dag[0][8].index = short_call_sub;
  initial.pe[0].data[0][1] = 1U << raw_shift;
  initial.pe[0].astat = predicated ? az : 0;
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  state.lcntr = 3;
  entered(trace, {.address = counted_address | (base + 4), .count = 3});
  state.faddr = base + 4;
  state.daddr = base + 2;
  trace.prefixes.back().registers = state;
  --state.loop_counter_stack[0];
  for (unsigned turn = 0; turn < 3; ++turn) { four_word_endpoint_turn(trace, form, turn); }
  verify_timeline(program, initial, trace);
}

struct ShortEndpointTurn {
  ShortAddress first, second{}, returned;
};

constexpr ShortEndpointTurn early_three_word_turn{.first = {.pc = 2, .fetch = 3, .decode = 1},
                                                  .second = {.pc = 3, .fetch = 1, .decode = 2},
                                                  .returned = {.pc = 1, .fetch = 2, .decode = 3}};
constexpr std::array three_word_turns{
    early_three_word_turn,
    early_three_word_turn,
    ShortEndpointTurn{.first = {.pc = 2, .fetch = 3, .decode = 1},
                      .second = {.pc = 3, .fetch = 4, .decode = 2},
                      .returned = {.pc = 1, .fetch = 5, .decode = 3}},
    ShortEndpointTurn{.first = {.pc = 2, .fetch = 6, .decode = 4},
                      .second = {.pc = 3, .fetch = 7, .decode = 5},
                      .returned = {.pc = 4, .fetch = 8, .decode = 6}}};

enum class ShortEndpointPredicate : std::uint8_t { unconditional, not_lce };

void counted_short_endpoint_call(
    unsigned width,
    bool delayed,
    ShortEndpointPredicate predicate = ShortEndpointPredicate::unconditional) {
  constexpr std::array two_word{ShortEndpointTurn{.first = {.pc = 2, .fetch = 2, .decode = 2},
                                                  .returned = {.pc = 1, .fetch = 1, .decode = 1}},
                                ShortEndpointTurn{.first = {.pc = 2, .fetch = 2, .decode = 2},
                                                  .returned = {.pc = 1, .fetch = 3, .decode = 1}},
                                ShortEndpointTurn{.first = {.pc = 2, .fetch = 4, .decode = 2},
                                                  .returned = {.pc = 1, .fetch = 5, .decode = 3}},
                                ShortEndpointTurn{.first = {.pc = 2, .fetch = 6, .decode = 4},
                                                  .returned = {.pc = 3, .fetch = 7, .decode = 5}}};
  const auto &turns = width == 2 ? two_word : three_word_turns;
  std::array<std::uint64_t, 15> words{};
  words[0] = do_immediate | (std::uint64_t{4} << count_shift) | width;
  words[width] = delayed ? call_dag2 : call_direct | short_call_sub;
  const bool predicated = predicate == ShortEndpointPredicate::not_lce;
  if (predicated) { words[width] = if_not_lce(words[width]); }
  words[width + 1] = jump_direct | (base + 14);
  words[8] = do_immediate | (std::uint64_t{1} << count_shift) | 1;
  words[9] = fixture::add(0, 0, 1);
  words[10] = rts | lr | (delayed ? db : 0);
  words[11] = fixture::immediate(2, 7);
  fixture::Program program(words);
  program.code.variants.front().entries.push_back(short_call_sub);
  auto initial = starting();
  initial.dag[0][8].index = short_call_sub;
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  state.lcntr = 4;
  entered(trace, {.address = short_counted_types[width - 1] | (base + width), .count = 4});
  state.faddr = base + 2;
  state.daddr = base + width;
  trace.prefixes.back().registers = state;
  --state.loop_counter_stack[0];
  for (std::size_t i = 0; i < turns.size(); ++i) {
    const auto &turn = turns[i];
    const auto first_cycles = width == 2 && i == 0 ? 2U : 1U;
    short_boundary(trace, turn.first, first_cycles);
    if (width == 3) {
      // This fetch has happened by the time E transfers to the callee.
      if (i < 2) { parent_count_sample(trace, i == 0); }
      short_boundary(trace, turn.second);
    }
    const auto tags = call_tag | (width == 3 && i == 1 ? termination_tag : 0);
    const auto taken_through = width == 2 ? 2U : 1U;
    const auto cycles = !predicated || i <= taken_through
                            ? short_call_round_trip(trace, turn.returned.pc, delayed, tags)
                            : 1U;
    // In a two-word body the fourth return fetch reaches E after the freeze.
    if (width == 2 && i < 2) { parent_count_sample(trace, i == 0); }
    short_boundary(trace, turn.returned, cycles);
  }
  verify_timeline(program, initial, trace);
}

enum class ThreeWordCall : std::uint8_t { direct, delayed, predicated };

void three_word_penultimate_call(ThreeWordCall form, bool observe_after_return = false) {
  constexpr std::uint64_t call_eq = 0x068000000000;
  const bool delayed = form == ThreeWordCall::delayed;
  const bool predicated = form == ThreeWordCall::predicated;
  std::array<std::uint64_t, 15> words{};
  words[0] = do_immediate | (std::uint64_t{4} << count_shift) | 3;
  words[2] = delayed ? call_dag2 : (predicated ? call_eq : call_direct) | short_call_sub;
  if (observe_after_return) { words[3] = if_not_lce(fixture::add(6, 6, 1)); }
  words[4] = jump_direct | (base + 14);
  words[8] = do_immediate | (std::uint64_t{1} << count_shift) | 1;
  words[9] = fixture::add(0, 0, 1);
  words[10] = rts | lr | (delayed ? db : 0);
  words[11] = fixture::immediate(2, 7);
  fixture::Program program(words);
  program.code.variants.front().entries.push_back(short_call_sub);
  auto initial = starting();
  initial.dag[0][8].index = short_call_sub;
  initial.pe[0].data[0][1] = 1U << raw_shift;
  initial.pe[0].astat = predicated ? az : 0;
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  state.lcntr = 4;
  entered(trace, {.address = short_counted_types[2] | (base + 3), .count = 4});
  state.faddr = base + 2;
  state.daddr = base + 3;
  trace.prefixes.back().registers = state;
  --state.loop_counter_stack[0];
  for (std::size_t i = 0; i < three_word_turns.size(); ++i) {
    const auto &turn = three_word_turns[i];
    short_boundary(trace, turn.first);
    const auto cycles = !predicated || i == 0 ? short_call_round_trip(trace, 3, delayed) : 1U;
    // E reaches Fetch1 after the return's frozen clocks, or in place on a false CALL.
    if (i < 2) { parent_count_sample(trace, i == 0); }
    short_boundary(trace, turn.second, cycles);
    if (observe_after_return && i < 2) { state.pe[0].data[0][6] += 1U << raw_shift; }
    short_boundary(trace, turn.returned);
  }
  verify_timeline(program, initial, trace);
}

void compact_three_word_penultimate_call(bool chain) {
  std::array<std::uint64_t, 11> words{};
  words[0] = do_immediate | (std::uint64_t{1} << count_shift) | 3;
  words[2] = call_direct | short_call_sub;
  if (chain) { words[3] = words[2]; }
  words[8] = do_immediate | (std::uint64_t{1} << count_shift) | 1;
  words[9] = fixture::add(0, 0, 1);
  words[10] = rts | lr;
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  trace.registers.lcntr = 1;
  entered(trace, {.address = short_counted_types[2] | (base + 3), .count = 1});
  trace.registers.faddr = base + 2;
  trace.registers.daddr = base + 3;
  trace.prefixes.back().registers = trace.registers;
  short_boundary(trace, {.pc = 2, .fetch = 3, .decode = 3});
  const auto cycles = short_call_round_trip(trace, 3, false);
  parent_count_sample(trace, false);
  short_boundary(trace, {.pc = 3, .fetch = 4, .decode = 3}, cycles);
  if (chain) {
    const auto exit_cycles = short_call_round_trip(trace, 4, false, call_tag | termination_tag);
    short_boundary(trace, {.pc = 4, .fetch = 8, .decode = 6}, exit_cycles);
  } else {
    trace.boundary(base + 4, 4);
    trace.record();
  }
  verify_timeline(program, initial, trace);
}

void single_iteration_two_word_call() {
  constexpr std::uint64_t call_eq = 0x068000000000;
  constexpr auto call = call_eq | short_call_sub;
  std::array<std::uint64_t, 9> words{};
  words[0] = do_immediate | (std::uint64_t{1} << count_shift) | 2;
  words[2] = call;
  words[8] = rts | lr;
  fixture::Program program(words);
  auto initial = starting();
  Timeline trace{.registers = initial, .prefixes = {}};
  trace.registers.lcntr = 1;
  entered(trace, {.address = short_counted_types[1] | (base + 2), .count = 1});
  trace.registers.faddr = base + 2;
  trace.registers.daddr = base + 2;
  trace.prefixes.back().registers = trace.registers;
  parent_count_sample(trace, false);
  short_boundary(trace, {.pc = 2, .fetch = 4, .decode = 2}, 2);
  const auto before = trace.registers;
  trace.boundary(base + 3, 3);
  trace.record();
  verify_timeline(program, initial, trace);

  initial.pe[0].astat = az;
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  fixture::install(core, native, initial);
  check(fixture::completed(core.run(2), 2, before.emuclk),
        "one-iteration short DO failed to enter");
  auto at_call = before;
  at_call.pe[0].astat = az;
  const auto fault = core.run(1);
  check(fault.has_fault && fault.fault.reason == sharc::FaultReason::unsupported_feature &&
            fault.fault.address == base + 2 && fault.fault.instruction == call &&
            fault.progress == sharc::Progress{} && core.registers() == at_call,
        "taken CALL in one-iteration two-word DO failed to preserve the boundary");
}

void single_iteration_three_word_call(bool interlock) {
  constexpr std::uint64_t call_eq = 0x068000000000;
  std::array<std::uint64_t, 9> words{};
  words[0] = do_immediate | (std::uint64_t{1} << count_shift) | 3;
  if (interlock) { words[2] = fixture::add(0, 0, 0); }
  words[3] = (interlock ? call_eq : call_direct) | short_call_sub;
  words[8] = rts | lr;
  fixture::Program program(words);
  const auto initial = starting();
  Timeline trace{.registers = initial, .prefixes = {}};
  trace.registers.lcntr = 1;
  entered(trace, {.address = short_counted_types[2] | (base + 3), .count = 1});
  trace.registers.faddr = base + 2;
  trace.registers.daddr = base + 3;
  trace.prefixes.back().registers = trace.registers;
  short_boundary(trace, {.pc = 2, .fetch = 3, .decode = 3});
  parent_count_sample(trace, false);
  if (interlock) { trace.registers.pe[0].astat = az; }
  short_boundary(trace, {.pc = 3, .fetch = 4, .decode = 3});
  // The taken CALL replaces the empty clocks of the compact loop's exit.
  const auto tags = call_tag | (interlock ? 0 : termination_tag);
  entered_call(
      trace, {.target = short_call_sub, .raw_return = tags | (base + 4)}, interlock ? 5U : 4U);
  popped_call(trace.registers);
  short_boundary(trace, {.pc = 4, .fetch = 8, .decode = 6}, 4);
  verify_timeline(program, initial, trace);
}

void late_address_long_loop_call() {
  constexpr std::uint64_t call_eq = 0x068000000000;
  std::array<std::uint64_t, 9> words{};
  words[0] = do_immediate | (std::uint64_t{1} << count_shift) | 5;
  words[1] = fixture::add(0, 0, 0);
  words[2] = call_eq | short_call_sub;
  words[8] = rts | lr;
  fixture::Program program(words);
  const auto initial = starting();
  Timeline trace{.registers = initial, .prefixes = {}};
  trace.registers.lcntr = 1;
  entered(trace, {.address = counted_address | (base + 5), .count = 1});
  parent_count_sample(trace, false);
  trace.registers.pe[0].astat = az;
  counted_call_boundary(trace, 2, false, 1);
  entered_call(trace, {.target = short_call_sub, .raw_return = call_tag | (base + 3)}, 5);
  popped_call(trace.registers);
  counted_call_boundary(trace, 3, false, 4);
  for (unsigned pc = 4; pc <= 6; ++pc) { counted_call_boundary(trace, pc, false, 1); }
  verify_timeline(program, initial, trace);
}

void refused_plain_short_loop_return() {
  std::array<std::uint64_t, 9> words{};
  words[0] = do_immediate | 4; // Zero encodes the full 32-bit count.
  words[3] = call_direct | short_call_sub;
  words[8] = rts;
  fixture::Program program(words);
  // An unrelated long-loop CALL makes plain RTS valid for its deferred fetch.
  // Its packed kind must remain distinct from the high bits of the short count.
  constexpr auto separate = base + 32;
  std::array<std::uint64_t, 7> long_loop{};
  long_loop[0] = do_immediate | (std::uint64_t{1} << count_shift) | 5;
  long_loop[1] = call_direct | short_call_sub;
  program.add_span(separate, long_loop);
  program.code.variants.front().entries.push_back(separate);

  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  fixture::install(core, native, starting());
  check(fixture::completed(core.run(4), 4, 8) && core.registers().pc == short_call_sub,
        "full-count short loop failed to reach an ordinary RTS");
  const auto before = core.registers();
  const auto fault = core.run(1);
  check(fault.has_fault && fault.fault.reason == sharc::FaultReason::unsupported_feature &&
            fault.fault.address == short_call_sub && fault.fault.instruction == rts &&
            fault.fault.detail.contains("RTS (LR)") && fault.progress == sharc::Progress{} &&
            core.registers() == before,
        "ordinary RTS consumed a buffered short loop");
}

void refused_overlapping_short_call() {
  constexpr auto count_two = do_immediate | (std::uint64_t{2} << count_shift);
  constexpr auto call = call_direct | (base + 10);
  std::array<std::uint64_t, 11> words{};
  words[0] = count_two | 9;
  words[1] = count_two | 4;
  words[3] = call;
  words[10] = rts | lr;
  const fixture::Program program(words);
  const auto fault = fixture::refusal(program.code, "overlapping short CALL was prepared");
  check(fault.reason == sharc::FaultReason::unsupported_instruction && fault.address == base + 3 &&
            fault.instruction == call,
        "overlapping short CALL fault lost instruction attribution");
}

enum class ArithmeticSampleCall : std::uint8_t {
  immediate,
  delayed,
  repeated,
  skipped,
  split_simd
};

void arithmetic_sample_call(ArithmeticSampleCall form) {
  constexpr auto sub = base + 8;
  constexpr std::uint64_t call_ne = 0x06a000000000;
  constexpr auto simd = 1U << 21;
  const bool skipped = form == ArithmeticSampleCall::skipped;
  const bool late_return = form == ArithmeticSampleCall::delayed;
  const bool split = form == ArithmeticSampleCall::split_simd;
  const bool repeats = form == ArithmeticSampleCall::repeated || split;
  const bool sub_sets_zero = split || (!late_return && !repeats);
  std::array<std::uint64_t, 14> words{};
  words[0] = do_eq | 5;
  words[3] = (skipped ? call_ne : call_direct) | sub;
  if (split) { words[3] = call_dag2; }
  words[6] = jump_direct | (base + 13);
  words[8] = sub_sets_zero ? fixture::add(0, 3, 3) : fixture::add(0, 1, 1);
  words[9] = rts | lr | (late_return ? db : 0);
  words[10] = fixture::immediate(2, 7);
  words[11] = fixture::add(0, 3, 3);
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].astat = skipped || repeats ? az : 0;
  initial.pe[0].data[0][1] = 1U << raw_shift;
  if (split) {
    program.code.variants[0].entries.push_back(sub);
    program.code.variants[0].entries.push_back(base + 4);
    initial.mode1 = simd;
    initial.dag[0][8].index = sub;
    initial.pe[1].data[0][3] = 1U << raw_shift;
  }
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  entered(trace, {.address = base + 5, .count = old_lcntr});
  short_boundary(trace, {.pc = 2, .fetch = 1, .decode = 4});
  short_boundary(trace, {.pc = 3, .fetch = 2, .decode = 5});
  unsigned clocks = 1;
  if (!skipped) {
    entered_call(trace, {.target = sub, .raw_return = call_tag | (base + 4)});
    state.pe[0].data[0][0] = sub_sets_zero ? 0 : 2U << raw_shift;
    state.pe[0].astat = sub_sets_zero ? az : 0;
    state.pe[1].data[0][0] = initial.pe[1].data[0][3] * 2U;
    trace.boundary(sub + 1);
    trace.record();
    popped_call(state);
    if (late_return) {
      sample_return_slots(trace, 4);
      state.pe[0].data[0][0] = 0;
      state.pe[0].astat = az;
    }
    clocks = late_return ? 2U : 4U;
  }
  // The restored fetch reaches the second loop word using ASTAT from
  // the callee or the second return slot. A false CALL samples in place.
  if (!repeats) { popped(state); }
  short_boundary(trace, {.pc = 4, .fetch = repeats ? 3U : 6U, .decode = 1}, clocks);
  short_boundary(trace, {.pc = 5, .fetch = repeats ? 4U : 7U, .decode = 2});
  trace.boundary(base + (repeats ? 1U : 6U), repeats ? 1U : 3U);
  trace.record();
  verify_timeline(program, initial, trace);
}

void arithmetic_fetch_call() {
  constexpr auto sub = base + 8;
  std::array<std::uint64_t, 11> words{};
  words[0] = do_eq | 5;
  words[1] = call_direct | sub;
  words[6] = jump_direct | (base + 10);
  words[8] = fixture::add(0, 0, 0);
  words[9] = rts | lr;
  fixture::Program program(words);
  const auto initial = starting();
  Timeline trace{.registers = initial, .prefixes = {}};
  entered(trace, {.address = base + 5, .count = old_lcntr});
  entered_call(trace, {.target = sub, .raw_return = call_tag | (base + 2)});
  trace.registers.pe[0].astat = az;
  trace.boundary(sub + 1);
  trace.record();
  popped_call(trace.registers);
  // Returning from E-4 first selects the speculative backedge. The
  // arithmetic condition is sampled later, at the ordinary E-2 phase.
  short_boundary(trace, {.pc = 2, .fetch = 1, .decode = 4}, 4);
  short_boundary(trace, {.pc = 3, .fetch = 2, .decode = 5});
  popped(trace.registers);
  short_boundary(trace, {.pc = 4, .fetch = 6, .decode = 1});
  short_boundary(trace, {.pc = 5, .fetch = 7, .decode = 2});
  trace.boundary(base + 6, 3);
  trace.record();
  verify_timeline(program, initial, trace);
}

void conditional_endpoint_call(bool call_if_nonzero) {
  constexpr auto start = base + 1;
  constexpr auto end = base + 5;
  constexpr auto sub = base + 8;
  constexpr std::uint64_t call_eq = 0x068000000000;
  constexpr std::uint64_t call_ne = 0x06a000000000;
  constexpr auto minus_one = std::numeric_limits<std::uint32_t>::max();
  std::array<std::uint64_t, 12> words{};
  words[0] = do_eq | 5;
  words[1] = fixture::add(0, 0, 1);
  words[5] = (call_if_nonzero ? call_ne : call_eq) | sub;
  words[6] = jump_direct | (base + 11);
  words[9] = rts | lr;
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].data[0][0] = 2U << raw_shift;
  initial.pe[0].data[0][1] = std::uint64_t{minus_one} << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  entered(trace, {.address = end, .count = old_lcntr});
  for (const bool repeats : {true, false}) {
    auto &lane = state.pe[0];
    lane.data[0][0] = repeats ? 1U << raw_shift : 0;
    lane.astat = repeats ? ac : ac | az;
    short_boundary(trace, {.pc = 2, .fetch = 1, .decode = 4});
    short_boundary(trace, {.pc = 3, .fetch = 2, .decode = 5});
    if (!repeats) { popped(state); }
    short_boundary(trace, {.pc = 4, .fetch = repeats ? 3U : 6U, .decode = 1});
    short_boundary(trace, {.pc = 5, .fetch = repeats ? 4U : 7U, .decode = 2});
    const auto target = repeats ? start : end + 1;
    if (repeats == call_if_nonzero) {
      entered_call(trace, {.target = sub, .raw_return = call_tag | target});
      trace.boundary(sub + 1);
      trace.record();
      popped_call(state);
      trace.boundary(target, 4);
    } else {
      // A false CALL leaves the ordinary loop backedge or two-clock refill.
      trace.boundary(target, repeats ? 1U : 3U);
    }
    trace.record();
  }
  verify_timeline(program, initial, trace);
}

void arithmetic_call_reentry() {
  constexpr auto sub = base + 8;
  constexpr auto ret = base + 13;
  constexpr auto end = base + 6;
  constexpr auto inner_end = base + 12;
  std::array<std::uint64_t, 16> words{};
  words[0] = do_eq | 6;
  words[5] = call_direct | sub;
  words[7] = jump_direct | (base + 15);
  words[8] = do_eq | 4;
  words[13] = rts | lr;
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].astat = az;
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  entered(trace, {.address = end, .count = old_lcntr});
  short_boundary(trace, {.pc = 2, .fetch = 6, .decode = 4});
  short_boundary(trace, {.pc = 3, .fetch = 1, .decode = 5});
  short_boundary(trace, {.pc = 4, .fetch = 2, .decode = 6});
  popped(state);
  short_boundary(trace, {.pc = 5, .fetch = 7, .decode = 1});
  entered_call(trace, {.target = sub, .raw_return = call_tag | termination_tag | end});

  // A second arithmetic DO can use its own refill while the caller's
  // terminating tail is suspended by the call.
  state.pc_stack[1] = sub + 1;
  state.pc_stack_pointer = 2;
  state.loop_address_depth = state.loop_counter_depth = 1;
  state.loop_address_stack[0] = inner_end;
  state.loop_counter_stack[0] = old_lcntr;
  state.pe[0].stky &= ~loop_empty;
  short_boundary(trace, {.pc = 9, .fetch = 9, .decode = 11});
  short_boundary(trace, {.pc = 10, .fetch = 10, .decode = 12});
  state.pc_stack_pointer = 1;
  state.loop_address_depth = state.loop_counter_depth = 0;
  state.pe[0].stky |= loop_empty;
  short_boundary(trace, {.pc = 11, .fetch = 13, .decode = 9});
  short_boundary(trace, {.pc = 12, .fetch = 14, .decode = 10});
  trace.boundary(ret, 3);
  trace.record();
  const auto before_return = trace.prefixes.back();
  const auto return_step = trace.prefixes.size();
  popped_call(state);
  short_boundary(trace, {.pc = 6, .fetch = 8, .decode = 2}, 4);
  short_boundary(trace, {.pc = 7, .fetch = 11, .decode = 9}, 3);
  verify_timeline(program, initial, trace);

  words[13] = rts;
  fixture::Program plain(words);
  const auto native = sharc::Core::compile(plain.code);
  sharc::Core core(plain.memory());
  fixture::install(core, native, initial);
  check(fixture::completed(core.run(return_step), return_step, before_return.cycles) &&
            core.registers() == before_return.registers,
        "call lost its loop continuation before an ordinary RTS");
  const auto fault = core.run(1);
  check(fault.has_fault && fault.fault.reason == sharc::FaultReason::unsupported_feature &&
            fault.fault.address == ret && fault.fault.instruction == rts &&
            fault.fault.detail.contains("RTS (LR)") && fault.progress == sharc::Progress{} &&
            core.registers() == before_return.registers,
        "an ordinary RTS consumed a suspended hardware loop");
}

void predicated_loop_abort() {
  constexpr auto setup = do_immediate | (std::uint64_t{2} << count_shift) | 6;
  constexpr auto target = base + 8;
  constexpr std::uint64_t jump_eq = 0x060000000000;
  std::array<std::uint64_t, 9> words{};
  words[0] = setup;
  words[2] = jump_eq | la | target;
  words[5] = fixture::add(0, 3, 3);
  fixture::Program program(words);
  const auto initial = starting();
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  state.lcntr = 2;
  entered(trace, {.address = counted_address | (base + 6), .count = 2});
  short_boundary(trace, {.pc = 2, .fetch = 6, .decode = 4});
  // With the predicate false, Execute still retires the Fetch1 count sample.
  state.loop_counter_stack[0] = 1;
  short_boundary(trace, {.pc = 3, .fetch = 1, .decode = 5});
  short_boundary(trace, {.pc = 4, .fetch = 2, .decode = 6});
  short_boundary(trace, {.pc = 5, .fetch = 3, .decode = 1});
  state.pe[0].astat = az;
  short_boundary(trace, {.pc = 6, .fetch = 4, .decode = 2});
  short_boundary(trace, {.pc = 1, .fetch = 5, .decode = 3});
  short_boundary(trace, {.pc = 2, .fetch = 6, .decode = 4});
  // The taken jump discards this fetch and pops the live loop.
  popped(state);
  trace.boundary(target, 4);
  trace.record();
  verify_timeline(program, initial, trace);
}

void arithmetic_loop_abort() {
  constexpr std::uint64_t do_ne = 0x0e2000000000;
  constexpr std::uint64_t jump_eq = 0x060000000000;
  constexpr auto target = base + 8;
  constexpr std::uint32_t ne_laddr = 0x10000000;
  std::array<std::uint64_t, 9> words{};
  words[0] = do_ne | 5;
  words[3] = jump_eq | la | target;
  fixture::Program program(words);
  for (const bool taken : {true, false}) {
    auto initial = starting();
    initial.pe[0].astat = taken ? az : 0;
    Timeline trace{.registers = initial, .prefixes = {}};
    entered(trace, {.address = ne_laddr | (base + 5), .count = old_lcntr});
    short_boundary(trace, {.pc = 2, .fetch = 1, .decode = 4});
    short_boundary(trace, {.pc = 3, .fetch = 2, .decode = 5});
    popped(trace.registers);
    if (taken) {
      trace.boundary(target, 4);
      trace.record();
    } else {
      // The false EQ branch leaves the outstanding NE loop test to retire.
      short_boundary(trace, {.pc = 4, .fetch = 6, .decode = 1});
      short_boundary(trace, {.pc = 5, .fetch = 7, .decode = 2});
      trace.boundary(base + 6, 3);
      trace.record();
    }
    verify_timeline(program, initial, trace);
  }
}

void check_loop_abort_fault(sharc::Core &core, std::uint64_t instruction) {
  const auto before = core.registers();
  const auto result = core.run(1);
  check(result.has_fault && result.fault.reason == sharc::FaultReason::unsupported_feature &&
            result.fault.address == before.pc && result.fault.instruction == instruction &&
            result.fault.detail.contains("JUMP (LA)") && result.progress == sharc::Progress{} &&
            core.registers() == before,
        "disallowed LA target or stack changed architectural state");
}

void nested_loop_abort() {
  constexpr Range outer{.start = base + 1, .end = base + 14};
  constexpr auto inner_start = base + 3;
  constexpr auto inner_end = base + 7;
  constexpr auto abort_word = 5U;
  constexpr auto setup = do_immediate | (std::uint64_t{2} << count_shift) | 14;
  constexpr auto inner_setup = do_immediate | (std::uint64_t{1} << count_shift) | 5;
  std::array<std::uint64_t, 16> words{};
  words[0] = setup;
  words[2] = inner_setup;
  words[abort_word] = jump_direct | la | db | (base + 10);
  words[6] = fixture::add(0, 0, 1);
  words[7] = fixture::add(2, 2, 1);
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  auto &state = trace.registers;
  state.lcntr = 2;
  entered(trace, {.address = counted_address | outer.end, .count = 2});
  for (const bool repeats : {true, false}) {
    trace.boundary(base + 2);
    trace.record();
    state.lcntr = 1;
    state.loop_address_depth = state.loop_counter_depth = state.pc_stack_pointer = 2;
    state.pc_stack[1] = inner_start;
    state.loop_address_stack[1] = counted_address | inner_end;
    state.loop_counter_stack[1] = 1;
    trace.boundary(inner_start);
    trace.record();
    state.loop_counter_stack[1] = 0;
    state.loop_address_depth = state.loop_counter_depth = state.pc_stack_pointer = 1;
    trace.boundary(inner_start + 1);
    trace.record();
    trace.boundary(base + abort_word);
    trace.record();

    // The child already popped at Fetch1. LA leaves the parent's frame
    // intact and the two delayed slots still retire in order.
    short_boundary(trace, {.pc = 6, .fetch = 11, .decode = 8});
    state.pe[0].data[0][0] += 1U << raw_shift;
    short_boundary(trace, {.pc = 7, .fetch = 12, .decode = 10});
    state.pe[0].data[0][2] += 1U << raw_shift;
    trace.boundary(base + 10, 2);
    trace.record();
    --state.loop_counter_stack[0];
    if (!repeats) { popped(state); }
    tail(trace, outer, repeats);
  }
  verify_timeline(program, initial, trace);

  constexpr auto escape_both = jump_direct | la | db | (base + 15);
  constexpr auto to_arithmetic_end = jump_direct | la | db | outer.end;
  for (const auto invalid_jump : {escape_both, to_arithmetic_end}) {
    words[0] = invalid_jump == escape_both ? setup : do_eq | 14;
    words[abort_word] = invalid_jump;
    fixture::Program invalid(words);
    const auto native = sharc::Core::compile(invalid.code);
    sharc::Core core(invalid.memory());
    fixture::install(core, native, initial);
    constexpr auto before_abort = 5U;
    check(fixture::completed(core.run(before_abort), before_abort, before_abort),
          "nested branch fault fixture did not reach LA");
    check_loop_abort_fault(core, invalid_jump);
  }
}

void computed_loop_abort() {
  constexpr auto setup = do_immediate | (std::uint64_t{2} << count_shift) | 5;
  constexpr std::uint64_t jump_dag2 = 0x083e00000000;
  constexpr std::uint64_t relative = 1ULL << 40;
  constexpr unsigned displacement_shift = 27;
  constexpr auto target = base + 8;
  constexpr auto dag_jump = jump_dag2 | la | (fixture::add(0, 1, 1) & 0x7fffff);
  constexpr auto rel_jump = dag_jump | relative | (std::uint64_t{7} << displacement_shift);
  for (const auto jump : {dag_jump, rel_jump}) {
    std::array<std::uint64_t, 9> words{};
    words[0] = setup;
    words[1] = jump;
    fixture::Program program(words);
    program.code.variants[0].entries.push_back(target);
    auto initial = starting();
    initial.pe[0].data[0][1] = 1U << raw_shift;
    initial.dag[0][8].index = target - 1;
    initial.dag[0][8].modifier = 1;
    Timeline trace{.registers = initial, .prefixes = {}};
    trace.registers.lcntr = 2;
    entered(trace, {.address = counted_address | (base + 5), .count = 2});
    popped(trace.registers);
    trace.registers.pe[0].data[0][0] = 2U << raw_shift;
    trace.boundary(target, 4);
    trace.record();
    verify_timeline(program, initial, trace);
    if (jump == rel_jump) { continue; }

    const auto native = sharc::Core::compile(program.code);
    sharc::Core core(program.memory());
    initial.dag[0][8].index = base + 1;
    fixture::install(core, native, initial);
    check(fixture::completed(core.run(1), 1, 1), "indirect branch fault fixture did not enter DO");
    check_loop_abort_fault(core, jump);
  }
}

void refused_loop_abort() {
  constexpr auto setup = do_immediate | (std::uint64_t{2} << count_shift) | 5;
  constexpr auto inside = jump_direct | la | (base + 3);
  std::array<std::uint64_t, 8> words{};
  words[0] = setup;
  words[1] = inside;
  const fixture::Program bad_target(words);
  const auto target_fault = fixture::refusal(bad_target.code, "LA can target its own loop");
  check(target_fault.reason == sharc::FaultReason::invalid_encoding &&
            target_fault.address == base + 1 && target_fault.instruction == inside,
        "invalid LA target lacked instruction attribution");

  constexpr auto call_with_abort = call_direct | la | (base + 7);
  words[1] = call_with_abort;
  const fixture::Program bad_modifier(words);
  const auto modifier_fault = fixture::refusal(bad_modifier.code, "CALL can use LA");
  check(modifier_fault.reason == sharc::FaultReason::invalid_encoding &&
            modifier_fault.address == base + 1 && modifier_fault.instruction == call_with_abort,
        "invalid CALL modifier lacked instruction attribution");
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
  single_word_loops();
  multiword_short_loops();
  single_word_arithmetic();
  single_word_simd_condition();
  two_word_arithmetic();
  three_word_arithmetic();
  four_word_arithmetic();
  short_forever_do();
  arithmetic_do();
  forever_do();
  nested_do();
  counted_arithmetic_overlap();
  arithmetic_refill_overlap(false);
  arithmetic_refill_overlap(true);
  compact_counted_short_overlap();
  steady_counted_short_overlap();
  short_counted_parent_arithmetic(false);
  short_counted_parent_arithmetic(true);
  short_arithmetic_child_overlap();
  short_arithmetic_refill_not_lce();
  short_child_after_parent_fetch();
  nested_short_do();
  counted_call_reentry();
  counted_sample_call(CountedSampleCall::plain);
  counted_sample_call(CountedSampleCall::direct);
  counted_sample_call(CountedSampleCall::delayed);
  counted_sample_call(CountedSampleCall::predicated);
  counted_sample_before_second_call();
  counted_not_lce_compute();
  for (const auto gap : {1U, 2U, 3U, 4U}) { nested_counted_not_lce(gap); }
  nested_counted_not_lce(4, NestedTailReturn::immediate);
  nested_counted_not_lce(4, NestedTailReturn::delayed);
  counted_not_lce_call(2);
  counted_not_lce_call(3);
  counted_not_lce_call(4);
  counted_not_lce_call(3, true);
  refused_not_lce_without_loop();
  four_word_loop_call(FourWordCall::direct);
  four_word_loop_call(FourWordCall::delayed);
  four_word_loop_call(FourWordCall::predicated);
  four_word_loop_call(FourWordCall::full_count);
  four_word_loop_call(FourWordCall::delayed, 2);
  four_word_endpoint_call(FourWordCall::direct);
  four_word_endpoint_call(FourWordCall::delayed);
  four_word_endpoint_call(FourWordCall::predicated);
  counted_short_endpoint_call(2, false);
  counted_short_endpoint_call(2, true);
  counted_short_endpoint_call(3, false);
  counted_short_endpoint_call(3, true);
  counted_short_endpoint_call(2, true, ShortEndpointPredicate::not_lce);
  counted_short_endpoint_call(3, false, ShortEndpointPredicate::not_lce);
  three_word_penultimate_call(ThreeWordCall::direct);
  three_word_penultimate_call(ThreeWordCall::delayed);
  three_word_penultimate_call(ThreeWordCall::predicated);
  three_word_penultimate_call(ThreeWordCall::delayed, true);
  compact_three_word_penultimate_call(false);
  compact_three_word_penultimate_call(true);
  single_iteration_two_word_call();
  single_iteration_three_word_call(false);
  single_iteration_three_word_call(true);
  late_address_long_loop_call();
  refused_plain_short_loop_return();
  refused_overlapping_short_call();
  arithmetic_sample_call(ArithmeticSampleCall::immediate);
  arithmetic_sample_call(ArithmeticSampleCall::delayed);
  arithmetic_sample_call(ArithmeticSampleCall::repeated);
  arithmetic_sample_call(ArithmeticSampleCall::skipped);
  arithmetic_sample_call(ArithmeticSampleCall::split_simd);
  arithmetic_fetch_call();
  conditional_endpoint_call(false);
  conditional_endpoint_call(true);
  arithmetic_call_reentry();
  predicated_loop_abort();
  arithmetic_loop_abort();
  nested_loop_abort();
  computed_loop_abort();
  refused_loop_abort();
  refused_do();
  skipped_fetch_boundary();
}
