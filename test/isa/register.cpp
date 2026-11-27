// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <vector>

namespace {
using fixture::check;
constexpr auto base = fixture::code_address;
constexpr unsigned source_shift = 38, source_low_shift = 31, destination_shift = 23;
constexpr unsigned condition_shift = 33, simd_bit = 21;
constexpr unsigned shadow_first = 0x50, true_condition = 31, i8_code = 0x18;
constexpr unsigned px = 0x6b, px1 = 0x6c, px2 = 0x6d;
constexpr std::array ustat{0x70U, 0x71U, 0x7eU, 0x7fU};
constexpr unsigned lcntr_code = 0x68;
constexpr unsigned emuclk_code = 0x69, emuclk2_code = 0x6a;
constexpr unsigned pc_code = 0x63, pcstk_code = 0x64, pcstkp_code = 0x65;
constexpr unsigned laddr_code = 0x66, curlcntr_code = 0x67;
constexpr std::uint32_t empty_pc_stack = 0x03ffffff, empty_loop_stack = 0xffffffff;
constexpr unsigned mode1_code = 0x72, astat_x_code = 0x76, astat_y_code = 0x77;
constexpr unsigned stky_x_code = 0x78, stky_y_code = 0x79, irptl_code = 0x7a;
constexpr std::array lane_status_codes{astat_x_code, astat_y_code, stky_x_code, stky_y_code};

struct ScalarControl {
  unsigned code;
  std::uint32_t sharc::Registers::*member;
  std::uint32_t seed;
};

constexpr std::array scalar_controls{
    ScalarControl{.code = lcntr_code, .member = &sharc::Registers::lcntr, .seed = 0x1234},
    ScalarControl{.code = mode1_code, .member = &sharc::Registers::mode1, .seed = 0},
    ScalarControl{.code = 0x73, .member = &sharc::Registers::mmask, .seed = 0x17abcdef},
    ScalarControl{.code = 0x74, .member = &sharc::Registers::mode2, .seed = 0x0400},
    ScalarControl{.code = 0x75, .member = &sharc::Registers::flags, .seed = 0x0a},
    ScalarControl{.code = irptl_code, .member = &sharc::Registers::irptl, .seed = 0x0280},
    ScalarControl{.code = 0x7b, .member = &sharc::Registers::imask, .seed = 0x126000},
    ScalarControl{.code = 0x7c, .member = &sharc::Registers::imaskp, .seed = 0x18000},
    ScalarControl{.code = 0x7d, .member = &sharc::Registers::lirptl, .seed = 0x24},
};
constexpr std::uint64_t extension = 0xab;
constexpr std::uint32_t old_sticky = 0xa5000040;
constexpr std::uint64_t clock_seed = 123;
constexpr std::array bank_bits{4U, 3U, 6U, 5U};

struct Move {
  unsigned source{}, destination{};
  bool swap{};
};

std::uint64_t encoding(Move move, unsigned condition = 0) {
  constexpr std::uint64_t prefix = 0x700000000000;
  constexpr std::uint64_t swap_prefix = 0x780000000000;
  const auto source = move.swap ? (std::uint64_t{move.source & 15} << source_shift)
                                : (std::uint64_t{move.source >> 2} << source_shift) |
                                      (std::uint64_t{move.source & 3} << source_low_shift);
  return (move.swap ? swap_prefix : prefix) | source |
         (std::uint64_t{condition} << condition_shift) |
         (std::uint64_t{move.destination} << destination_shift);
}

bool is_data(unsigned reg) {
  constexpr unsigned sreg_first = 0x60;
  return reg < 16 || (reg >= shadow_first && reg < sreg_first);
}

bool px_half(unsigned reg) {
  return reg == px1 || reg == px2;
}

bool has_peer(unsigned reg) {
  return is_data(reg) || px_half(reg) || std::ranges::contains(lane_status_codes, reg) ||
         std::ranges::contains(ustat, reg);
}

unsigned data_bank(const sharc::Registers &state, unsigned reg) {
  constexpr unsigned low_bit = 10;
  constexpr unsigned high_bit = 7;
  return (state.mode1 >> ((reg & 15) < 8 ? low_bit : high_bit)) & 1;
}

template <class State> auto &dag(State &state, unsigned reg) {
  const auto index = reg & 15;
  const auto bank = (state.mode1 >> bank_bits[index / 4]) & 1;
  auto &value = state.dag[bank][index];
  switch (reg >> 4) {
    case 1: return value.index;
    case 2: return value.modifier;
    case 3: return value.length;
    default: return value.base;
  }
}

std::optional<std::uint32_t>
control_word(const sharc::Registers &state, unsigned reg, bool implicit) {
  const auto lane = (reg & 1) ^ unsigned { implicit };
  if (reg == astat_x_code || reg == astat_y_code) { return state.pe[lane].astat; }
  if (reg == stky_x_code || reg == stky_y_code) { return state.pe[lane].stky; }
  const auto *const control = std::ranges::find(scalar_controls, reg, &ScalarControl::code);
  if (control != scalar_controls.end()) { return state.*control->member; }
  return std::nullopt;
}

std::uint64_t read(const sharc::Registers &state, unsigned reg, bool implicit) {
  if (reg == px) {
    constexpr unsigned register_alignment = 24;
    return state.px >> register_alignment;
  }
  if (px_half(reg)) {
    constexpr unsigned word_bits = 32;
    const auto shift = ((reg == px2) != implicit) ? word_bits : 0U;
    return std::uint64_t{static_cast<std::uint32_t>(state.px >> shift)} << 8;
  }
  const auto peer = implicit ? reg ^ 1 : reg;
  const auto *const status = std::ranges::find(ustat, peer);
  if (status != ustat.end()) {
    return std::uint64_t{state.ustat[static_cast<std::size_t>(status - ustat.begin())]} << 8;
  }
  if (const auto word = control_word(state, reg, implicit)) { return std::uint64_t{*word} << 8; }
  if (!is_data(reg)) { return std::uint64_t{dag(state, reg)} << 8; }
  const auto lane = unsigned{reg >= shadow_first} ^ unsigned { implicit };
  return state.pe[lane].data[data_bank(state, reg)][reg & 15];
}

void write(sharc::Registers &state, unsigned reg, bool implicit, std::uint64_t raw) {
  if (reg == px) {
    constexpr unsigned register_alignment = 24;
    state.px = raw << register_alignment;
    return;
  }
  const auto value = static_cast<std::uint32_t>(raw >> 8);
  if (reg == astat_x_code || reg == astat_y_code) {
    constexpr std::uint32_t writable = 0xff043fff;
    constexpr std::uint32_t fifo_status = 1U << 14;
    auto &lane = state.pe[(reg & 1) ^ unsigned{implicit}].astat;
    lane = (lane & fifo_status) | (value & writable);
    return;
  }
  if (reg == stky_x_code || reg == stky_y_code) {
    constexpr std::array<std::uint32_t, 2> writable{0x001e03e7, 0x000003e7};
    constexpr std::uint32_t stack_flags = 0x07e00000;
    const auto lane = (reg & 1) ^ unsigned { implicit };
    auto &stky = state.pe[lane].stky;
    stky = (lane == 0 ? stky & stack_flags : 0) | (value & writable[lane]);
    return;
  }
  if (reg == lcntr_code) {
    state.lcntr = value;
    return;
  }
  if (px_half(reg)) {
    constexpr unsigned word_bits = 32;
    const auto shift = ((reg == px2) != implicit) ? word_bits : 0U;
    constexpr std::uint64_t word_mask = 0xffffffff;
    state.px = (state.px & ~(word_mask << shift)) | (std::uint64_t{value} << shift);
    return;
  }
  const auto peer = implicit ? reg ^ 1 : reg;
  const auto *const status = std::ranges::find(ustat, peer);
  if (status != ustat.end()) {
    state.ustat[static_cast<std::size_t>(status - ustat.begin())] = value;
    return;
  }
  if (!is_data(reg)) {
    dag(state, reg) = value;
    return;
  }
  const auto lane = unsigned{reg >= shadow_first} ^ unsigned { implicit };
  state.pe[lane].data[data_bank(state, reg)][reg & 15] = raw;
}

void apply(const sharc::Registers &before, Move move, sharc::Registers &state) {
  if (move.source == px && move.destination == px) { return; }
  const bool x = (before.pe[0].astat & 1) != 0;
  const bool y = (before.pe[1].astat & 1) != 0;
  const bool simd = (before.mode1 & (1U << simd_bit)) != 0;
  if (move.source == px && !is_data(move.destination) && !px_half(move.destination) &&
      !std::ranges::contains(lane_status_codes, move.destination)) {
    if (x) {
      const auto low = static_cast<std::uint32_t>(before.px);
      write(state, move.destination, false, std::uint64_t{low} << 8);
    }
    return;
  }
  if (move.destination == px && !is_data(move.source) && !px_half(move.source)) {
    if (x) {
      constexpr std::uint64_t half_radix = 0x100000000;
      const auto *const reg = std::ranges::find(ustat, move.source);
      if (reg == ustat.end()) {
        state.px = (half_radix + 1) * (read(before, move.source, false) >> 8);
      } else {
        const auto pos = static_cast<std::size_t>(reg - ustat.begin());
        state.px = before.ustat[pos] + (half_radix * before.ustat[pos ^ 1]);
      }
    }
    return;
  }
  if (x) { write(state, move.destination, false, read(before, move.source, false)); }
  if (move.swap) {
    if (simd ? y : x) { write(state, move.source, false, read(before, move.destination, false)); }
  } else if (simd && y && has_peer(move.destination)) {
    write(state, move.destination, true, read(before, move.source, true));
  }
}

void advance(sharc::Registers &state) {
  fixture::boundary(state, {.pc = state.pc + 1, .clock = state.emuclk + 1});
}

sharc::Registers initial(unsigned configuration) {
  constexpr std::uint64_t raw_seed = 0x8123456701;
  constexpr std::uint32_t dag_seed = 0x12340000;
  sharc::Registers state;
  state.pc = base;
  state.emuclk = clock_seed;
  constexpr std::uint64_t px_seed = 0x89abcdef12ab5678;
  constexpr std::array<std::uint32_t, 4> status_seed{
      0x12347268, 0xf1946aef, 0x919d1347, 0xfa8253ea};
  state.px = px_seed;
  state.ustat = status_seed;
  state.mode1 = ((configuration & 4) << (simd_bit - 2)) | (((configuration >> 3) & 1) << 10) |
                (((configuration >> 4) & 1) << 7);
  for (unsigned quarter = 0; quarter < bank_bits.size(); ++quarter) {
    state.mode1 |= ((configuration >> (5 + quarter)) & 1) << bank_bits[quarter];
  }
  for (unsigned lane = 0; lane < 2; ++lane) {
    state.pe[lane].astat = (configuration >> lane) & 1;
    state.pe[lane].stky = old_sticky + lane;
    for (unsigned bank = 0; bank < 2; ++bank) {
      for (unsigned reg = 0; reg < 16; ++reg) {
        state.pe[lane].data[bank][reg] =
            raw_seed + (std::uint64_t{lane} * 256) + (std::uint64_t{bank} * 32) + reg;
      }
    }
  }
  for (unsigned reg = 0x10; reg < shadow_first; ++reg) { dag(state, reg) = dag_seed + reg; }
  return state;
}

void verify_moves(const std::vector<Move> &moves) {
  std::vector<std::uint64_t> words;
  words.reserve(moves.size());
  for (const auto &move : moves) { words.push_back(encoding(move)); }
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    for (unsigned configuration = 0; configuration < 512; ++configuration) {
      auto expected = initial(configuration);
      for (const auto &control : scalar_controls) { expected.*control.member |= control.seed; }
      core.set_registers(expected);
      const auto saved = core.save();
      for (unsigned count = 1; count <= moves.size(); ++count) {
        const auto before = expected;
        apply(before, moves[count - 1], expected);
        advance(expected);
        core.restore(saved);
        const auto result = core.run(count);
        check(fixture::completed(result, count, count) && core.registers() == expected,
              "raw register move/swap lost an old source, lane, physical bank or complete prefix");
      }
    }
  }
}

