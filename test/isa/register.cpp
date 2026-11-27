// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

bool has_peer(unsigned reg) {
  return is_data(reg) || reg == px1 || reg == px2 || std::ranges::contains(ustat, reg);
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

std::uint64_t read(const sharc::Registers &state, unsigned reg, bool implicit) {
  if (reg == px) {
    constexpr unsigned register_alignment = 24;
    return state.px >> register_alignment;
  }
  if (reg == px1 || reg == px2) {
    constexpr unsigned word_bits = 32;
    const auto shift = ((reg == px2) != implicit) ? word_bits : 0U;
    return std::uint64_t{static_cast<std::uint32_t>(state.px >> shift)} << 8;
  }
  const auto peer = implicit ? reg ^ 1 : reg;
  const auto *const status = std::ranges::find(ustat, peer);
  if (status != ustat.end()) {
    return std::uint64_t{state.ustat[static_cast<std::size_t>(status - ustat.begin())]} << 8;
  }
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
  if (reg == px1 || reg == px2) {
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
  const bool x = (before.pe[0].astat & 1) != 0;
  const bool y = (before.pe[1].astat & 1) != 0;
  const bool simd = (before.mode1 & (1U << simd_bit)) != 0;
  if (move.source == px && !is_data(move.destination)) {
    if (x) {
      const auto low = static_cast<std::uint32_t>(before.px);
      write(state, move.destination, false, std::uint64_t{low} << 8);
    }
    return;
  }
  if (move.destination == px && !is_data(move.source)) {
    if (x) {
      constexpr std::uint64_t half_radix = 0x100000000;
      const auto *const reg = std::ranges::find(ustat, move.source);
      if (reg == ustat.end()) {
        state.px = (half_radix + 1) * dag(before, move.source);
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
                             Move{.source = 0, .destination = px2},
                             Move{.source = px, .destination = ustat[0]},
                             Move{.source = ustat[3], .destination = px},
                             Move{.source = 0x50, .destination = 0, .swap = true}};
  for (const auto &move : moves) {
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
  }
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
  verify_immediates({ustat[0], ustat[1], ustat[2], ustat[3], px1, px2, px});
}

enum class UserBitAction : std::uint8_t { set, clear, toggle, test, compare };

struct UserBit {
  UserBitAction action;
  unsigned port;
  std::uint32_t mask;
};

constexpr std::uint32_t btf = 1U << 18;
constexpr unsigned tf_condition = 13, not_tf_condition = 29;

std::uint64_t bit_word(UserBit bit) {
  constexpr std::uint64_t prefix = 0x140000000000;
  constexpr unsigned operation_shift = 37;
  constexpr unsigned register_shift = 32;
  constexpr std::array operations{0U, 1U, 2U, 4U, 5U};
  constexpr std::array ports{0U, 1U, 14U, 15U};
  return prefix |
         (std::uint64_t{operations[static_cast<unsigned>(bit.action)]} << operation_shift) |
         (std::uint64_t{ports[bit.port]} << register_shift) | bit.mask;
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
  constexpr unsigned mode1_code = 0x72;
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
            sharc::InstructionSet::isa, encoding({.source = mode1_code, .destination = 0})));
      },
      "unimplemented system-register read was accepted");
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
  combined_px_moves();
  parallel_priority();
  for (unsigned quarter = 0; quarter < 4; ++quarter) { immediate_registers(quarter); }
  system_immediates();
  user_status_bits();
  bit_flag_consumers();
  user_status_bit_refusals();
  dependencies();
  two_pending_loads();
  refused_register_fields();
}
} // namespace fixture
