// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <vector>

namespace {
using fixture::check;
constexpr unsigned circular_bit = 24, simd_bit = 21;
constexpr unsigned sticky_bit = 17, interrupt_bit = 20;
constexpr std::array<unsigned, 16> bank_bits{4, 4, 4, 4, 3, 3, 3, 3, 6, 6, 6, 6, 5, 5, 5, 5};
constexpr std::uint64_t raw_seed = 0x81234567ef;
constexpr std::uint32_t primary_base = 0x2000, alternate_base = 0x6000;
constexpr std::uint32_t old_latch = 0x15, old_status = 0x13579bdf, old_sticky = 0x80000400;
constexpr std::uint32_t buffer_length = 17, old_modifier = 0xabcdef00, near_wrap = 0xfffffff8;
constexpr std::array<std::int32_t, 8> steps{1, -1, 16, -16, 7, -7, 0, 1};

struct Modify {
  unsigned source{}, destination{};
  std::int32_t displacement{};
  bool reverse{};
};

std::uint64_t encoding(const Modify &op) {
  constexpr unsigned reverse_shift = 39;
  constexpr unsigned dag_shift = 38;
  constexpr unsigned destination_shift = 35;
  constexpr std::uint64_t opcode = 0x160000000000;
  return opcode | (std::uint64_t{op.reverse} << reverse_shift) |
         (std::uint64_t{op.source / 8} << dag_shift) |
         (std::uint64_t{(op.source ^ op.destination) & 7} << destination_shift) |
         (std::uint64_t{op.source & 7} << 32) | static_cast<std::uint32_t>(op.displacement);
}

std::uint32_t reverse_bits(std::uint32_t value) {
  std::uint32_t result{};
  for (unsigned bit = 0; bit < 32; ++bit) { result = (result << 1) | ((value >> bit) & 1); }
  return result;
}

void apply(sharc::Registers &registers, const Modify &op) {
  const auto source_bank = (registers.mode1 >> bank_bits[op.source]) & 1;
  const auto destination_bank = (registers.mode1 >> bank_bits[op.destination]) & 1;
  const auto source = registers.dag[source_bank][op.source];
  auto result = std::int64_t{source.index} + op.displacement;
  bool overflow{};
  if (source.length != 0) {
    if (result < source.base) {
      result += source.length;
      overflow = true;
    } else if (result >= std::int64_t{source.base} + source.length) {
      result -= source.length;
      overflow = true;
    }
  } else if ((registers.mode1 & (1U << circular_bit)) != 0) {
    const auto word = static_cast<std::uint32_t>(result);
    overflow = op.displacement < 0 ? word < source.base : word >= source.base;
  }
  auto word = static_cast<std::uint32_t>(result);
  if (op.reverse) { word = reverse_bits(word); }
  registers.dag[destination_bank][op.destination].index = word;
  if ((op.source & 7) == 7 && overflow) {
    registers.pe[0].stky |= 1U << (sticky_bit + (op.source / 8));
    registers.irptl |= 1U << (interrupt_bit + (op.source / 8));
  }
  fixture::boundary(registers, {.pc = registers.pc + 1, .clock = registers.emuclk + 1});
}

struct Input {
  unsigned source{}, flags{}, configuration{};
};

sharc::Registers initial(const Input &input) {
  const auto [source, flags, configuration] = input;
  sharc::Registers registers;
  registers.pc = fixture::code_address;
  registers.irptl = old_latch;
  for (unsigned quarter = 0; quarter < 4; ++quarter) {
    constexpr std::array quarter_bits{4U, 3U, 6U, 5U};
    registers.mode1 |= ((flags >> quarter) & 1) << quarter_bits[quarter];
  }
  registers.mode1 |= ((flags >> 4) & 1) << simd_bit;
  registers.mode1 |= ((flags >> 5) & 1) << circular_bit;
  for (unsigned lane = 0; lane < 2; ++lane) {
    for (unsigned bank = 0; bank < 2; ++bank) {
      registers.pe[lane].data[bank].fill(raw_seed + (std::uint64_t{lane} * 16) + bank);
    }
    registers.pe[lane].astat = old_status;
    registers.pe[lane].stky = old_sticky;
  }
  for (unsigned bank = 0; bank < 2; ++bank) {
    for (unsigned index = 0; index < 16; ++index) {
      auto &dag = registers.dag[bank][index];
      dag.base = (bank == 0 ? primary_base : alternate_base) + (index * 32);
      dag.index = dag.base + (configuration == 2 ? 16U : 0U);
      dag.length = configuration == 0 ? 0U : buffer_length;
      dag.modifier = old_modifier + index;
    }
    auto &dag = registers.dag[bank][source];
    if ((source & 7) == 7) {
      // Enhanced I7/I15 overflow routing is not inferred by these cases.
      dag.length = 4096;
      dag.index = dag.base + 2048;
    } else if (configuration == 0) {
      dag.index = bank == 0 ? near_wrap : 8;
    }
  }
  return registers;
}

void verify(fixture::Program &program,
            const sharc::NativeImage &native,
            sharc::ExecutionMode mode,
            sharc::Registers expected,
            const std::vector<Modify> &operations) {
  sharc::Core core(program.memory(), mode);
  fixture::install(core, native, expected);
  const auto saved = core.save();
  std::uint64_t count{};
  for (const auto &op : operations) {
    apply(expected, op);
    ++count;
    for (const bool cycles : {false, true}) {
      core.restore(saved);
      const auto result = cycles ? core.run_until(count) : core.run(count);
      check(fixture::completed(result, count, count) && core.registers() == expected,
            "DAG modify/reverse source, destination, banks, flags or exact prefix differs");
    }
  }
}

void index_matrix(sharc::ExecutionMode mode) {
  for (unsigned source = 0; source < 16; ++source) {
    for (const bool reverse : {false, true}) {
      std::vector<Modify> operations;
      std::vector<std::uint64_t> words;
      for (unsigned i = 0; i < 8; ++i) {
        const Modify op{.source = source,
                        .destination = (source & 8) | ((source + i + 1) & 7),
                        .displacement = steps[i],
                        .reverse = reverse};
        operations.push_back(op);
        words.push_back(encoding(op));
      }
      fixture::Program program(words);
      const auto native = fixture::compile(program.code, mode);
      for (unsigned flags = 0; flags < 64; ++flags) {
        for (unsigned configuration = 0; configuration < 3; ++configuration) {
          verify(program,
                 native,
                 mode,
                 initial({.source = source, .flags = flags, .configuration = configuration}),
                 operations);
        }
      }
    }
  }
}

void verify_special(fixture::Program &program,
                    const sharc::NativeImage &native,
                    sharc::ExecutionMode mode,
                    const Modify &op) {
  for (unsigned flags = 0; flags < 64; ++flags) {
    for (const unsigned length : {0U, buffer_length}) {
      auto expected = initial({.source = op.source, .flags = flags, .configuration = 1});
      for (auto &bank : expected.dag) {
        bank[op.source].length = length;
        bank[op.source].index =
            bank[op.source].base + (op.displacement < 0 || length == 0 ? 0U : length - 1);
      }
      verify(program, native, mode, expected, {op});
    }
  }
}

void special_buffers(sharc::ExecutionMode mode) {
  for (const unsigned index : {7U, 15U}) {
    for (const bool reverse : {false, true}) {
      for (const auto displacement : {-1, 1}) {
        const Modify op{.source = index,
                        .destination = index,
                        .displacement = displacement,
                        .reverse = reverse};
        const std::array words{encoding(op)};
        fixture::Program program(words);
        const auto native = fixture::compile(program.code, mode);
        verify_special(program, native, mode, op);
      }
    }
  }
}

struct RegisterModify {
  unsigned source{}, modifier{}, destination{};
  std::uint64_t compute{};
  unsigned condition{};
};

constexpr std::uint64_t encoding(const RegisterModify &op) {
  constexpr std::uint64_t prefix = 0x040000000000;
  constexpr unsigned dag_shift = 38;
  constexpr unsigned condition_shift = 33;
  constexpr unsigned source_shift = 30;
  constexpr unsigned modifier_shift = 27;
  constexpr unsigned destination_shift = 24;
  return prefix | (std::uint64_t{op.source / 8} << dag_shift) |
         (std::uint64_t{op.condition} << condition_shift) |
         (std::uint64_t{op.source & 7} << source_shift) |
         (std::uint64_t{op.modifier & 7} << modifier_shift) |
         (std::uint64_t{(op.destination ^ op.source) & 7} << destination_shift) | op.compute;
}

constexpr std::uint64_t add_r4 = fixture::add(4, 1, 2) & 0x7fffff;
constexpr RegisterModify enhanced_pm{
    .source = 15, .modifier = 14, .destination = 13, .compute = add_r4};
constexpr std::uint64_t assembled_enhanced_pm = 0x0441f2001412;
static_assert(encoding(enhanced_pm) == assembled_enhanced_pm);

void expect_register_modify(sharc::Registers &state,
                            const RegisterModify &op,
                            unsigned predicates) {
  constexpr unsigned alu_status = 0x43f;
  constexpr std::array<unsigned, 2> sums{49, 70};
  const auto active = (state.mode1 & (1U << simd_bit)) == 0 ? predicates & 1 : predicates;
  if (op.compute != 0) {
    for (unsigned lane = 0; lane < state.pe.size(); ++lane) {
      if ((active & (1U << lane)) == 0) { continue; }
      state.pe[lane].data[0][4] = sums[lane] << 8;
      state.pe[lane].astat &= ~alu_status;
    }
  }
  if (active != 0) {
    const auto bank = (state.mode1 >> bank_bits[op.modifier]) & 1;
    const auto delta = std::bit_cast<std::int32_t>(state.dag[bank][op.modifier].modifier);
    apply(state, {.source = op.source, .destination = op.destination, .displacement = delta});
  } else {
    fixture::boundary(state, {.pc = state.pc + 1, .clock = state.emuclk + 1});
  }
}

struct ModifyConfiguration {
  unsigned mode_flags{}, predicates{};
};

sharc::Registers register_modify_input(const RegisterModify &op,
                                       ModifyConfiguration configuration) {
  constexpr unsigned step = 7;
  constexpr std::array<std::uint64_t, 2> x{19, 10};
  constexpr std::array<std::uint64_t, 2> y{30, 60};
  constexpr unsigned x_extension = 0x59;
  constexpr unsigned y_extension = 0xa5;
  auto state =
      initial({.source = op.source, .flags = configuration.mode_flags, .configuration = 1});
  for (unsigned bank = 0; bank < state.dag.size(); ++bank) {
    auto &source = state.dag[bank][op.source];
    source.length = buffer_length;
    source.index = source.base + (bank == 0 ? buffer_length - 2 : 1);
    state.dag[bank][op.modifier].modifier = bank == 0 ? step : 0U - step;
  }
  for (unsigned lane = 0; lane < state.pe.size(); ++lane) {
    auto &pe = state.pe[lane];
    pe.astat = (old_status & ~1U) | ((configuration.predicates >> lane) & 1);
    pe.data[0][1] = (x[lane] << 8) | x_extension;
    pe.data[0][2] = (y[lane] << 8) | y_extension;
  }
  return state;
}

void register_modify_matrix(sharc::ExecutionMode mode) {
  constexpr std::array operations{
      RegisterModify{.source = 7, .modifier = 1, .destination = 2, .compute = add_r4},
      RegisterModify{.source = 7, .modifier = 3, .destination = 7},
      enhanced_pm,
      RegisterModify{.source = 12, .modifier = 9, .destination = 8, .compute = add_r4}};
  constexpr unsigned contexts = 64;
  constexpr unsigned masks = 4;
  for (const auto &op : operations) {
    fixture::Program program(std::array{encoding(op), std::uint64_t{0}});
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    const auto beginning = core.save();
    for (unsigned flags = 0; flags < contexts; ++flags) {
      for (unsigned predicates = 0; predicates < masks; ++predicates) {
        auto expected = register_modify_input(op, {.mode_flags = flags, .predicates = predicates});
        core.restore(beginning);
        core.set_registers(expected);
        const auto start = core.save();
        expect_register_modify(expected, op, predicates);
        check(fixture::completed(core.run_until(1), 1, 1) && core.registers() == expected,
              "register MODIFY lost a predicate, bank, computation or overflow");
        core.restore(start);
        fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 2});
        check(fixture::completed(core.run(2), 2, 2) && core.registers() == expected,
              "register MODIFY changed effects under a whole-unit grant");
      }
    }
  }
}