void raw_moves() {
  for (unsigned quarter = 0; quarter < 4; ++quarter) {
    std::vector<Move> moves;
    for (unsigned index = quarter * 4; index < quarter * 4 + 4; ++index) {
      for (unsigned field = 1; field <= 4; ++field) {
        const auto reg = (field * 16) + index;
        const auto data = (field & 1) != 0 ? index : shadow_first + index;
        moves.push_back({.source = reg, .destination = data});
        moves.push_back({.source = data, .destination = reg});
      }
      for (unsigned direction = 0; direction < 4; ++direction) {
        moves.push_back(
            {.source = index + ((direction & 1) != 0 ? shadow_first : 0),
             .destination = ((index + 5) & 15) + ((direction & 2) != 0 ? shadow_first : 0)});
      }
    }
    verify_moves(moves);
  }
  std::vector<Move> swaps;
  swaps.reserve(16);
  for (unsigned reg = 0; reg < 16; ++reg) {
    swaps.push_back({.source = shadow_first + (15 - reg), .destination = reg, .swap = true});
  }
  verify_moves(swaps);
  const std::vector<Move> cross_quarters{Move{.source = 0x11, .destination = 0x2d},
                                         Move{.source = 0x3e, .destination = 0x40}};
  verify_moves(cross_quarters);
}

void system_moves() {
  constexpr unsigned shadow4 = shadow_first + 4;
  const std::vector<Move> moves{
      {.source = ustat[0], .destination = ustat[1]},
      {.source = ustat[3], .destination = px1},
      {.source = px2, .destination = ustat[2]},
      {.source = ustat[1], .destination = shadow4},
      {.source = shadow4, .destination = ustat[1]},
      {.source = px1, .destination = px2},
      {.source = i8_code, .destination = ustat[0]},
      {.source = px2, .destination = i8_code},
      {.source = px, .destination = 4},
      {.source = px, .destination = shadow4},
      {.source = 4, .destination = px},
      {.source = shadow4, .destination = px},
  };
  verify_moves(moves);
}

void control_reads() {
  constexpr unsigned s10 = shadow_first + 10;
  constexpr unsigned i12 = 0x1c;
  for (const auto destination : {4U, s10, i12, ustat[2], px}) {
    std::vector<Move> moves;
    moves.reserve(scalar_controls.size() + lane_status_codes.size());
    for (const auto &control : scalar_controls) {
      moves.push_back({.source = control.code, .destination = destination});
    }
    for (const auto source : lane_status_codes) {
      moves.push_back({.source = source, .destination = destination});
    }
    verify_moves(moves);
  }
}

void loop_preload_moves() {
  constexpr unsigned i12 = 0x1c;
  std::vector<Move> moves;
  for (const auto source :
       {6U, shadow_first + 6, i12, ustat[1], px, px1, astat_y_code, lcntr_code}) {
    moves.push_back({.source = source, .destination = lcntr_code});
  }
  verify_moves(moves);
}

void sticky_status_moves() {
  constexpr unsigned i12 = 0x1c;
  for (const auto destination : {stky_x_code, stky_y_code}) {
    std::vector<Move> moves;
    for (const auto source : {6U, shadow_first + 6, i12, ustat[1], px, px1, stky_y_code}) {
      moves.push_back({.source = source, .destination = destination});
    }
    verify_moves(moves);
  }
}

void read_latched_dag_overflow() {
  constexpr std::uint64_t modify_i7_by_one = 0x160700000001;
  constexpr std::uint32_t overflow = 1U << 20;
  constexpr sharc::AddressGenerator circular{.index = 0x2003, .length = 4, .base = 0x2000};
  const std::array words{modify_i7_by_one,
                         encoding({.source = irptl_code, .destination = 4}, true_condition)};
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    sharc::Registers state;
    state.pc = base;
    state.irptl = 2;
    state.dag[0][7] = circular;
    fixture::install(core, native, state);
    const auto report = core.run(words.size());
    const auto actual = core.registers();
    const auto expected = state.irptl | overflow;
    check(fixture::completed(report, words.size(), words.size()) && actual.irptl == expected &&
              actual.pe[0].data[0][4] == (std::uint64_t{expected} << 8),
          "IRPTL read missed a DAG overflow in the same native grant");
  }
}

void emulation_clock_wrap() {
  constexpr std::uint64_t seed = 0x12345678fffffffe;
  constexpr std::uint32_t low = 0xfffffffe;
  constexpr std::uint32_t high = 0x12345678;
  constexpr std::uint32_t high_after_wrap = high + 1;
  constexpr std::uint64_t px_word = 0x1234567912345679;
  for (const auto reg : {emuclk_code, emuclk2_code}) {
    const auto code = fixture::image(sharc::InstructionSet::isa, fixture::immediate(reg, 0));
    const auto fault = fixture::refusal(code, "user-space EMUCLK write was lowered");
    check(fault.reason == sharc::FaultReason::unsupported_instruction,
          "EMUCLK write lost its explicit refusal");
  }
  const std::array words{encoding({.source = emuclk_code, .destination = 4}, true_condition),
                         encoding({.source = emuclk2_code, .destination = 5}, true_condition),
                         encoding({.source = emuclk_code, .destination = 6}, true_condition),
                         encoding({.source = emuclk2_code, .destination = 7}, true_condition),
                         encoding({.source = emuclk2_code, .destination = px}, true_condition)};
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    sharc::Core replay(program.memory(), mode);
    auto expected = initial(4);
    expected.emuclk = seed;
    fixture::install(core, native, expected);
    (void)replay.install(native, fixture::Program::variant_id);
    const auto beginning = core.save();
    check(fixture::completed(core.run(2), 2, 2), "clock prefix did not retire");
    replay.restore(core.save());
    check(fixture::completed(replay.run(3), 3, 3), "clock replay did not retire");
    for (auto &pe : expected.pe) {
      pe.data[0][4] = std::uint64_t{low} << 8;
      pe.data[0][5] = std::uint64_t{high} << 8;
      pe.data[0][6] = 0;
      pe.data[0][7] = std::uint64_t{high_after_wrap} << 8;
    }
    expected.px = px_word;
    fixture::boundary(expected, {.pc = base + 5, .clock = seed + 5});
    check(replay.registers() == expected, "clock halves lost the carry or SIMD projection");
    core.restore(beginning);
    check(fixture::completed(core.run(words.size()), words.size(), words.size()) &&
              core.registers() == expected,
          "continuous clock reads differed from the snapshot replay");
  }
}

void emulation_clock_short_loop() {
  constexpr std::uint64_t count_three_do_one = 0x0c0003000001;
  constexpr std::uint64_t seed = 0x8abcde00;
  const std::array words{encoding({.source = emuclk_code, .destination = 4}, true_condition),
                         count_three_do_one,
                         encoding({.source = emuclk_code, .destination = 5}, true_condition),
                         encoding({.source = emuclk_code, .destination = 6}, true_condition)};
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    auto before = initial(0);
    before.emuclk = seed;
    fixture::install(core, native, before);
    const auto report = core.run(6);
    const auto data = core.registers().pe[0].data[0];
    check(fixture::completed(report, 6, 10) && data[4] == (seed << 8) &&
              data[5] == ((seed + 5) << 8) && data[6] == ((seed + 9) << 8),
          "clock read in a short DO lost a turn or refill cycle");
  }
}

void verify_sequencer_reads(sharc::Core &core, bool filled) {
  constexpr std::uint32_t tagged_return = 0x03543210;
  constexpr std::uint32_t loop_address = 0xef000133;
  constexpr std::uint32_t count = 0x56781234;
  auto expected = initial(4);
  expected.pc_stack_pointer = filled ? 2 : 0;
  expected.loop_address_depth = expected.loop_counter_depth = filled ? 2 : 0;
  expected.pc_stack[1] = tagged_return;
  expected.loop_address_stack[1] = loop_address;
  expected.loop_counter_stack[1] = count;
  core.set_registers(expected);
  const std::array reads{base,
                         filled ? tagged_return : empty_pc_stack,
                         filled ? 2U : 0U,
                         filled ? loop_address : empty_loop_stack,
                         filled ? count : empty_loop_stack};
  for (auto &pe : expected.pe) {
    for (std::size_t i = 0; i < reads.size(); ++i) {
      pe.data[0][4 + i] = std::uint64_t{reads[i]} << 8;
    }
  }
  fixture::boundary(expected, {.pc = base + 5, .clock = clock_seed + 5});
  check(fixture::completed(core.run(reads.size()), reads.size(), reads.size()) &&
            core.registers() == expected,
        "sequencer read lost a tag, logical stack top, empty value or lane");
}

