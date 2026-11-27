// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "fixed_cases.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
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

struct DualRegisters {
  unsigned sum, difference, x, y;
};

struct DualControls {
  unsigned banks, predicates;
  bool simd;
};

constexpr std::uint64_t dual_field(DualRegisters operands) {
  constexpr unsigned difference_shift = 12;
  constexpr std::uint64_t fixed_dual = 0x070000;
  return fixed_dual | (std::uint64_t{operands.difference} << difference_shift) |
         (std::uint64_t{operands.sum} << destination_shift) |
         (std::uint64_t{operands.x} << source_shift) | operands.y;
}

template <class State> auto &dual_data(State &state, unsigned lane, unsigned reg) {
  const auto shift = reg < 8 ? low_bank_bit : high_bank_bit;
  return state.pe[lane].data[(state.mode1 >> shift) & 1][reg];
}

sharc::Registers
dual_state(const fixture::FixedDualCase &item, DualRegisters operands, DualControls controls) {
  sharc::Registers state;
  state.pc = fixture::code_address;
  state.emuclk = initial_clock;
  state.mode1 = ((controls.banks & 1) << low_bank_bit) | ((controls.banks >> 1) << high_bank_bit) |
                (unsigned{item.saturate} << saturation_bit) | (unsigned{controls.simd} << simd_bit);
  for (unsigned lane = 0; lane < state.pe.size(); ++lane) {
    auto &pe = state.pe[lane];
    const auto &reference = item.lanes[lane];
    for (unsigned bank = 0; bank < pe.data.size(); ++bank) {
      for (unsigned reg = 0; reg < pe.data[bank].size(); ++reg) {
        pe.data[bank][reg] = raw_seed + (lane << 16) + (bank << 8) + reg;
      }
    }
    dual_data(state, lane, operands.x) = reference.inputs[0];
    dual_data(state, lane, operands.y) = reference.inputs[1];
    pe.astat = reference.old_astat | ((controls.predicates >> lane) & 1);
    pe.stky = reference.old_stky;
  }
  return state;
}

void expect_dual(sharc::Registers &state,
                 const fixture::FixedDualCase &item,
                 DualRegisters operands,
                 unsigned active) {
  for (unsigned lane = 0; lane < state.pe.size(); ++lane) {
    if ((active & (1U << lane)) == 0) { continue; }
    const auto &reference = item.lanes[lane];
    dual_data(state, lane, operands.difference) = reference.outputs[1];
    dual_data(state, lane, operands.sum) = reference.outputs[0];
    state.pe[lane].astat = reference.astat;
    state.pe[lane].stky = reference.stky;
  }
}

void verify_dual(sharc::Core &core,
                 const fixture::FixedDualCase &item,
                 DualRegisters operands,
                 DualControls controls) {
  const auto before = dual_state(item, operands, controls);
  core.set_registers(before);
  const auto start = core.save();
  auto expected = before;
  expect_dual(expected, item, operands, controls.predicates & (controls.simd ? 3U : 1U));
  const auto deadline = core.state().progress.base_cycles + 1;
  for (const bool prefix : {true, false}) {
    core.restore(start);
    const auto count = prefix ? 1U : 3U;
    const auto result = prefix ? core.run_until(deadline) : core.run(count);
    fixture::boundary(expected, {.pc = before.pc + count, .clock = before.emuclk + count});
    fixture::check(fixture::completed(result, count, count) && core.registers() == expected,
                   "fixed dual ALU lost an old input, result, predicate or flag union");
  }
}

void fixed_dual(DualRegisters operands) {
  constexpr std::uint64_t if_eq = 0x010000000000;
  const std::array words{if_eq | dual_field(operands), std::uint64_t{0}, std::uint64_t{0}};
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    for (const auto &item : fixture::fixed_dual_cases) {
      for (unsigned banks = 0; banks < 4; ++banks) {
        for (const bool simd : {false, true}) {
          for (unsigned predicates = 0; predicates < (simd ? 4U : 2U); ++predicates) {
            verify_dual(
                core, item, operands, {.banks = banks, .predicates = predicates, .simd = simd});
          }
        }
      }
    }
  }
}

enum class DualPacketKind : std::uint8_t {
  dual_bus,
  indexed_load,
  immediate_store,
  register_move,
  modify,
  branch,
  branch_else
};

struct DualPacket {
  DualPacketKind kind;
  std::uint64_t word;
};

constexpr DualRegisters packet_registers{.sum = 6, .difference = 15, .x = 1, .y = 2};
constexpr std::uint32_t dual_data_address = 0x90000, dual_program_address = 0x90010;
constexpr unsigned dual_memory_id = 21;
constexpr std::size_t normal_word_bytes = sizeof(std::uint32_t);
constexpr unsigned dual_dm_index = 1, dual_pm_index = 9;
constexpr std::array<std::uint32_t, 2> dual_load_words{0x4af65302, 0x75ee1209};
using PacketMemory = std::array<std::byte, 4 * normal_word_bytes>;

void put_packet_word(std::byte *word, std::uint32_t value) {
  for (unsigned byte = 0; byte < normal_word_bytes; ++byte) {
    word[byte] = static_cast<std::byte>(value >> (byte * 8));
  }
}

unsigned packet_compute_lanes(DualPacketKind kind, DualControls controls) {
  const auto available = controls.simd ? 3U : 1U;
  if (kind == DualPacketKind::dual_bus || (kind == DualPacketKind::branch && !controls.simd)) {
    return available;
  }
  const auto active =
      kind == DualPacketKind::branch_else ? ~controls.predicates : controls.predicates;
  return active & available;
}