void verify_register_modify_timing(sharc::ExecutionMode mode, unsigned reg, unsigned gap) {
  constexpr unsigned i5 = 0x15;
  constexpr unsigned m1 = 0x21;
  constexpr unsigned b5 = 0x45;
  constexpr unsigned ram_id = 27;
  constexpr unsigned dm_address = 0x90000;
  constexpr unsigned source = 5;
  constexpr unsigned destination = 2;
  constexpr unsigned condition_true = 31;
  constexpr std::uint64_t read_r4 = 0xa40400000000; // R4 = DM(0,I2).
  constexpr std::uint32_t value = 0x2468ace0;
  const auto payload = reg == i5 || reg == b5 ? dm_address : static_cast<unsigned>(reg == m1);
  std::vector words{reg == 0 ? std::uint64_t{0} : fixture::immediate(reg, payload)};
  words.insert(words.end(), gap, 0);
  words.push_back(encoding(RegisterModify{
      .source = source, .modifier = 1, .destination = destination, .condition = condition_true}));
  words.push_back(read_r4);
  fixture::Program program(words);
  program.code.variants.front().layout.regions.push_back(
      fixture::normal_region(dm_address, 2, ram_id));
  std::array<std::byte, 8> ram{};
  for (unsigned byte = 0; byte < 4; ++byte) {
    ram[4 + byte] = static_cast<std::byte>(value >> (byte * 8));
  }
  const auto native = fixture::compile(program.code, mode);
  sharc::Core core(program.memory(ram_id, ram), mode);
  sharc::Registers before;
  before.pc = fixture::code_address;
  before.dag[0][source] = {.index = dm_address, .base = dm_address};
  before.dag[0][1].modifier = 1;
  fixture::install(core, native, before);
  const auto start = core.save();
  const auto count = static_cast<std::uint32_t>(words.size());
  const auto clocks = count + (reg == 0 ? 0 : 2 - gap);
  auto expected = before;
  expected.dag[0][destination].index = dm_address + 1;
  expected.pe[0].data[0][4] = std::uint64_t{value} << 8;
  fixture::boundary(expected, {.pc = before.pc + count, .clock = clocks});
  check(fixture::completed(core.run(count), count, clocks) && core.registers() == expected,
        "register MODIFY lost a source interlock or delayed the resulting address");
  core.restore(start);
  check(fixture::completed(core.run_until(clocks - 1), count - 1, clocks - 1),
        "register MODIFY did not stop at the exact memory boundary");
  check(fixture::completed(core.run_until(clocks), 1, 1) && core.registers() == expected,
        "immediately following load did not see the modified index after a bound");
}