void sequencer_register_reads() {
  const std::array words{encoding({.source = pc_code, .destination = 4}, true_condition),
                         encoding({.source = pcstk_code, .destination = 5}, true_condition),
                         encoding({.source = pcstkp_code, .destination = 6}, true_condition),
                         encoding({.source = laddr_code, .destination = 7}, true_condition),
                         encoding({.source = curlcntr_code, .destination = 8}, true_condition)};
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    for (const bool filled : {false, true}) { verify_sequencer_reads(core, filled); }
  }
}

constexpr std::uint32_t pc_stack_immediate = 0x89ab1234;

struct PcStackWrite {
  std::uint8_t depth;
  unsigned predicate;
};

void verify_pc_stack_writes(sharc::Core &core, PcStackWrite input) {
  const auto [depth, predicate] = input;
  constexpr std::uint32_t reg_word = 0xf3245678;
  constexpr std::uint64_t y_word = 0x8765432100;
  constexpr std::uint32_t low_px = 0x02c01234;
  constexpr std::uint64_t raw_px = 0x01d0abcd00000000 | low_px;
  sharc::Registers expected;
  expected.pc = base;
  expected.mode1 = 1U << simd_bit;
  expected.pc_stack_pointer = depth;
  expected.pc_stack.fill(base + 1);
  expected.pe[0].astat = predicate & 1;
  expected.pe[1].astat = predicate >> 1;
  expected.pe[0].data[0][6] = (std::uint64_t{reg_word} << 8) | extension;
  expected.pe[1].data[0][6] = y_word;
  expected.px = raw_px;
  core.set_registers(expected);
  const bool occupied = depth != 0 && depth <= sharc::Registers::pc_stack_capacity;
  const auto old_top = occupied ? expected.pc_stack[depth - 1] : empty_pc_stack;
  const auto after_move = occupied && (predicate & 1) != 0 ? reg_word & empty_pc_stack : old_top;
  const auto after_immediate = occupied ? pc_stack_immediate & empty_pc_stack : old_top;
  if (occupied) { expected.pc_stack[depth - 1] = low_px; }
  for (auto &pe : expected.pe) {
    pe.data[0][4] = std::uint64_t{after_move} << 8;
    pe.data[0][5] = std::uint64_t{after_immediate} << 8;
  }
  fixture::boundary(expected, {.pc = base + 5, .clock = 5});
  check(fixture::completed(core.run(5), 5, 5) && core.registers() == expected,
        "a PCSTK write lost the stack top, X predicate, raw mask, or PX1 source");
}

void pc_stack_writes() {
  constexpr std::array<std::uint8_t, 4> depths{0, 2, sharc::Registers::pc_stack_capacity, 31};
  const std::array words{encoding({.source = 6, .destination = pcstk_code}),
                         encoding({.source = pcstk_code, .destination = 4}, true_condition),
                         fixture::immediate(pcstk_code, pc_stack_immediate),
                         encoding({.source = pcstk_code, .destination = 5}, true_condition),
                         encoding({.source = px, .destination = pcstk_code}, true_condition)};
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    for (const auto depth : depths) {
      for (unsigned predicate = 0; predicate < 4; ++predicate) {
        verify_pc_stack_writes(core, {.depth = depth, .predicate = predicate});
      }
    }
  }
}

constexpr std::uint32_t loop_address_immediate = 0xef123456;

void verify_loop_address_write(sharc::Core &core, bool occupied, unsigned predicate) {
  constexpr std::uint32_t original = 0x00456789;
  constexpr std::uint32_t from_r6 = 0x00543210;
  constexpr std::uint32_t from_px1 = 0x00654321;
  constexpr std::uint64_t raw_px = 0x0076543200000000 | from_px1;
  sharc::Registers expected;
  expected.pc = base;
  expected.loop_address_depth = expected.loop_counter_depth = occupied ? 2 : 0;
  expected.loop_address_stack[0] = base;
  expected.loop_address_stack[1] = original;
  expected.loop_counter_stack[1] = 3;
  expected.mode1 = 1U << simd_bit;
  expected.pe[0].astat = predicate & 1;
  expected.pe[1].astat = predicate >> 1;
  expected.pe[0].data[0][6] = (std::uint64_t{from_r6} << 8) | extension;
  expected.px = raw_px;
  core.set_registers(expected);
  const auto after_register = (predicate & 1) != 0 ? from_r6 : original;
  for (auto &pe : expected.pe) {
    pe.data[0][4] = std::uint64_t{occupied ? after_register : empty_loop_stack} << 8;
    pe.data[0][5] = std::uint64_t{occupied ? loop_address_immediate : empty_loop_stack} << 8;
  }
  if (occupied) { expected.loop_address_stack[1] = from_px1; }
  fixture::boundary(expected, {.pc = base + 5, .clock = 5});
  check(fixture::completed(core.run(5), 5, 5) && core.registers() == expected,
        "an LADDR write lost the selected stack cell, X predicate or PX1 source");
}

void loop_address_writes() {
  const std::array words{encoding({.source = 6, .destination = laddr_code}),
                         encoding({.source = laddr_code, .destination = 4}, true_condition),
                         fixture::immediate(laddr_code, loop_address_immediate),
                         encoding({.source = laddr_code, .destination = 5}, true_condition),
                         encoding({.source = px, .destination = laddr_code}, true_condition)};
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    for (const bool occupied : {false, true}) {
      for (unsigned predicate = 0; predicate < 4; ++predicate) {
        verify_loop_address_write(core, occupied, predicate);
      }
    }
  }
}

struct LoopAddressEdit {
  unsigned site, endpoint, instructions, early, old, late;
  bool missed{}, committed{}, immediate{};
};

void rewrite_loop_address(const LoopAddressEdit &edit) {
  constexpr unsigned old_end = 13;
  constexpr unsigned early_end = 11;
  constexpr unsigned late_end = 15;
  constexpr unsigned source = 6;
  constexpr std::uint64_t do_three = 0x0c0003000000;
  constexpr std::uint32_t counted = 0xef000000;
  const auto replacement = counted | (base + edit.endpoint);
  std::array<std::uint64_t, late_end + 2> words{};
  words.front() = do_three | old_end;
  words[edit.site] = edit.immediate
                         ? fixture::immediate(laddr_code, replacement)
                         : encoding({.source = source, .destination = laddr_code}, true_condition);
  words[early_end] = fixture::add(2, 2, 1);
  words[old_end] = fixture::add(3, 3, 1);
  words[late_end] = fixture::add(4, 4, 1);
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    sharc::Registers before;
    before.pc = base;
    before.pe[0].data[0][1] = 1U << 8;
    before.pe[0].data[0][source] = std::uint64_t{replacement} << 8;
    fixture::install(core, native, before);
    const auto prefix = edit.site + 1;
    check(fixture::completed(core.run(prefix), prefix, prefix),
          "failed to reach the LADDR write boundary");
    const auto after_write = core.registers();
    check(after_write.loop_address_stack[0] == replacement &&
              (!edit.committed || after_write.faddr == base + 1),
          "the next fetch lost the committed endpoint after rewriting LADDR");
    sharc::Core replay(program.memory(), mode);
    (void)replay.install(native, fixture::Program::variant_id);
    replay.restore(core.save());
    const auto remaining = edit.instructions - prefix;
    check(fixture::completed(replay.run(remaining), remaining, remaining),
          "replaying the LADDR edit did not retire the expected instructions and cycles");
    fixture::install(core, native, before);
    check(fixture::completed(core.run(edit.instructions), edit.instructions, edit.instructions) &&
              core.registers() == replay.registers(),
          "the LADDR edit changed across an execution boundary or snapshot");
    const auto after = core.registers();
    const auto &r = after.pe[0].data[0];
    check(r[2] == (std::uint64_t{edit.early} << 8) && r[3] == (std::uint64_t{edit.old} << 8) &&
              r[4] == (std::uint64_t{edit.late} << 8) && after.pc == base + late_end + 1 &&
              after.loop_address_depth == unsigned{edit.missed} &&
              after.loop_counter_depth == unsigned{edit.missed} &&
              after.loop_counter_stack[0] == (edit.missed ? 3U : 0U),
          "the rewritten loop used the wrong endpoint, counter or retirement boundary");
  }
}

void loop_address_edits() {
  // The distinct markers come from the early, original and late ends in the simulator probes.
  constexpr std::array edits{
      LoopAddressEdit{
          .site = 6, .endpoint = 11, .instructions = 38, .early = 3, .old = 1, .late = 1},
      LoopAddressEdit{.site = 7,
                      .endpoint = 11,
                      .instructions = 16,
                      .early = 1,
                      .old = 1,
                      .late = 1,
                      .missed = true},
      LoopAddressEdit{.site = 9,
                      .endpoint = 11,
                      .instructions = 40,
                      .early = 3,
                      .old = 2,
                      .late = 1,
                      .committed = true},
      LoopAddressEdit{.site = 8,
                      .endpoint = 15,
                      .instructions = 46,
                      .early = 3,
                      .old = 3,
                      .late = 3,
                      .immediate = true},
      LoopAddressEdit{.site = 9,
                      .endpoint = 15,
                      .instructions = 44,
                      .early = 3,
                      .old = 3,
                      .late = 2,
                      .committed = true},
      LoopAddressEdit{.site = 9,
                      .endpoint = 13,
                      .instructions = 42,
                      .early = 3,
                      .old = 3,
                      .late = 1,
                      .committed = true}};
  std::ranges::for_each(edits, rewrite_loop_address);
}

