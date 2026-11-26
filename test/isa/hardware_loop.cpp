// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "../support/prefix.hpp"

#include <array>
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

struct ShortBoundary {
  unsigned pc, fetch, decode;
  unsigned cycles{1};
  std::uint32_t count;
  bool frame_live{};
};

void short_counted_case(unsigned length,
                        std::uint32_t count,
                        std::span<const ShortBoundary> boundaries,
                        std::uint64_t successor = 0) {
  constexpr std::array address_types{
      single_word_counted_address, 0x8f000000U, 0xcf000000U, counted_address};
  constexpr std::array setup_fetch{1U, 2U, 2U, 4U};
  constexpr std::array setup_decode{1U, 2U, 3U, 2U};
  std::vector<std::uint64_t> words(length + 2, fixture::add(0, 0, 1));
  words.front() = do_immediate | (std::uint64_t{count} << count_shift) | length;
  words.back() = successor;
  fixture::Program program(words);
  auto initial = starting();
  initial.pe[0].data[0][1] = 1U << raw_shift;
  Timeline trace{.registers = initial, .prefixes = {}};
  trace.registers.lcntr = count;
  entered(trace, {.address = address_types[length - 1] | (base + length), .count = count});
  trace.registers.faddr = base + setup_fetch[length - 1];
  trace.registers.daddr = base + setup_decode[length - 1];
  trace.prefixes.back().registers = trace.registers;
  for (const auto &expected : boundaries) {
    if (trace.registers.pc != base + length + 1) {
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

struct CallTransfer {
  std::uint32_t target, raw_return;
};

void entered_call(Timeline &trace, CallTransfer call) {
  auto &state = trace.registers;
  state.pc_stack[state.pc_stack_pointer++] = call.raw_return;
  state.pe[0].stky &= ~pc_empty;
  trace.boundary(call.target, 4);
  trace.record();
}

void popped_call(sharc::Registers &state) {
  if (--state.pc_stack_pointer == 0) { state.pe[0].stky |= pc_empty; }
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
  nested_short_do();
  counted_call_reentry();
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