void register_modify_timing(sharc::ExecutionMode mode) {
  constexpr std::array sources{0U, 0x15U, 0x21U, 0x35U, 0x45U}; // NOP, I5, M1, L5, B5.
  for (const auto reg : sources) {
    for (unsigned gap = 0; gap <= 2; ++gap) { verify_register_modify_timing(mode, reg, gap); }
  }
}

struct LoadTiming {
  unsigned source, gap;
  bool natural_long, forced;
};

constexpr unsigned timing_ram_address = 0x90000;
constexpr unsigned timing_ram_id = 31;
constexpr unsigned explicit_index = 13;
constexpr unsigned neighbor_index = explicit_index ^ 1;
constexpr unsigned consumer_modifier = 14;
constexpr unsigned condition_true = 31;
constexpr std::uint32_t new_explicit_index = 0xc4001;
constexpr std::uint32_t new_neighbor_index = 0xc4002;

void verify_dag_load_timing(sharc::Core &core, const LoadTiming &form, bool predicate) {
  const auto bank = predicate ? 0U : 1U;
  sharc::Registers before;
  before.pc = fixture::code_address;
  before.mode1 = bank << bank_bits[explicit_index];
  before.pe[0].astat = predicate ? 1U : 0U;
  before.dag[0][1].index = timing_ram_address;
  before.dag[bank][explicit_index].index = primary_base;
  before.dag[bank][neighbor_index].index = alternate_base;
  before.dag[bank][consumer_modifier].modifier = 1;
  core.set_registers(before);
  const auto entry = core.save();
  const unsigned instructions = form.gap + 2;
  const bool interlocked =
      form.source == explicit_index || (form.forced && form.source == neighbor_index);
  const auto stalls = interlocked ? 2 - form.gap : 0;
  auto expected = before;
  if (predicate) {
    expected.dag[bank][explicit_index].index = new_explicit_index;
    if (form.natural_long || form.forced) {
      expected.dag[bank][neighbor_index].index = new_neighbor_index;
    }
  }
  ++expected.dag[bank][form.source].index;
  fixture::boundary(expected, {.pc = before.pc + instructions, .clock = instructions + stalls});
  check(fixture::completed(core.run(instructions), instructions, instructions + stalls) &&
            core.registers() == expected,
        "DAG load used the wrong address dependency, cycle cost or resulting register");
  core.restore(entry);
  check(fixture::completed(core.run_until(instructions - 1), instructions - 1, instructions - 1),
        "DAG load did not reach the bound just before the dependent instruction");
  const auto pending = core.save();
  for (const bool restore : {false, true}) {
    if (restore) { core.restore(pending); }
    check(fixture::completed(core.run_until(instructions), 1, 1 + stalls) &&
              core.registers() == expected,
          "DAG dependency or consumed value changed across a bound or snapshot");
  }
}