void pc_stack_pointer_latency() {
  constexpr std::uint32_t first_cell = 0x1081001;
  constexpr std::uint32_t third_cell = 0x2081003;
  constexpr std::uint32_t high_bits = 0xc0000000;
  constexpr std::uint32_t full_flag = 1U << 21;
  constexpr std::uint32_t empty_flag = 1U << 22;
  constexpr std::uint32_t stack_interrupt = 1U << 3;
  constexpr auto capacity = sharc::Registers::pc_stack_capacity;
  constexpr std::uint64_t raw_shift = 8;
  const std::array words{encoding({.source = 6, .destination = pcstkp_code}),
                         encoding({.source = pcstk_code, .destination = 4}, true_condition),
                         encoding({.source = pcstkp_code, .destination = 5}, true_condition)};
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    sharc::Core replay(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    (void)replay.install(native, fixture::Program::variant_id);
    for (const auto target : {0U, 1U, 29U, 30U, 31U}) {
      sharc::Registers expected;
      expected.pc = base;
      expected.mode1 = 1U << simd_bit;
      expected.pc_stack_pointer = 3;
      expected.pc_stack[0] = first_cell;
      expected.pc_stack[2] = third_cell;
      expected.pe[0].astat = 1;
      expected.pe[0].data[0][6] = std::uint64_t{high_bits | target} << raw_shift;
      core.set_registers(expected);
      const auto start = core.save();
      fixture::boundary(expected, {.pc = base + 1, .clock = 1});
      check(fixture::completed(core.run(1), 1, 1) && core.registers() == expected,
            "the PCSTKP write took effect before the following execute cycle");
      replay.restore(core.save());
      expected.pc_stack_pointer = static_cast<std::uint8_t>(target);
      expected.pe[0].stky = (target == 0 ? empty_flag : 0) | (target >= capacity ? full_flag : 0);
      expected.irptl = target == capacity - 1 ? stack_interrupt : 0;
      for (auto &pe : expected.pe) {
        pe.data[0][4] = std::uint64_t{third_cell} << raw_shift;
        pe.data[0][5] = std::uint64_t{target} << raw_shift;
      }
      fixture::boundary(expected, {.pc = base + 3, .clock = 3});
      const auto deadline = replay.state().progress.base_cycles + 2;
      check(fixture::completed(replay.run_until(deadline), 2, 2) && replay.registers() == expected,
            "the PCSTKP effect lost the old top, new depth, stack flags, or interrupt latch");
      core.restore(start);
      check(fixture::completed(core.run(3), 3, 3) && core.registers() == expected,
            "continuous PCSTKP execution differed from snapshot replay");
    }
  }
}

void pc_stack_pointer_rts(unsigned gap) {
  constexpr std::uint64_t rts = 0x0a3e00000000;
  constexpr auto low = base + 5;
  constexpr auto high = base + 6;
  std::array<std::uint64_t, 7> words{fixture::immediate(pcstkp_code, 1)};
  words[gap + 1] = rts;
  fixture::Program program(words);
  program.code.variants.front().entries.insert(program.code.variants.front().entries.end(),
                                               {low, high});
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    sharc::Registers initial;
    initial.pc = base;
    initial.pc_stack_pointer = 3;
    initial.pc_stack[0] = low;
    initial.pc_stack[2] = high;
    fixture::install(core, native, initial);
    const auto count = gap + 2;
    check(fixture::completed(core.run(count), count, count + 3),
          "PCSTKP followed by RTS failed to retire");
    const auto result = core.registers();
    check(result.pc == (gap < 2 ? high : low) && result.pc_stack_pointer == unsigned{gap == 0} &&
              result.pc_stack == initial.pc_stack,
          "RTS used the wrong target sample or popped the wrong pointer");
  }
}

void pc_stack_pointer_in_delayed_return() {
  constexpr std::uint64_t rts_db = 0x0a3e04000000;
  constexpr auto target = base + 6;
  const std::array<std::uint64_t, 7> words{
      rts_db,
      0,
      fixture::immediate(pcstkp_code, 1),
      0,
      0,
      0,
      encoding({.source = pcstkp_code, .destination = 4}, true_condition)};
  fixture::Program program(words);
  program.code.variants.front().entries.push_back(target);
  const auto native = fixture::compile(program.code, sharc::ExecutionMode::strict);
  sharc::Core core(program.memory());
  sharc::Registers initial;
  initial.pc = base;
  initial.pc_stack_pointer = 3;
  initial.pc_stack[2] = target;
  fixture::install(core, native, initial);
  check(fixture::completed(core.run(3), 3, 4) && core.registers().pc == target &&
            core.registers().pc_stack_pointer == 1,
        "the return refill failed to mature the second-slot PCSTKP write");
  check(fixture::completed(core.run(1), 1, 1) && core.registers().pe[0].data[0][4] == 1U << 8,
        "a declared return entry lost the stack-pointer continuation");
}

void pc_stack_pointer_before_do() {
  constexpr std::uint64_t two_turns = 0x0c0002000007;
  const std::array<std::uint64_t, 10> words{
      fixture::immediate(pcstkp_code, 1),
      two_turns,
      encoding({.source = pcstkp_code, .destination = 4}, true_condition)};
  fixture::Program program(words);
  const auto native = fixture::compile(program.code, sharc::ExecutionMode::strict);
  sharc::Core core(program.memory());
  sharc::Registers initial;
  initial.pc = base;
  initial.pc_stack_pointer = 3;
  fixture::install(core, native, initial);
  constexpr unsigned count = 16;
  check(fixture::completed(core.run(count), count, count) &&
            core.registers().pc_stack_pointer == 1 && core.registers().pe[0].data[0][4] == 2U << 8,
        "DO did not create and remove its frame above the forwarded stack depth");
}

void rewrite_delayed_return(bool first_slot) {
  constexpr std::uint64_t call_db = 0x06be04000000;
  constexpr std::uint64_t rts = 0x0a3e00000000;
  constexpr auto callee = base + 5;
  constexpr auto alternate = base + 6;
  constexpr std::uint32_t original = (base + 3) | (1U << 24);
  const auto write = encoding({.source = 6, .destination = pcstk_code}, true_condition);
  const auto read = encoding({.source = pcstk_code, .destination = 4}, true_condition);
  const std::array words{call_db | callee,
                         first_slot ? write : read,
                         first_slot ? read : write,
                         std::uint64_t{0},
                         std::uint64_t{0},
                         rts,
                         std::uint64_t{0}};
  fixture::Program program(words);
  program.code.variants.front().entries.push_back(alternate);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    sharc::Core replay(program.memory(), mode);
    sharc::Registers initial;
    initial.pc = base;
    initial.pe[0].data[0][6] = std::uint64_t{alternate} << 8;
    fixture::install(core, native, initial);
    (void)replay.install(native, fixture::Program::variant_id);
    const auto start = core.save();
    const auto slot_read = first_slot ? alternate : original;
    check(fixture::completed(core.run(2), 2, 2) && core.registers().pc_stack[0] == slot_read,
          "the first CALL slot exposed the wrong stack word at its boundary");
    replay.restore(core.save());
    check(fixture::completed(replay.run(2), 2, 6), "the rewritten return failed after a snapshot");
    const auto result = replay.registers();
    check(result.pc == alternate && result.pc_stack_pointer == 0 &&
              result.pe[0].data[0][4] == (std::uint64_t{slot_read} << 8),
          "the slot read or RTS observed the wrong return address");
    core.restore(start);
    check(fixture::completed(core.run(4), 4, 8) && core.registers() == result,
          "bounded and continuous execution disagreed on the rewritten PC stack");
  }
}

void refuse_unimplemented_loop_stack_write(unsigned reg) {
  constexpr std::uint64_t do_three = 0x0c0003000007;
  const auto write = fixture::immediate(reg, base + 2);
  const std::array<std::uint64_t, 9> words{do_three, write};
  fixture::Program program(words);
  const auto native = fixture::compile(program.code, sharc::ExecutionMode::strict);
  sharc::Core core(program.memory());
  sharc::Registers initial;
  initial.pc = base;
  fixture::install(core, native, initial);
  const auto result = core.run(words.size());
  const auto state = core.registers();
  check(result.has_fault && result.fault.reason == sharc::FaultReason::unsupported_feature &&
            result.fault.address == base + 1 && result.fault.instruction == write &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            state.pc_stack_pointer == 1 && state.pc_stack[0] == base + 1 &&
            state.loop_address_depth == 1,
        "an unsupported stack write changed a live DO frame or lost the exact prefix");
}

void pc_and_stack_in_delay_slots() {
  constexpr std::uint64_t call_db = 0x06be04000000;
  constexpr std::uint64_t rts = 0x0a3e00000000;
  constexpr auto target = base + 5;
  constexpr std::uint64_t nop = 0;
  const std::array words{call_db | target,
                         encoding({.source = pc_code, .destination = 4}, true_condition),
                         encoding({.source = pcstk_code, .destination = 5}, true_condition),
                         encoding({.source = pcstkp_code, .destination = 6}, true_condition),
                         nop,
                         encoding({.source = pcstkp_code, .destination = 7}, true_condition),
                         rts};
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    sharc::Core replay(program.memory(), mode);
    fixture::install(core, native, initial(0));
    (void)replay.install(native, fixture::Program::variant_id);
    const auto beginning = core.save();
    check(fixture::completed(core.run(2), 2, 2), "CALL and its first slot did not retire");
    replay.restore(core.save());
    check(fixture::completed(replay.run(4), 4, 8), "delay-slot stack observation did not replay");
    const auto expected = replay.registers();
    const auto &data = expected.pe[0].data[0];
    constexpr auto return_with_call_tag = (base + 3) | (1U << 24);
    check(data[4] == ((base + 1) << 8) && data[5] == (std::uint64_t{return_with_call_tag} << 8) &&
              data[6] == 0 && data[7] == (1U << 8) && expected.pc == base + 4,
          "PC or PC stack read selected the wrong delayed address or stack frame");
    core.restore(beginning);
    check(fixture::completed(core.run(6), 6, 10) && core.registers() == expected,
          "continuous delay-slot stack observation differed from replay");
  }
}

struct LoopRegisterRead {
  unsigned port, length, position;
  std::uint32_t word;
};

void loop_register_read(LoopRegisterRead probe) {
  constexpr std::uint64_t three_turns = 0x0c0003000000;
  std::vector<std::uint64_t> words(probe.length + 2);
  words.front() = three_turns | probe.length;
  words[probe.position + 1] = encoding({.source = probe.port, .destination = 4}, true_condition);
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    fixture::install(core, native, initial(0));
    const auto count = 1 + (3 * probe.length);
    const auto report = core.run(count);
    const auto state = core.registers();
    check(!report.has_fault && report.progress.instructions == count &&
              state.pe[0].data[0][4] == (std::uint64_t{probe.word} << 8) &&
              state.loop_counter_depth == 0,
          "sequencer read lost the loop count or the early pop on the final turn");
  }
}

void loop_register_reads() {
  constexpr std::uint32_t long_counted = 0xef000000;
  constexpr std::array cases{
      LoopRegisterRead{.port = curlcntr_code, .length = 1, .position = 0, .word = empty_loop_stack},
      LoopRegisterRead{.port = curlcntr_code, .length = 7, .position = 1, .word = 1},
      LoopRegisterRead{
          .port = laddr_code, .length = 5, .position = 0, .word = long_counted | (base + 5)},
      LoopRegisterRead{.port = laddr_code, .length = 5, .position = 1, .word = empty_loop_stack},
      LoopRegisterRead{.port = pcstkp_code, .length = 5, .position = 0, .word = 1},
      LoopRegisterRead{.port = pcstk_code, .length = 5, .position = 1, .word = empty_pc_stack}};
  std::ranges::for_each(cases, loop_register_read);
}