void expect_packet_transfer(sharc::Registers &state,
                            const sharc::Registers &before,
                            PacketMemory &memory,
                            DualPacketKind kind,
                            unsigned active) {
  if (active == 0) { return; }
  if (kind == DualPacketKind::dual_bus || kind == DualPacketKind::indexed_load ||
      kind == DualPacketKind::immediate_store || kind == DualPacketKind::modify) {
    state.dag[0][dual_dm_index].index += 2;
  }
  if (kind == DualPacketKind::dual_bus) { state.dag[0][dual_pm_index].index += 2; }
  for (unsigned lane = 0; lane < state.pe.size(); ++lane) {
    if ((active & (1U << lane)) == 0) { continue; }
    const auto source = dual_data(before, lane, packet_registers.difference);
    if (kind == DualPacketKind::dual_bus || kind == DualPacketKind::indexed_load) {
      dual_data(state, lane, packet_registers.sum) = std::uint64_t{dual_load_words[lane]} << 8;
    }
    if (kind == DualPacketKind::dual_bus || kind == DualPacketKind::immediate_store) {
      const auto slot = kind == DualPacketKind::dual_bus ? lane + 2 : lane;
      put_packet_word(memory.data() + (slot * normal_word_bytes),
                      static_cast<std::uint32_t>(source >> 8));
    }
    if (kind == DualPacketKind::register_move) {
      dual_data(state, lane, packet_registers.sum) = source;
    }
  }
}

void verify_dual_packet(sharc::Core &core,
                        PacketMemory &memory,
                        DualPacketKind kind,
                        DualControls controls) {
  const auto &reference = fixture::fixed_dual_packet;
  auto before = dual_state(reference, packet_registers, controls);
  before.dag[0][dual_dm_index].index = dual_data_address;
  before.dag[0][dual_pm_index].index = dual_program_address;
  before.dag[0][3].modifier = 2;
  before.dag[0][11].modifier = 2;
  core.set_registers(before);
  const auto start = core.save();
  PacketMemory seed{};
  for (unsigned lane = 0; lane < before.pe.size(); ++lane) {
    put_packet_word(seed.data() + (lane * normal_word_bytes), dual_load_words[lane]);
  }
  auto expected = before;
  auto expected_memory = seed;
  const auto active = packet_compute_lanes(kind, controls);
  expect_dual(expected, reference, packet_registers, active);
  expect_packet_transfer(expected, before, expected_memory, kind, active);
  const auto all_pe = controls.simd ? 3U : 1U;
  const bool branch = kind == DualPacketKind::branch || kind == DualPacketKind::branch_else;
  const bool taken = branch && (controls.predicates & all_pe) == all_pe;
  const auto deadline = core.state().progress.base_cycles + 1;
  for (const bool prefix : {true, false}) {
    core.restore(start);
    memory = seed;
    const auto count = prefix ? 1U : 2U;
    constexpr unsigned taken_penalty = 3;
    const auto cycles = count + (taken ? taken_penalty : 0U);
    const auto result = prefix ? core.run_until(deadline) : core.run(count);
    fixture::boundary(expected,
                      {.pc = before.pc + count + unsigned{taken}, .clock = before.emuclk + cycles});
    fixture::check(fixture::completed(result, count, cycles) && core.registers() == expected &&
                       memory == expected_memory,
                   "fixed dual packet lost an old operand, writeback priority or branch predicate");
  }
}

void dual_packets() {
  // Classic headers for the packets assembled in the independent reference probes.
  constexpr std::array packets{
      DualPacket{.kind = DualPacketKind::dual_bus, .word = 0x22ec5f800000},
      DualPacket{.kind = DualPacketKind::indexed_load, .word = 0x52c003000000},
      DualPacket{.kind = DualPacketKind::immediate_store, .word = 0x62c017800000},
      DualPacket{.kind = DualPacketKind::register_move, .word = 0x70c183000000},
      DualPacket{.kind = DualPacketKind::modify, .word = 0x040058000000},
      DualPacket{.kind = DualPacketKind::branch, .word = 0x090010000000},
      DualPacket{.kind = DualPacketKind::branch_else, .word = 0x090012000000},
  };
  for (const auto &packet : packets) {
    const std::array words{packet.word | dual_field(packet_registers),
                           std::uint64_t{0},
                           std::uint64_t{0},
                           std::uint64_t{0}};
    fixture::Program program(words);
    auto &regions = program.code.variants.front().layout.regions;
    regions.push_back(fixture::normal_region(dual_data_address, 2, dual_memory_id));
    auto pm = fixture::normal_region(dual_program_address, 2, dual_memory_id);
    pm.space = sharc::MemorySpace::program;
    pm.byte_offset = 2 * normal_word_bytes;
    regions.push_back(pm);
    for (const auto mode : fixture::numeric_modes) {
      const auto native = fixture::compile(program.code, mode);
      PacketMemory memory{};
      sharc::Core core(program.memory(dual_memory_id, memory), mode);
      (void)core.install(native, fixture::Program::variant_id);
      for (unsigned banks = 0; banks < 4; ++banks) {
        for (const bool simd : {false, true}) {
          for (unsigned predicates = 0; predicates < (simd ? 4U : 2U); ++predicates) {
            verify_dual_packet(core,
                               memory,
                               packet.kind,
                               {.banks = banks, .predicates = predicates, .simd = simd});
          }
        }
      }
    }
  }
}
} // namespace

int main() {
  return fixture::run_test([] {
    fixed_dual({.sum = 6, .difference = 15, .x = 1, .y = 10});
    fixed_dual({.sum = 1, .difference = 10, .x = 1, .y = 10});
    fixed_dual({.sum = 10, .difference = 10, .x = 1, .y = 10});
    dual_packets();
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