void dag_load_timing(sharc::ExecutionMode mode, const LoadTiming &form) {
  constexpr std::uint64_t load = 0x52c00e800000; // IF EQ I13 = DM(I1,M3).
  constexpr std::uint64_t force_long = 1ULL << 30;
  const auto word = load | (form.forced ? force_long : 0);
  std::vector words{word};
  words.insert(words.end(), form.gap, 0);
  words.push_back(encoding(RegisterModify{.source = form.source,
                                          .modifier = consumer_modifier,
                                          .destination = form.source,
                                          .condition = condition_true}));
  fixture::Program program(words);
  auto data = fixture::normal_region(timing_ram_address, 2, timing_ram_id);
  if (form.natural_long) {
    data.view = sharc::MemoryView::long_word;
    data.count = 1;
    data.byte_stride = 8;
  }
  program.code.variants.front().layout.regions.push_back(data);
  std::array<std::byte, 8> ram{};
  constexpr std::uint64_t payload = (std::uint64_t{new_neighbor_index} << 32) | new_explicit_index;
  for (unsigned byte = 0; byte < ram.size(); ++byte) {
    ram[byte] = static_cast<std::byte>(payload >> (byte * 8));
  }
  const auto native = fixture::compile(program.code, mode);
  sharc::Core core(program.memory(timing_ram_id, ram), mode);
  (void)core.install(native, fixture::Program::variant_id);
  const auto beginning = core.save();
  for (const bool predicate : {false, true}) {
    core.restore(beginning);
    verify_dag_load_timing(core, form, predicate);
  }
}

void dag_load_timing(sharc::ExecutionMode mode) {
  constexpr unsigned unrelated = 14;
  for (const auto source : {explicit_index, neighbor_index, unrelated}) {
    for (unsigned gap = 0; gap <= 2; ++gap) {
      dag_load_timing(mode, {.source = source, .gap = gap, .natural_long = false, .forced = false});
      dag_load_timing(mode, {.source = source, .gap = gap, .natural_long = true, .forced = false});
      dag_load_timing(mode, {.source = source, .gap = gap, .natural_long = false, .forced = true});
    }
  }
}
} // namespace

void fixture::dag_addressing() {
  for (const auto mode : fixture::numeric_modes) {
    index_matrix(mode);
    special_buffers(mode);
    register_modify_matrix(mode);
    register_modify_timing(mode);
    dag_load_timing(mode);
  }
}