struct LoopCounterWrite {
  unsigned distance, count, replacement, turns;
  std::uint32_t observed;
};

void loop_counter_write(LoopCounterWrite probe) {
  constexpr unsigned length = 9;
  constexpr unsigned count_shift = 24;
  constexpr unsigned first_moved = 6;
  constexpr unsigned readback = 8;
  constexpr unsigned read_distance = 2;
  constexpr std::uint64_t immediate_do = 0x0c0000000000;
  std::array<std::uint64_t, length + 2> words{};
  words.front() = immediate_do | (std::uint64_t{probe.count} << count_shift) | length;
  const auto write_site = length - probe.distance;
  words[write_site] = encoding({.source = first_moved, .destination = curlcntr_code});
  words[length - read_distance] = encoding({.source = curlcntr_code, .destination = readback});
  words[length] = fixture::add(0, 0, 1);
  fixture::Program program(words);
  auto before = initial(1);
  before.pe[0].data[0].fill(0);
  before.pe[0].data[0][1] = 1U << 8;
  before.pe[0].data[0][first_moved] = std::uint64_t{probe.replacement} << 8;
  const auto prefix = write_site + 1;
  const auto retired = 1 + (length * probe.turns);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    fixture::install(core, native, before);
    check(fixture::completed(core.run(retired), retired, retired),
          "CURLCNTR write changed loop retirement or clock");
    const auto expected = core.registers();
    check(expected.pe[0].data[0][0] == (std::uint64_t{probe.turns} << 8) &&
              expected.pe[0].data[0][readback] == (std::uint64_t{probe.observed} << 8) &&
              expected.loop_counter_depth == 0 && expected.lcntr == probe.count &&
              expected.pc == base + length + 1,
          "CURLCNTR write lost sample precedence or changed the preload");
    fixture::install(core, native, before);
    check(fixture::completed(core.run(prefix), prefix, prefix),
          "CURLCNTR write did not reach the snapshot boundary");
    sharc::Core resumed(program.memory(), mode);
    (void)resumed.install(native, fixture::Program::variant_id);
    resumed.restore(core.save());
    const auto remaining = retired - prefix;
    check(fixture::completed(resumed.run(remaining), remaining, remaining) &&
              resumed.registers() == expected,
          "CURLCNTR write changed after snapshot replay");
  }
}

void loop_counter_writes() {
  constexpr std::array cases{
      LoopCounterWrite{.distance = 6, .count = 1, .replacement = 4, .turns = 4, .observed = 3},
      LoopCounterWrite{.distance = 5, .count = 1, .replacement = 4, .turns = 5, .observed = 4},
      LoopCounterWrite{
          .distance = 5, .count = 2, .replacement = 1, .turns = 1, .observed = empty_loop_stack},
      LoopCounterWrite{
          .distance = 4, .count = 1, .replacement = 4, .turns = 1, .observed = empty_loop_stack},
      LoopCounterWrite{.distance = 4, .count = 2, .replacement = 1, .turns = 2, .observed = 1},
      LoopCounterWrite{.distance = 3, .count = 2, .replacement = 4, .turns = 5, .observed = 4}};
  std::ranges::for_each(cases, loop_counter_write);
}

void loop_counter_write_after_nested_pop() {
  constexpr unsigned outer_end = 19;
  constexpr unsigned inner_setup = 2;
  constexpr unsigned inner_end = 11;
  constexpr unsigned fetch_distance = 4;
  constexpr unsigned source = 6;
  constexpr unsigned readback = 8;
  constexpr unsigned turns = 4;
  constexpr std::uint64_t do_twice = 0x0c0002000000;
  constexpr std::uint64_t do_once = 0x0c0001000000;
  std::array<std::uint64_t, outer_end + 2> words{};
  words.front() = do_twice | outer_end;
  words[inner_setup] = do_once | (inner_end - inner_setup);
  words[inner_end - fetch_distance] = encoding({.source = source, .destination = curlcntr_code});
  words[inner_end - fetch_distance + 1] =
      encoding({.source = curlcntr_code, .destination = readback});
  words[inner_end] = fixture::add(0, 0, 1);
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    sharc::Registers before;
    before.pc = base;
    before.pe[0].astat = 1;
    before.pe[0].data[0][1] = 1U << 8;
    before.pe[0].data[0][source] = turns << 8;
    fixture::install(core, native, before);
    constexpr auto retired = 1 + (turns * outer_end);
    check(fixture::completed(core.run(retired), retired, retired),
          "CURLCNTR write after the inner pop changed retirement or clock");
    const auto result = core.registers();
    check(result.pe[0].data[0][0] == (turns << 8) &&
              result.pe[0].data[0][readback] == (turns << 8) && result.loop_address_depth == 0 &&
              result.loop_counter_depth == 0 && result.pc == base + outer_end + 1,
          "CURLCNTR did not replace the outer counter after the inner fetch popped its frame");
  }
}

void refuse_buffered_loop_counter_write() {
  constexpr std::uint64_t do_three = 0x0c0003000004;
  constexpr auto write = fixture::immediate(curlcntr_code, 2);
  const std::array<std::uint64_t, 6> words{do_three, write};
  fixture::Program program(words);
  const auto native = fixture::compile(program.code, sharc::ExecutionMode::strict);
  sharc::Core core(program.memory());
  fixture::install(core, native, initial(0));
  const auto result = core.run(words.size());
  check(result.has_fault && result.fault.reason == sharc::FaultReason::unsupported_feature &&
            result.fault.address == base + 1 && result.fault.instruction == write &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            core.registers().loop_counter_stack[0] == 3,
        "an unsupported buffered CURLCNTR write changed the frame or lost the exact prefix");
}

void combined_px_moves() {
  for (unsigned quarter = 0; quarter < 4; ++quarter) {
    std::vector<Move> from_px;
    std::vector<Move> to_px;
    const auto base_index = quarter * 4;
    for (unsigned field = 1; field <= 4; ++field) {
      const auto dag_reg = (field * 16) + base_index;
      from_px.push_back({.source = px, .destination = dag_reg + 1});
      to_px.push_back({.source = dag_reg, .destination = px});
    }
    from_px.push_back({.source = px, .destination = ustat[quarter]});
    to_px.push_back({.source = ustat[quarter], .destination = px});
    verify_moves(from_px);
    verify_moves(to_px);
  }
}

constexpr std::array px_alias_moves{Move{.source = px1, .destination = px},
                                    Move{.source = px2, .destination = px},
                                    Move{.source = px, .destination = px1},
                                    Move{.source = px, .destination = px2},
                                    Move{.source = px, .destination = px}};

void combined_px_aliases() {
  for (const auto &move : px_alias_moves) { verify_moves({move}); }
}

void verify_parallel(sharc::Core &core, Move move, unsigned configuration) {
  constexpr std::uint64_t positive_rail = 0x7fffffff00;
  constexpr std::uint64_t negative_rail = 0x8000000000;
  auto before = initial(configuration);
  for (auto &pe : before.pe) {
    for (auto &bank : pe.data) {
      bank[2] = positive_rail;
      bank[3] = 1U << 8;
    }
  }
  auto expected = before;
  for (unsigned lane = 0; lane < ((before.mode1 & (1U << simd_bit)) != 0 ? 2U : 1U); ++lane) {
    if ((before.pe[lane].astat & 1) != 0) {
      expected.pe[lane].data[data_bank(before, 0)][0] = negative_rail;
      expected.pe[lane].astat = 6;
      expected.pe[lane].stky |= 4;
    }
  }
  apply(before, move, expected);
  advance(expected);
  core.set_registers(before);
  const auto result = core.run(1);
  check(!result.has_fault && core.registers() == expected,
        "parallel UREG write lost old operands/predicates or ALU flags/write priority");
}

void parallel_priority() {
  constexpr std::uint64_t add_r0_r2_r3 = 0x1023;
  constexpr std::array moves{Move{.source = 1, .destination = 0},
                             Move{.source = 0, .destination = 0x18},
                             Move{.source = 0x50, .destination = 0x2c},
                             Move{.source = 0x18, .destination = 0},
                             Move{.source = ustat[0], .destination = 0},
                             Move{.source = astat_x_code, .destination = 0},
                             Move{.source = stky_y_code, .destination = 0},
                             Move{.source = 0, .destination = px2},
                             Move{.source = px, .destination = ustat[0]},
                             Move{.source = ustat[3], .destination = px},
                             Move{.source = 0, .destination = lcntr_code},
                             Move{.source = 0, .destination = astat_x_code},
                             Move{.source = 0, .destination = astat_y_code},
                             Move{.source = 0, .destination = stky_x_code},
                             Move{.source = 0, .destination = stky_y_code},
                             Move{.source = px, .destination = astat_y_code},
                             Move{.source = 0x50, .destination = 0, .swap = true}};
  const auto exercise = [](const Move &move) {
    const std::array words{encoding(move) | add_r0_r2_r3};
    fixture::Program program(words);
    for (const auto mode : fixture::numeric_modes) {
      const auto native = fixture::compile(program.code, mode);
      sharc::Core core(program.memory(), mode);
      (void)core.install(native, fixture::Program::variant_id);
      for (unsigned configuration = 0; configuration < 512; ++configuration) {
        verify_parallel(core, move, configuration);
      }
    }
  };
  std::ranges::for_each(moves, exercise);
  std::ranges::for_each(px_alias_moves, exercise);
}

constexpr std::uint32_t immediate_test_word = 0x89abcdef;

void apply_immediate(sharc::Registers &state, unsigned reg) {
  if (reg == px) {
    constexpr std::uint64_t duplicated = 0x89abcdef89abcdef;
    state.px = duplicated;
    return;
  }
  constexpr auto raw = std::uint64_t{immediate_test_word} << 8;
  write(state, reg, false, raw);
  if (has_peer(reg) && (state.mode1 & (1U << simd_bit)) != 0) { write(state, reg, true, raw); }
}

void verify_immediates(const std::vector<unsigned> &registers) {
  std::vector<std::uint64_t> words;
  words.reserve(registers.size());
  for (const auto reg : registers) {
    words.push_back(fixture::immediate(reg, immediate_test_word));
  }
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    for (unsigned configuration = 0; configuration < 512; ++configuration) {
      auto expected = initial(configuration);
      core.set_registers(expected);
      const auto saved = core.save();
      for (unsigned count = 1; count <= registers.size(); ++count) {
        apply_immediate(expected, registers[count - 1]);
        advance(expected);
        core.restore(saved);
        const auto result = core.run(count);
        check(fixture::completed(result, count, count) && core.registers() == expected,
              "universal-register immediate lost its width, bank or prefix");
      }
    }
  }
}

