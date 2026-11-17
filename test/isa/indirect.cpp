// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <array>
#include <cstdint>
#include <sharc/core.hpp>
#include <vector>

namespace {
using fixture::check;
constexpr auto base = fixture::code_address;
constexpr std::uint32_t address_mask = 0xffffff, call_tag = 1U << 24, stack_empty = 1U << 22;
constexpr std::uint64_t raw_seed = 0x81234567ef;
constexpr unsigned condition_shift = 33, index_shift = 30, modifier_shift = 27, simd_bit = 21;
constexpr unsigned displacement_mask = 63;
constexpr std::uint64_t relative_opcode = 0x090000000000, initial_clock = 123;
constexpr std::uint32_t old_sticky = 0x80000400;
constexpr std::uint32_t index_seed = 0x10203040, modifier_seed = 0x23456789, buffer_base = 0x5555;
constexpr std::uint32_t near_wrap = 0xfffffff0;
constexpr std::array modes{sharc::ExecutionMode::strict, sharc::ExecutionMode::fast_fp32};

std::uint64_t indirect(unsigned index, unsigned modifier, unsigned condition = 31) {
  return 0x080000000000 | (std::uint64_t{condition} << condition_shift) |
         (std::uint64_t{index - 8} << index_shift) |
         (std::uint64_t{modifier - 8} << modifier_shift);
}

std::uint64_t relative(int displacement, unsigned condition = 31) {
  return relative_opcode | (std::uint64_t{condition} << condition_shift) |
         (std::uint64_t{static_cast<unsigned>(displacement) & displacement_mask} << modifier_shift);
}

sharc::Registers initial() {
  sharc::Registers result;
  result.pc = base;
  result.emuclk = initial_clock;
  for (auto &pe : result.pe) {
    for (auto &bank : pe.data) { bank.fill(raw_seed); }
    pe.stky = old_sticky;
    pe.data[0][1] = 3U << 8;
    pe.data[0][2] = 4U << 8;
  }
  return result;
}

struct Boundary {
  std::uint32_t pc;
  unsigned cycles;
};

void advance(sharc::Registers &state, Boundary boundary) {
  const auto [pc, cycles] = boundary;
  state.pc = pc;
  state.faddr = (pc + 4) & address_mask;
  state.daddr = (pc + 2) & address_mask;
  state.emuclk += cycles;
}

struct Selector {
  unsigned row, banks;
};

void verify_selector(sharc::Core &core, Selector selector) {
  const auto [row, banks] = selector;
  constexpr std::uint32_t target = base + 16;
  const auto index = 8 + (row & 7);
  const auto modifier = 8 + ((row + (row < 8 ? 0 : 4)) & 7);
  auto expected = initial();
  expected.pc = base + row;
  expected.mode1 = ((banks & 1) << 6) | (((banks >> 1) & 1) << 5);
  for (unsigned bank = 0; bank < 2; ++bank) {
    for (unsigned i = 0; i < 16; ++i) {
      expected.dag[bank][i] = {.index = index_seed + bank + i,
                               .modifier = modifier_seed + bank + i,
                               .length = 13,
                               .base = buffer_base};
    }
  }
  const auto index_bank = (banks >> (index < 12 ? 0 : 1)) & 1;
  const auto modifier_bank = (banks >> (modifier < 12 ? 0 : 1)) & 1;
  expected.dag[index_bank][index].index = near_wrap;
  expected.dag[modifier_bank][modifier].modifier = target + 16;
  core.set_registers(expected);
  const auto saved = core.save();
  advance(expected, {.pc = target, .cycles = 4});
  for (const bool deadline : {false, true}) {
    core.restore(saved);
    const auto result =
        deadline ? core.run_until(core.state().progress.base_cycles + 1) : core.run(1);
    check(!result.has_fault &&
              result.progress == sharc::Progress{.instructions = 1, .base_cycles = 4} &&
              core.registers() == expected,
          "indirect selector, bank, wrapping, pre-modify or unchanged DAG state differs");
  }
}

void selectors() {
  // Every Ic/Md selector, with equal and opposite DAG quarters. The independent
  // bank matrix makes an incorrect shared I/M bank produce a different target.
  std::vector<std::uint64_t> words;
  for (const unsigned offset : {0U, 4U}) {
    for (unsigned index = 8; index < 16; ++index) {
      words.push_back(indirect(index, 8 + ((index + offset) & 7)));
    }
  }
  words.push_back(0);
  fixture::Program program(words);
  for (unsigned i = 1; i < words.size(); ++i) {
    program.code.variants[0].entries.push_back(base + i);
  }
  for (const auto mode : modes) {
    const auto native = sharc::Core::compile(program.code, {.mode = mode, .callbacks = {}});
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    for (unsigned row = 0; row < 16; ++row) {
      for (unsigned banks = 0; banks < 4; ++banks) {
        verify_selector(core, {.row = row, .banks = banks});
      }
    }
  }
}

void displacements() {
  for (const auto address : {0U, base, address_mask}) {
    for (const int displacement : {-32, -31, -1, 0, 1, 30, 31}) {
      const std::array words{relative(displacement)};
      fixture::Program program(words, address);
      for (const auto mode : modes) {
        const auto native = sharc::Core::compile(program.code, {.mode = mode, .callbacks = {}});
        sharc::Core core(program.memory(), mode);
        auto expected = initial();
        expected.pc = address;
        core.set_registers(expected);
        (void)core.install(native, fixture::Program::variant_id);
        advance(expected,
                {.pc = (address + static_cast<std::uint32_t>(displacement)) & address_mask,
                 .cycles = 4});
        const auto result = core.run(1);
        check(!result.has_fault &&
                  result.progress == sharc::Progress{.instructions = 1, .base_cycles = 4} &&
                  core.registers() == expected,
              "signed six-bit PC-relative target or 24-bit wrap differs");
      }
    }
  }
}

void verify_parallel(sharc::Core &core, bool otherwise, unsigned row) {
  const bool simd = (row & 4) != 0;
  const bool x = (row & 1) != 0;
  const bool y = (row & 2) != 0;
  auto expected = initial();
  expected.mode1 = unsigned{simd} << simd_bit;
  expected.pe[0].astat = unsigned{x};
  expected.pe[1].astat = unsigned{y};
  expected.dag[0][8].index = base;
  expected.dag[0][12].modifier = 2;
  core.set_registers(expected);
  const auto saved = core.save();
  const bool taken = x && (!simd || y);
  for (unsigned lane = 0; lane < (simd ? 2U : 1U); ++lane) {
    const bool predicate = lane == 0 ? x : y;
    const bool compute = otherwise ? !predicate : !simd || predicate;
    if (compute) {
      expected.pe[lane].data[0][0] = 7U << 8;
      expected.pe[lane].astat = 0;
    }
  }
  const auto cycles = taken ? 4U : 1U;
  advance(expected, {.pc = base + (taken ? 2 : 1), .cycles = cycles});
  for (const bool deadline : {false, true}) {
    core.restore(saved);
    const auto result =
        deadline ? core.run_until(core.state().progress.base_cycles + 1) : core.run(1);
    check(!result.has_fault &&
              result.progress == sharc::Progress{.instructions = 1, .base_cycles = cycles} &&
              core.registers() == expected,
          "parallel branch used new flags or incorrect SISD/SIMD/ELSE compute lanes");
  }
}

void parallel_predicates() {
  constexpr std::uint64_t add_r0_r1_r2 = 0x1012;
  for (const bool dynamic : {false, true}) {
    for (const bool otherwise : {false, true}) {
      const std::array words{(dynamic ? indirect(8, 12, 0) : relative(2, 0)) |
                                 (std::uint64_t{otherwise} << 25) | add_r0_r1_r2,
                             std::uint64_t{0},
                             std::uint64_t{0}};
      fixture::Program program(words);
      program.code.variants[0].entries.push_back(base + 2);
      for (const auto mode : modes) {
        const auto native = sharc::Core::compile(program.code, {.mode = mode, .callbacks = {}});
        sharc::Core core(program.memory(), mode);
        (void)core.install(native, fixture::Program::variant_id);
        for (unsigned row = 0; row < 8; ++row) { verify_parallel(core, otherwise, row); }
      }
    }
  }
}

std::vector<sharc::Registers> call_states() {
  constexpr std::array pcs{base + 4, base + 5, base + 1, base + 2, base + 6, base + 7};
  constexpr std::array cycles{4U, 5U, 9U, 10U, 14U, 15U};
  auto expected = initial();
  expected.pe[0].stky |= stack_empty;
  expected.dag[0][8].index = base + 4;
  expected.dag[0][9].index = base + 6;
  std::vector states{expected};
  for (unsigned i = 0; i < pcs.size(); ++i) {
    if (i == 0) {
      expected.pc_stack[0] = (base + 1) | call_tag;
      expected.pc_stack_pointer = 1;
      expected.pe[0].stky &= ~stack_empty;
    }
    if (i == 1) { expected.pe[0].data[0][6] = 7U << 8; }
    if (i == 2) {
      expected.pc_stack_pointer = 0;
      expected.pe[0].stky |= stack_empty;
    }
    if (i == 3) { expected.pe[0].data[0][0] = 9U << 8; }
    advance(expected, {.pc = pcs[i], .cycles = cycles[i] - (i == 0 ? 0 : cycles[i - 1])});
    states.push_back(expected);
  }
  return states;
}

void cross_unit_call() {
  const std::array words{indirect(8, 8) | (std::uint64_t{1} << 39),
                         fixture::immediate(0, 9),
                         indirect(9, 9),
                         std::uint64_t{0},
                         fixture::add(6, 1, 2),
                         std::uint64_t{0x0a3e00000000},
                         std::uint64_t{0}};
  fixture::Program program(words);
  program.code.variants[0].entries.insert(program.code.variants[0].entries.end(),
                                          {base + 2, base + 4, base + 6});
  constexpr std::array pcs{base + 4, base + 5, base + 1, base + 2, base + 6, base + 7};
  constexpr std::array cycles{4U, 5U, 9U, 10U, 14U, 15U};
  for (const auto mode : modes) {
    const auto native = sharc::Core::compile(program.code, {.mode = mode, .callbacks = {}});
    const auto states = call_states();
    sharc::Core core(program.memory(), mode);
    sharc::Core resumed(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    (void)resumed.install(native, fixture::Program::variant_id);
    for (unsigned count = 1; count < states.size(); ++count) {
      core.set_registers(states[0]);
      const auto result = core.run(count);
      check(!result.has_fault && core.registers() == states[count] &&
                result.progress ==
                    sharc::Progress{.instructions = count, .base_cycles = cycles[count - 1]},
            "indirect call/return differs at an owner instruction bound");
      resumed.restore(core.save());
      if (count < pcs.size()) {
        const auto tail = resumed.run(pcs.size() - count);
        check(!tail.has_fault && resumed.registers() == states.back(),
              "cross-Core snapshot lost an implicit return continuation");
      }
    }
    for (unsigned deadline = 1; deadline <= cycles.back(); ++deadline) {
      core.set_registers(states[0]);
      unsigned count = 1;
      while (cycles[count - 1] < deadline) { ++count; }
      const auto result = core.run_until(core.state().progress.base_cycles + deadline);
      check(!result.has_fault && core.registers() == states[count],
            "indirect call/return crossed the wrong cycle boundary");
    }
    auto redirected = states[0];
    redirected.pc = base + 2;
    redirected.dag[0][9].index = base + 1;
    core.set_registers(redirected);
    const auto refused = core.run(2);
    advance(redirected, {.pc = base + 1, .cycles = 4});
    check(refused.has_fault && refused.fault.reason == sharc::FaultReason::undeclared_entry &&
              refused.fault.address == base + 1 &&
              refused.progress == sharc::Progress{.instructions = 1, .base_cycles = 4} &&
              core.registers() == redirected,
          "ordinary indirect jump acquired a private return entry");
  }
}

void full_parallel_call() {
  const std::array words{indirect(8, 8, 0) | (std::uint64_t{1} << 39) | 0x1012, std::uint64_t{0}};
  fixture::Program program(words);
  for (const auto mode : modes) {
    const auto native = sharc::Core::compile(program.code, {.mode = mode, .callbacks = {}});
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    for (const bool taken : {false, true}) {
      auto expected = initial();
      expected.pc_stack_pointer = sharc::Registers::pc_stack_capacity;
      expected.pe[0].astat = unsigned{taken};
      expected.dag[0][8].index = base;
      core.set_registers(expected);
      const auto result = core.run(1);
      if (taken) {
        check(result.has_fault && result.progress == sharc::Progress{} &&
                  result.fault.reason == sharc::FaultReason::unsupported_feature &&
                  core.registers() == expected,
              "refused parallel CALL committed its compute before checking stack capacity");
      } else {
        expected.pe[0].data[0][0] = 7U << 8;
        advance(expected, {.pc = base + 1, .cycles = 1});
        check(!result.has_fault && core.registers() == expected,
              "untaken parallel CALL inspected a full stack or skipped SISD compute");
      }
    }
  }
}

void parallel_interlock() {
  const std::array words{relative(2, 0) | 0x1012, relative(2, 0), std::uint64_t{0}, relative(0, 0)};
  fixture::Program program(words);
  for (const auto mode : modes) {
    const auto native = sharc::Core::compile(program.code, {.mode = mode, .callbacks = {}});
    sharc::Core core(program.memory(), mode);
    sharc::Core resumed(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    (void)resumed.install(native, fixture::Program::variant_id);
    for (const bool zero : {false, true}) {
      auto expected = initial();
      if (zero) {
        expected.pe[0].data[0][1] = 0;
        expected.pe[0].data[0][2] = 0;
      }
      core.set_registers(expected);
      check(!core.run(1).has_fault, "parallel branch prefix failed");
      expected.pe[0].data[0][0] = zero ? 0 : 7U << 8;
      expected.pe[0].astat = unsigned{zero};
      advance(expected, {.pc = base + 1, .cycles = 1});
      check(core.registers() == expected, "parallel compute used its new flags for its own branch");
      resumed.restore(core.save());
      const auto result = resumed.run_until(resumed.state().progress.base_cycles + 1);
      advance(expected, {.pc = base + (zero ? 3 : 2), .cycles = zero ? 5U : 2U});
      check(!result.has_fault && resumed.registers() == expected &&
                result.progress ==
                    sharc::Progress{.instructions = 1, .base_cycles = zero ? 5U : 2U},
            "snapshot lost the parallel-compute to conditional-branch interlock");
      if (zero) {
        const auto following = resumed.run(1);
        advance(expected, {.pc = base + 3, .cycles = 4});
        check(!following.has_fault && resumed.registers() == expected &&
                  following.progress == sharc::Progress{.instructions = 1, .base_cycles = 4},
              "taken branch failed to clear its preceding compute interlock");
      }
    }
  }
}

void refused_forms() {
  constexpr std::array unsupported_bits{24U, 26U, 38U};
  for (const unsigned bit : unsupported_bits) {
    const auto word = indirect(8, 8) | (std::uint64_t{1} << bit);
    fixture::throws<sharc::PreparationError>(
        [&] { (void)sharc::Core::lower(fixture::image(sharc::InstructionSet::isa, word)); },
        "unsupported computed-branch modifier was accepted");
  }
  const auto word = indirect(8, 8) | (std::uint64_t{1} << 23);
  try {
    (void)sharc::Core::lower(fixture::image(sharc::InstructionSet::isa, word));
    check(false, "reserved computed-branch bit was accepted");
  } catch (const sharc::PreparationError &error) {
    check(error.fault().reason == sharc::FaultReason::invalid_encoding &&
              error.fault().instruction == word && error.fault().address == base,
          "computed-branch refusal lost identity or reason");
  }
}

void long_indirect_chain() {
  constexpr unsigned count = 100000;
  constexpr std::uint64_t total_cycles = std::uint64_t{count} * 4;
  const std::array words{indirect(8, 8)};
  fixture::Program program(words);
  for (const auto mode : modes) {
    const auto native = sharc::Core::compile(program.code, {.mode = mode, .callbacks = {}});
    sharc::Core core(program.memory(), mode);
    auto expected = initial();
    expected.dag[0][8].index = base;
    core.set_registers(expected);
    (void)core.install(native, fixture::Program::variant_id);
    const auto result = core.run(count);
    advance(expected, {.pc = base, .cycles = count * 4});
    check(!result.has_fault && core.registers() == expected &&
              result.progress ==
                  sharc::Progress{.instructions = count, .base_cycles = total_cycles},
          "indirect native chaining lost progress or used recursive host calls");
  }
}
} // namespace

namespace fixture {
void computed_control_flow() {
  selectors();
  displacements();
  parallel_predicates();
  cross_unit_call();
  full_parallel_call();
  parallel_interlock();
  refused_forms();
  long_indirect_chain();
}
} // namespace fixture