void immediate_registers(unsigned quarter) {
  std::vector<unsigned> registers;
  constexpr unsigned registers_per_quarter = 20;
  registers.reserve(registers_per_quarter);
  for (unsigned index = quarter * 4; index < quarter * 4 + 4; ++index) {
    for (unsigned field = 1; field <= 5; ++field) { registers.push_back((field * 16) + index); }
  }
  verify_immediates(registers);
}

void system_immediates() {
  verify_immediates({ustat[0],
                     ustat[1],
                     ustat[2],
                     ustat[3],
                     px1,
                     px2,
                     px,
                     lcntr_code,
                     astat_x_code,
                     astat_y_code,
                     stky_x_code,
                     stky_y_code});
}

enum class UserBitAction : std::uint8_t { set, clear, toggle, test, compare };

struct UserBit {
  UserBitAction action;
  unsigned port;
  std::uint32_t mask;
};

constexpr std::uint32_t btf = 1U << 18;
constexpr unsigned tf_condition = 13, not_tf_condition = 29;

std::uint64_t bit_word(UserBitAction action, unsigned reg, std::uint32_t mask) {
  constexpr std::uint64_t prefix = 0x140000000000;
  constexpr unsigned operation_shift = 37;
  constexpr unsigned register_shift = 32;
  constexpr std::array operations{0U, 1U, 2U, 4U, 5U};
  return prefix | (std::uint64_t{operations[static_cast<unsigned>(action)]} << operation_shift) |
         (std::uint64_t{reg & 15} << register_shift) | mask;
}

std::uint64_t bit_word(UserBit bit) {
  return bit_word(bit.action, ustat[bit.port], bit.mask);
}

void sticky_bit_writes() {
  using enum UserBitAction;
  constexpr std::uint32_t all = 0xffffffff;
  constexpr std::uint32_t overflow = 4;
  constexpr std::uint32_t x_stack = (1U << 22) | (1U << 26);
  constexpr std::uint32_t x_writable = 0x001e03e7;
  constexpr std::uint32_t y_writable = 0x000003e7;
  constexpr std::uint32_t y_initial = 0x20;
  const std::array words{bit_word(set, stky_x_code, all),
                         encoding({.source = stky_x_code, .destination = 4}, true_condition),
                         bit_word(toggle, stky_y_code, overflow),
                         bit_word(test, stky_x_code, overflow),
                         encoding({.source = 5, .destination = 6}, tf_condition),
                         bit_word(clear, stky_x_code, all)};
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    for (const bool simd : {false, true}) {
      auto expected = initial(unsigned{simd} << 2);
      expected.pe[0].stky = x_stack;
      expected.pe[1].stky = y_initial;
      core.set_registers(expected);
      expected.pe[0].data[0][4] = std::uint64_t{x_stack | x_writable} << 8;
      if (simd) {
        expected.pe[1].data[0][4] = std::uint64_t{y_writable} << 8;
        expected.pe[1].stky = 0;
      } else {
        expected.pe[0].astat = btf;
        expected.pe[0].data[0][6] = expected.pe[0].data[0][5];
        expected.pe[1].stky |= overflow;
      }
      fixture::boundary(expected,
                        {.pc = base + static_cast<std::uint32_t>(words.size()),
                         .clock = clock_seed + words.size()});
      check(fixture::completed(core.run(words.size()), words.size(), words.size()) &&
                core.registers() == expected,
            "STKY bit operations lost a stack flag, lane, or immediate test result");
    }
  }
}

void astat_effect_sequence(sharc::ExecutionMode mode, bool raw_read, bool old_zero) {
  constexpr std::uint32_t fifo_status = 1U << 14;
  const auto new_status = unsigned{!old_zero};
  constexpr auto conditional = Move{.source = 5, .destination = 4};
  const auto next = raw_read ? encoding({.source = astat_x_code, .destination = 4}, true_condition)
                             : encoding(conditional);
  const std::array words{fixture::immediate(astat_x_code, new_status),
                         next,
                         encoding({.source = 5, .destination = 6}),
                         encoding({.source = 5, .destination = 7})};
  fixture::Program program(words);
  const auto native = fixture::compile(program.code, mode);
  sharc::Core core(program.memory(), mode);
  sharc::Core replay(program.memory(), mode);
  auto before = initial(unsigned{old_zero});
  before.pe[0].astat |= fifo_status;
  fixture::install(core, native, before);
  (void)replay.install(native, fixture::Program::variant_id);
  const auto beginning = core.save();
  check(fixture::completed(core.run(1), 1, 1), "explicit ASTAT write did not retire");
  check(core.registers().pe[0].astat == (fifo_status | new_status), "ASTAT write lost SF or AZ");
  replay.restore(core.save());
  const auto cycles = 4U;
  check(fixture::completed(replay.run(3), 3, cycles), "ASTAT replay lost its pending timing");
  auto expected = before;
  const auto source = before.pe[0].data[0][5];
  expected.pe[0].astat = fifo_status | new_status;
  if (raw_read) {
    expected.pe[0].data[0][4] = std::uint64_t{expected.pe[0].astat} << 8;
  } else if (old_zero) {
    expected.pe[0].data[0][4] = source;
  }
  if (!old_zero) {
    expected.pe[0].data[0][6] = source;
    expected.pe[0].data[0][7] = source;
  }
  fixture::boundary(expected, {.pc = base + 4, .clock = clock_seed + cycles + 1});
  check(replay.registers() == expected, "ASTAT raw read and delayed predicate used the same flags");
  core.restore(beginning);
  check(fixture::completed(core.run(words.size()), words.size(), cycles + 1) &&
            core.registers() == expected,
        "ASTAT grant changed the architectural result or lost an interlock");
  core.restore(beginning);
  const auto now = core.state().progress.base_cycles;
  check(fixture::completed(core.run_until(now + 3), 3, 4) && core.registers().pc == base + 3 &&
            fixture::completed(core.run_until(now + 5), 1, 1) && core.registers() == expected,
        "ASTAT cycle deadline did not retain the interlocked prefix");
}

enum class AstatBranchWriter : std::uint8_t { plain, parallel, late, masked };

void astat_effect_branches(sharc::ExecutionMode mode, AstatBranchWriter writer) {
  constexpr unsigned source = 7;
  constexpr std::uint64_t add_r0_r2_r3 = 0x1023;
  const bool parallel = writer == AstatBranchWriter::parallel;
  const bool masked = writer == AstatBranchWriter::masked;
  const bool intervening_nop = masked || writer == AstatBranchWriter::late;
  const auto move = Move{.source = source, .destination = astat_x_code};
  const auto write = encoding(move, masked ? 0U : true_condition);
  constexpr auto target = base + 4;
  constexpr std::uint64_t jump_eq = 0x060000000000 | target;
  constexpr std::uint64_t nop = 0;
  const std::array words{write | (parallel ? add_r0_r2_r3 : 0),
                         intervening_nop ? nop : jump_eq,
                         intervening_nop ? jump_eq : nop,
                         nop,
                         nop};
  fixture::Program program(words);
  const auto native = fixture::compile(program.code, mode);
  sharc::Core core(program.memory(), mode);
  auto before = initial(0);
  before.pe[0].data[0][source] = 1U << 8;
  fixture::install(core, native, before);
  const auto instructions = intervening_nop ? 3U : 2U;
  const bool taken = parallel || writer == AstatBranchWriter::late;
  const auto cycles = taken ? 7U : instructions;
  const auto successor = taken ? target : base + instructions;
  check(fixture::completed(core.run(instructions), instructions, cycles) &&
            core.registers().pc == successor,
        "adjacent branch lost the ASTAT predicate age or parallel-compute interlock");
}

void astat_carry(sharc::ExecutionMode mode) {
  constexpr unsigned carry = 8;
  constexpr std::uint64_t add_with_carry = fixture::add(4, 1, 2) | (1ULL << 14);
  const std::array words{fixture::immediate(astat_x_code, carry), add_with_carry};
  fixture::Program program(words);
  const auto native = fixture::compile(program.code, mode);
  sharc::Core core(program.memory(), mode);
  sharc::Registers before;
  before.pc = base;
  before.pe[0].data[0][1] = 1U << 8;
  before.pe[0].data[0][2] = 2U << 8;
  fixture::install(core, native, before);
  check(fixture::completed(core.run(2), 2, 2) && core.registers().pe[0].data[0][4] == (4U << 8),
        "ADDC consumed the delayed predicate sample instead of the new carry operand");
}

void astat_effects() {
  for (const auto mode : fixture::numeric_modes) {
    for (const bool raw_read : {false, true}) {
      for (const bool old_zero : {false, true}) { astat_effect_sequence(mode, raw_read, old_zero); }
    }
    using enum AstatBranchWriter;
    for (const auto writer : {plain, parallel, late, masked}) {
      astat_effect_branches(mode, writer);
    }
    astat_carry(mode);
    const std::array words{bit_word(UserBitAction::toggle, astat_x_code, 1),
                           encoding({.source = 5, .destination = 4}),
                           encoding({.source = 5, .destination = 6})};
    fixture::Program program(words);
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    auto before = initial(4);
    before.pe[1].astat = 1;
    fixture::install(core, native, before);
    check(fixture::completed(core.run(3), 3, 4) &&
              core.registers().pe[0].data[0][6] == before.pe[0].data[0][5] &&
              core.registers().pe[1].data[0][4] == before.pe[1].data[0][5] &&
              core.registers().pe[0].data[0][4] == before.pe[0].data[0][4] &&
              core.registers().pe[1].data[0][6] == before.pe[1].data[0][6],
          "ASTAT bit toggle lost independent old and new SIMD predicates");
  }
}

void expected_user_bits(sharc::Registers &state, UserBit bit, unsigned lanes) {
  using enum UserBitAction;
  for (unsigned lane = 0; lane < lanes; ++lane) {
    auto &word = state.ustat[bit.port ^ lane];
    auto &astat = state.pe[lane].astat;
    switch (bit.action) {
      case set: word |= bit.mask; break;
      case clear: word &= ~bit.mask; break;
      case toggle: word ^= bit.mask; break;
      case test: astat = (astat & ~btf) | ((bit.mask & ~word) == 0 ? btf : 0); break;
      case compare: astat = (astat & ~btf) | (word == bit.mask ? btf : 0); break;
    }
  }
}

void user_status_bits() {
  using enum UserBitAction;
  constexpr std::array<std::uint32_t, 4> masks{0, 0xf1000000, 0xf1946aef, 0xffffffff};
  constexpr std::uint32_t old_status = 0xa3000040;
  for (unsigned port = 0; port < ustat.size(); ++port) {
    std::vector<UserBit> operations;
    std::vector<std::uint64_t> words;
    for (const auto action : {test, compare, set, clear, toggle}) {
      for (const auto mask : masks) {
        operations.push_back({.action = action, .port = port, .mask = mask});
        words.push_back(bit_word(operations.back()));
      }
    }
    fixture::Program program(words);
    for (const auto mode : fixture::numeric_modes) {
      const auto native = fixture::compile(program.code, mode);
      sharc::Core core(program.memory(), mode);
      (void)core.install(native, fixture::Program::variant_id);
      for (unsigned configuration = 0; configuration < 8; ++configuration) {
        auto expected = initial(configuration);
        for (unsigned lane = 0; lane < 2; ++lane) {
          expected.pe[lane].astat |= old_status | (((configuration >> lane) & 1) * btf);
        }
        core.set_registers(expected);
        const auto beginning = core.save();
        const auto lanes = (configuration & 4) != 0 ? 2U : 1U;
        for (unsigned count = 1; count <= words.size(); ++count) {
          expected_user_bits(expected, operations[count - 1], lanes);
          advance(expected);
          core.restore(beginning);
          check(fixture::completed(core.run(count), count, count) && core.registers() == expected,
                "USTAT bit operation changed the wrong mask, lane or architectural flag");
        }
      }
    }
  }
}

void expected_control_bits(sharc::Registers &state,
                           unsigned reg,
                           UserBitAction action,
                           std::uint32_t mask) {
  const auto before = state;
  const auto lanes = (state.mode1 & (1U << simd_bit)) != 0 ? 2U : 1U;
  for (unsigned lane = 0; lane < lanes; ++lane) {
    const auto word = static_cast<std::uint32_t>(read(before, reg, lane != 0) >> 8);
    const auto tested = action == UserBitAction::test ? word & mask : word;
    auto &astat = state.pe[lane].astat;
    astat = (astat & ~btf) | (tested == mask ? btf : 0);
  }
  advance(state);
}

struct ControlBitSource {
  unsigned reg;
  std::uint32_t compare_value;
};

void control_bit_reads(ControlBitSource source) {
  using enum UserBitAction;
  constexpr std::uint32_t status_x = 0xa5000040;
  constexpr std::uint32_t status_y = 0xd2000020;
  const std::array operations{test, compare, test, compare, test};
  const std::array masks{0U, source.compare_value, btf, status_x | btf, 0x40U};
  std::array<std::uint64_t, operations.size()> words{};
  for (std::size_t i = 0; i < words.size(); ++i) {
    words[i] = bit_word(operations[i], source.reg, masks[i]);
  }
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    for (unsigned configuration = 0; configuration < 8; ++configuration) {
      auto expected = initial(configuration);
      for (const auto &control : scalar_controls) { expected.*control.member |= control.seed; }
      expected.pe[0].astat = status_x | ((configuration & 1) * btf);
      expected.pe[1].astat = status_y | (((configuration >> 1) & 1) * btf);
      core.set_registers(expected);
      const auto saved = core.save();
      for (std::size_t i = 0; i < words.size(); ++i) {
        expected_control_bits(expected, source.reg, operations[i], masks[i]);
        core.restore(saved);
        const auto count = i + 1;
        check(fixture::completed(core.run(count), count, count) && core.registers() == expected,
              "control-register bit test lost the named source, peer, or old ASTAT");
      }
    }
  }
}

void control_bit_reads() {
  for (const auto &control : scalar_controls) {
    if (control.code >= mode1_code) {
      control_bit_reads({.reg = control.code, .compare_value = control.seed});
    }
  }
  for (const auto reg : lane_status_codes) {
    control_bit_reads({.reg = reg, .compare_value = old_sticky});
  }
}

void bit_flag_moves(sharc::ExecutionMode mode) {
  constexpr auto test = UserBit{.action = UserBitAction::test, .port = 3, .mask = 1};
  constexpr unsigned r4 = 4;
  constexpr unsigned r5 = 5;
  constexpr unsigned r6 = 6;
  constexpr unsigned r7 = 7;
  const std::array words{bit_word(test),
                         encoding({.source = r5, .destination = r4}, tf_condition),
                         encoding({.source = r7, .destination = r6}, not_tf_condition)};
  fixture::Program program(words);
  const auto native = fixture::compile(program.code, mode);
  sharc::Core core(program.memory(), mode);
  (void)core.install(native, fixture::Program::variant_id);
  for (unsigned configuration = 0; configuration < 8; ++configuration) {
    auto expected = initial(configuration);
    for (unsigned lane = 0; lane < 2; ++lane) {
      expected.ustat[test.port ^ lane] = (configuration >> lane) & 1;
    }
    core.set_registers(expected);
    const unsigned lanes = (configuration & 4) != 0 ? 2 : 1;
    expected_user_bits(expected, test, lanes);
    for (unsigned lane = 0; lane < lanes; ++lane) {
      const bool matched = (configuration & (1U << lane)) != 0;
      auto &data = expected.pe[lane].data[0];
      data[matched ? r4 : r6] = data[matched ? r5 : r7];
    }
    fixture::boundary(expected, {.pc = base + 3, .clock = clock_seed + 3});
    check(fixture::completed(core.run(3), 3, 3) && core.registers() == expected,
          "TF or NOT TF failed to select the corresponding USTAT test lane");
  }
}

std::uint64_t bit_branch_word(unsigned condition, std::uint32_t target) {
  constexpr std::uint64_t jump = 0x060000000000;
  return jump | (std::uint64_t{condition} << condition_shift) | target;
}

struct BitBranch {
  UserBitAction action;
  unsigned condition, gap{};
  bool interlock{};
};

void bit_flag_branch(sharc::ExecutionMode mode, BitBranch branch) {
  const UserBit op{.action = branch.action, .port = 0, .mask = 1};
  constexpr auto target = base + 4;
  std::array<std::uint64_t, 5> words{bit_word(op), 0, 0, 0, 0};
  words[branch.gap + 1] = bit_branch_word(branch.condition, target);
  fixture::Program program(words);
  const auto native = fixture::compile(program.code, mode);
  sharc::Core core(program.memory(), mode);
  sharc::Core resumed(program.memory(), mode);
  (void)core.install(native, fixture::Program::variant_id);
  (void)resumed.install(native, fixture::Program::variant_id);
  for (unsigned configuration = 0; configuration < 8; ++configuration) {
    auto expected = initial(configuration);
    const unsigned lanes = (configuration & 4) != 0 ? 2 : 1;
    for (unsigned lane = 0; lane < 2; ++lane) {
      const auto matched = (configuration >> lane) & 1;
      expected.ustat[lane] = matched;
      expected.pe[lane].astat = matched | (matched * btf);
    }
    core.set_registers(expected);
    const auto beginning = core.save();
    expected_user_bits(expected, op, lanes);
    advance(expected);
    check(fixture::completed(core.run(1), 1, 1) && core.registers() == expected,
          "USTAT bit operation retired at the wrong boundary");
    resumed.restore(core.save());
    const auto mask = (1U << lanes) - 1;
    const auto taken = (configuration & mask) == (branch.condition == not_tf_condition ? 0 : mask);
    const auto cycles = 2U + branch.gap + unsigned{branch.interlock} + (taken ? 3U : 0U);
    const auto count = 2U + branch.gap;
    fixture::boundary(expected,
                      {.pc = taken ? target : base + count, .clock = clock_seed + cycles});
    const auto deadline = resumed.state().progress.base_cycles + count - 1;
    check(fixture::completed(resumed.run_until(deadline), count - 1, cycles - 1) &&
              resumed.registers() == expected,
          "a cross-Core snapshot lost the TF branch decision or pending interlock");
    core.restore(beginning);
    check(fixture::completed(core.run(count), count, cycles) && core.registers() == expected,
          "the native grant changed the condition-specific bit-test branch timing");
  }
}

void bit_flag_loop(sharc::ExecutionMode mode) {
  constexpr std::uint64_t jump_back_if_tf = 0x071a00fffffe;
  const std::array words{bit_word({.action = UserBitAction::toggle, .port = 0, .mask = 1}),
                         bit_word({.action = UserBitAction::test, .port = 0, .mask = 1}),
                         jump_back_if_tf,
                         std::uint64_t{0}};
  fixture::Program program(words);
  const auto native = fixture::compile(program.code, mode);
  sharc::Core core(program.memory(), mode);
  auto expected = initial(0);
  expected.ustat[0] = 0;
  fixture::install(core, native, expected);
  constexpr unsigned retirements = 7;
  constexpr unsigned cycles = 12;
  fixture::boundary(expected, {.pc = base + 4, .clock = clock_seed + cycles});
  check(fixture::completed(core.run(retirements), retirements, cycles) &&
            core.registers() == expected,
        "batched bit-test loop lost an interlock or the terminating pass");
}

void bit_flag_consumers() {
  using enum UserBitAction;
  constexpr std::array cases{
      BitBranch{.action = test, .condition = tf_condition, .interlock = true},
      BitBranch{.action = test, .condition = not_tf_condition, .interlock = true},
      BitBranch{.action = compare, .condition = tf_condition, .interlock = true},
      BitBranch{.action = test, .condition = tf_condition, .gap = 1},
      BitBranch{.action = test, .condition = 0},
      BitBranch{.action = set, .condition = tf_condition}};
  for (const auto mode : fixture::numeric_modes) {
    bit_flag_moves(mode);
    for (const auto branch : cases) { bit_flag_branch(mode, branch); }
    bit_flag_loop(mode);
  }
}

void user_status_bit_refusals() {
  constexpr std::array reserved{
      0x146000000001ULL, 0x14c000000001ULL, 0x14e000000001ULL, 0x141000000001ULL};
  for (const auto word : reserved) {
    const auto fault = fixture::refusal(fixture::image(sharc::InstructionSet::isa, word),
                                        "reserved Type 18a field was accepted");
    check(fault.reason == sharc::FaultReason::invalid_encoding && fault.instruction == word,
          "reserved Type 18a field lost its encoding refusal");
  }
  constexpr std::uint64_t mode1_bit_set = 0x140200000001;
  const auto fault = fixture::refusal(fixture::image(sharc::InstructionSet::isa, mode1_bit_set),
                                      "unimplemented mode-register side effects were admitted");
  check(fault.reason == sharc::FaultReason::unsupported_instruction,
        "a legal system bit field must be distinguished from a reserved encoding");
}

constexpr std::uint32_t dependency_ram = 0x90000, dependency_ram_id = 11;
constexpr std::uint32_t dependency_word = 0x89abcdef;

struct DependencyCase {
  unsigned destination{}, gap{};
  bool memory{}, selected{true}, parallel{}, combined_px{};
};

std::uint32_t dependency_value(DependencyCase input) {
  if ((input.destination >> 4) != 1) { return 0; }
  return input.memory ? dependency_ram : base + 5;
}

std::vector<sharc::Registers> dependency_states(DependencyCase input) {
  const auto value = dependency_value(input);
  auto expected = initial(0);
  expected.dag[0][0].index = dependency_ram;
  expected.dag[0][8].index = base + 5;
  expected.dag[0][8].modifier = 0;
  expected.pe[0].data[0][0] = (std::uint64_t{value} << 8) | extension;
  if (input.combined_px) {
    constexpr std::uint64_t unrelated_px2 = 0xabcdefff00000000;
    expected.dag[0][8].index = base + 4;
    expected.px = unrelated_px2 | value;
  }
  expected.pe[0].data[0][2] = 0;
  expected.pe[0].data[0][3] = 0;
  std::vector states{expected};
  if (input.selected) { dag(expected, input.destination) = value; }
  if (input.parallel) {
    expected.pe[0].data[0][1] = 0;
    expected.pe[0].astat = 1;
  }
  advance(expected);
  states.push_back(expected);
  for (unsigned i = 0; i < input.gap; ++i) {
    advance(expected);
    states.push_back(expected);
  }
  const bool matching = (input.destination & 15) == (input.memory ? 0U : 8U) &&
                        (!input.memory || (input.destination >> 4) != 2);
  const auto load_stall = input.selected && matching ? 2 - input.gap : 0U;
  const auto stall = std::max(load_stall, unsigned{input.parallel && input.gap == 0});
  advance(expected);
  expected.emuclk += stall + (input.memory ? 0 : 3);
  if (input.memory) {
    expected.pe[0].data[0][1] = std::uint64_t{dependency_word} << 8;
  } else {
    expected.pc = base + 5;
    fixture::fetch_addresses(expected);
  }
  states.push_back(expected);
  advance(expected);
  states.push_back(expected);
  return states;
}

void exercise_dependency(sharc::Core &core,
                         const std::vector<sharc::Registers> &states,
                         sharc::Core &resumed) {
  core.set_registers(states.front());
  const auto saved = core.save();
  const auto total = static_cast<unsigned>(states.size() - 1);
  for (unsigned count = 1; count <= total; ++count) {
    core.restore(saved);
    const auto result = core.run(count);
    const auto cycles = states[count].emuclk - clock_seed;
    check(fixture::completed(result, count, cycles) && core.registers() == states[count],
          "DAG load dependency changed the wrong prefix, value or cycle count");
    resumed.restore(core.save());
    if (count < total) {
      const auto tail = resumed.run(total - count);
      check(!tail.has_fault && resumed.registers() == states.back(),
            "DAG dependency history did not survive a cross-Core snapshot");
    }
  }
  for (std::uint64_t deadline = 1; deadline <= states.back().emuclk - clock_seed; ++deadline) {
    core.restore(saved);
    unsigned count = 1;
    while (states[count].emuclk - clock_seed < deadline) { ++count; }
    const auto result = core.run_until(deadline);
    check(!result.has_fault && core.registers() == states[count],
          "DAG stall crossed the wrong deadline boundary");
  }
}

void dependency_case(DependencyCase input) {
  constexpr std::uint64_t jump_m8_i8 = 0x083e00000000;
  constexpr std::uint64_t load_r1_i0 = 0xa00100000000;
  constexpr std::uint64_t add_r1_r2_r3 = 0x1123;
  const Move move{.source = input.combined_px ? px : 0, .destination = input.destination};
  const auto value = dependency_value(input);
  const auto loader =
      (input.gap & 1) != 0 && !input.parallel && input.selected && !input.combined_px
          ? fixture::immediate(input.destination, value)
          : encoding(move, input.selected ? 31U : 0U) | (input.parallel ? add_r1_r2_r3 : 0);
  std::vector<std::uint64_t> words{loader};
  words.insert(words.end(), input.gap, 0);
  words.push_back(
      input.memory
          ? load_r1_i0
          : jump_m8_i8 ^ (input.parallel ? std::uint64_t{true_condition} << condition_shift : 0));
  if (input.memory) {
    words.push_back(0);
  } else {
    words.resize(6);
  }
  fixture::Program program(words);
  if (!input.memory) { program.code.variants[0].entries.push_back(base + 5); }
  std::array<std::byte, 4> ram{};
  for (unsigned byte = 0; byte < ram.size(); ++byte) {
    ram[byte] = static_cast<std::byte>(dependency_word >> (byte * 8));
  }
  if (input.memory) {
    auto region = fixture::normal_region(dependency_ram, 1, dependency_ram_id);
    region.writable = false;
    program.code.variants[0].layout.regions.push_back(region);
  }
  auto memory = program.memory();
  if (input.memory) {
    memory.regions.push_back({.storage_id = dependency_ram_id, .bytes = ram, .generation = 1});
  }
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(memory, mode);
    sharc::Core resumed(memory, mode);
    const auto states = dependency_states(input);
    (void)core.install(native, fixture::Program::variant_id);
    (void)resumed.install(native, fixture::Program::variant_id);
    exercise_dependency(core, states, resumed);
  }
}

void two_pending_loads() {
  constexpr std::uint64_t jump_m8_i8 = 0x083e00000000;
  constexpr unsigned m8_code = 0x28;
  constexpr unsigned m9_code = 0x29;
  constexpr unsigned i9_code = 0x19;
  for (const bool matching : {false, true}) {
    const auto destination = matching ? m8_code : m9_code;
    const std::array words{
        encoding({.source = 0, .destination = i8_code}, true_condition),
        encoding({.source = i9_code, .destination = destination}, true_condition),
        jump_m8_i8,
        std::uint64_t{0},
        std::uint64_t{0},
        std::uint64_t{0}};
    fixture::Program program(words);
    program.code.variants[0].entries.push_back(base + 5);
    for (const auto mode : fixture::numeric_modes) {
      const auto native = fixture::compile(program.code, mode);
      sharc::Core core(program.memory(), mode);
      sharc::Core resumed(program.memory(), mode);
      (void)core.install(native, fixture::Program::variant_id);
      (void)resumed.install(native, fixture::Program::variant_id);
      auto expected = initial(0);
      expected.pe[0].data[0][0] = std::uint64_t{base + 5} << 8;
      expected.dag[0][8].modifier = 0;
      expected.dag[0][9].index = 0;
      std::vector states{expected};
      expected.dag[0][8].index = base + 5;
      advance(expected);
      states.push_back(expected);
      dag(expected, destination) = 0;
      advance(expected);
      states.push_back(expected);
      advance(expected);
      expected.emuclk += matching ? 5 : 4;
      expected.pc = base + 5;
      fixture::fetch_addresses(expected);
      states.push_back(expected);
      advance(expected);
      states.push_back(expected);
      exercise_dependency(core, states, resumed);
    }
  }
}

void refused_register_fields() {
  constexpr std::array reserved{42U, 32U, 31U, 30U, 29U, 28U, 27U};
  for (const auto bit : reserved) {
    const auto word = encoding({.source = shadow_first, .destination = 0, .swap = true}) |
                      (std::uint64_t{1} << bit);
    const auto fault = fixture::refusal(fixture::image(sharc::InstructionSet::isa, word),
                                        "reserved raw40-swap field was accepted");
    check(fault.reason == sharc::FaultReason::invalid_encoding,
          "reserved raw40-swap field lost its encoding refusal");
  }
  fixture::throws<sharc::PreparationError>(
      [&] {
        (void)sharc::Core::lower(fixture::image(
            sharc::InstructionSet::isa, encoding({.source = 0, .destination = mode1_code})));
      },
      "unimplemented system-register write was accepted");
}

void dependencies() {
  constexpr std::array memory_registers{0x10U, 0x20U, 0x30U, 0x40U, 0x11U};
  constexpr std::array branch_registers{0x18U, 0x28U, 0x38U, 0x48U, 0x19U, 0x29U};
  for (const auto reg : memory_registers) {
    for (unsigned gap = 0; gap <= 2; ++gap) {
      dependency_case({.destination = reg, .gap = gap, .memory = true});
    }
  }
  for (const auto reg : branch_registers) {
    for (unsigned gap = 0; gap <= 2; ++gap) { dependency_case({.destination = reg, .gap = gap}); }
  }
  dependency_case({.destination = i8_code, .selected = false});
  dependency_case({.destination = i8_code, .parallel = true});
  dependency_case({.destination = i8_code, .combined_px = true});
}

} // namespace

namespace fixture {
void register_transfers() {
  raw_moves();
  system_moves();
  control_reads();
  loop_preload_moves();
  sticky_status_moves();
  read_latched_dag_overflow();
  emulation_clock_wrap();
  emulation_clock_short_loop();
  sequencer_register_reads();
  pc_stack_writes();
  loop_address_writes();
  loop_address_edits();
  pc_stack_pointer_latency();
  for (unsigned gap = 0; gap < 3; ++gap) { pc_stack_pointer_rts(gap); }
  pc_stack_pointer_in_delayed_return();
  pc_stack_pointer_before_do();
  rewrite_delayed_return(true);
  rewrite_delayed_return(false);
  refuse_unimplemented_loop_stack_write(pcstk_code);
  refuse_unimplemented_loop_stack_write(pcstkp_code);
  refuse_unimplemented_loop_stack_write(laddr_code);
  pc_and_stack_in_delay_slots();
  loop_register_reads();
  loop_counter_writes();
  loop_counter_write_after_nested_pop();
  refuse_buffered_loop_counter_write();
  combined_px_moves();
  combined_px_aliases();
  parallel_priority();
  for (unsigned quarter = 0; quarter < 4; ++quarter) { immediate_registers(quarter); }
  system_immediates();
  user_status_bits();
  sticky_bit_writes();
  astat_effects();
  control_bit_reads();
  bit_flag_consumers();
  user_status_bit_refusals();
  dependencies();
  two_pending_loads();
  refused_register_fields();
}
} // namespace fixture
