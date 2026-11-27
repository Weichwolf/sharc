// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "floating_cases.hpp"

#include <array>
#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <map>
#include <sharc/core.hpp>
#include <sstream>

namespace {
constexpr std::uint32_t simd_mode = 1U << 21;
constexpr std::uint32_t round32_mode = 1U << 16;
constexpr std::uint32_t truncate_mode = 1U << 15;
constexpr std::uint32_t alternate_low = 1U << 10;
constexpr std::uint64_t raw_seed = 0x81234567ef;
constexpr std::uint64_t compute_true = 0x013e00000000;
constexpr std::uint64_t compute_eq = 0x010000000000;
constexpr std::uint64_t destination_r1 = 0x100;
constexpr std::uint64_t sources_r2_r3 = 0x23;
constexpr std::uint32_t entry_stride = 8;
constexpr std::uint32_t conditional_offset = 4;
constexpr std::uint32_t zero_flag = 1;
constexpr std::uint32_t alternate_high = 1U << 7;
constexpr std::uint64_t copy_to_low = 0x70bf02000000; // R4 = R10
constexpr std::uint64_t compare_copy = 0x013e0008a04b;
constexpr std::uint64_t compare_high = 0x013e0008a0ab;
constexpr std::array comparison_program{copy_to_low,
                                        compare_copy,
                                        std::uint64_t{0x010000021540},
                                        compare_high,
                                        std::uint64_t{0x013e00021760}};

struct Entry {
  std::uint32_t address;
  bool conditional;
};

void verify(sharc::Core &core, const fixture::FloatingCase &test, Entry entry, unsigned mode1) {
  sharc::Registers initial;
  initial.pc = entry.address;
  constexpr unsigned alusat_mode = 1U << 13;
  initial.mode1 = mode1 | (test.rnd32 ? round32_mode : 0) | (test.truncate ? truncate_mode : 0) |
                  (test.saturate ? alusat_mode : 0);
  const unsigned bank = (mode1 & alternate_low) != 0 ? 1U : 0U;
  const std::size_t lanes = (mode1 & simd_mode) != 0 ? initial.pe.size() : 1;
  for (std::size_t lane = 0; lane < initial.pe.size(); ++lane) {
    auto &pe = initial.pe[lane];
    for (auto &data : pe.data) { data.fill(raw_seed); }
    pe.data[bank][2] = test.x[lane];
    pe.data[bank][3] = test.y[lane];
    pe.astat = test.old_astat[lane];
    pe.stky = test.old_stky[lane];
  }
  core.set_registers(initial);
  const auto start = core.save();
  auto expected = initial;
  for (std::size_t lane = 0; lane < lanes; ++lane) {
    if (entry.conditional && (test.old_astat[lane] & zero_flag) == 0) { continue; }
    auto &pe = expected.pe[lane];
    if (test.writes) { pe.data[bank][1] = test.raw[lane]; }
    pe.astat = test.astat[lane];
    pe.stky = test.stky[lane];
  }
  for (const auto instructions : {1U, 2U}) {
    core.restore(start);
    fixture::boundary(expected, {.pc = initial.pc + instructions, .clock = instructions});
    if (!fixture::completed(core.run(instructions), instructions, instructions) ||
        core.registers() != expected) {
      const auto actual = core.registers();
      std::ostringstream detail;
      detail << std::hex << "floating field=0x" << test.field << " FastFP32=" << test.fast
             << " MODE1=0x" << initial.mode1 << " conditional=" << entry.conditional << " X=0x"
             << test.x[0] << "/0x" << test.x[1] << " Y=0x" << test.y[0] << "/0x" << test.y[1]
             << " instructions=" << instructions;
      for (std::size_t lane = 0; lane < actual.pe.size(); ++lane) {
        const auto &pe = actual.pe[lane];
        const auto &want = expected.pe[lane];
        detail << " lane " << lane << " raw/ASTAT/STKY 0x" << pe.data[bank][1] << "/0x" << pe.astat
               << "/0x" << pe.stky << " expected 0x" << want.data[bank][1] << "/0x" << want.astat
               << "/0x" << want.stky;
      }
      fixture::check(false, detail.str());
    }
  }
}

void verify_compare_sequence(sharc::Core &core, bool fast_mode) {
  constexpr std::uint64_t one = 0x3f80000000;
  constexpr std::uint64_t midpoint = 0x3f80000080;
  constexpr std::uint64_t fixed_source = 0x13579abc7e;
  constexpr std::uint64_t fixed_result = 0x13579abc00;
  constexpr std::uint32_t original_history = 0x4c000000;
  constexpr std::uint32_t equal_twice = 0x13000000;
  constexpr std::uint32_t greater_twice = 0xd3000000;
  sharc::Registers initial;
  initial.pc = fixture::code_address;
  initial.mode1 = alternate_high;
  auto &pe = initial.pe[0];
  pe.data[1][10] = midpoint;
  pe.data[1][11] = one;
  pe.data[0][4] = raw_seed;
  pe.data[0][5] = raw_seed;
  pe.data[0][6] = fixed_source;
  pe.astat = original_history;
  core.set_registers(initial);
  auto expected = initial;
  // The raw move preserves the midpoint. The first compare controls IF EQ.
  // The second contributes CACC across an overwrite of the ordinary ALU flags.
  auto &finished = expected.pe[0];
  finished.data[0][4] = midpoint;
  finished.data[0][5] = fast_mode ? one : raw_seed;
  finished.data[0][7] = fixed_result;
  finished.astat = fast_mode ? equal_twice : greater_twice;
  constexpr auto count = static_cast<std::uint32_t>(comparison_program.size());
  fixture::boundary(expected, {.pc = initial.pc + count, .clock = count});
  // The midpoint would compare greater after upward host rounding in FastFP32.
  const auto host_rounding = std::fegetround();
  fixture::check(host_rounding != -1 && std::fesetround(FE_UPWARD) == 0,
                 "could not select upward host rounding");
  const auto result = core.run(count);
  const auto observed_rounding = std::fegetround();
  fixture::check(std::fesetround(host_rounding) == 0 && observed_rounding == FE_UPWARD,
                 "installed floating compute changed host rounding");
  fixture::check(fixture::completed(result, count, count) && core.registers() == expected,
                 "floating comparison lost the predicate, raw transfer or CACC history");
}

void floating() {
  fixture::Program program(comparison_program);
  auto &variant = program.code.variants.front();
  std::map<std::uint32_t, std::uint32_t> addresses;
  for (const auto &test : fixture::floating_cases) {
    const auto slot = 1 + static_cast<std::uint32_t>(addresses.size());
    const auto base = fixture::code_address + (entry_stride * slot);
    if (!addresses.try_emplace(test.field, base).second) { continue; }
    const auto operands = sources_r2_r3 | (test.writes ? destination_r1 : 0);
    program.add_span(base, std::array{compute_true | test.field | operands, std::uint64_t{0}});
    program.add_span(base + conditional_offset,
                     std::array{compute_eq | test.field | operands, std::uint64_t{0}});
    variant.entries.insert(variant.entries.end(), {base, base + conditional_offset});
  }
  const auto strict = fixture::compile(program.code, sharc::ExecutionMode::strict);
  const auto fast = fixture::compile(program.code, sharc::ExecutionMode::fast_fp32);
  fixture::check(strict.report().identity != fast.report().identity,
                 "floating modes share a prepared image identity");
  for (const auto mode : fixture::numeric_modes) {
    const bool fast_mode = mode == sharc::ExecutionMode::fast_fp32;
    sharc::Core core(program.memory(), mode);
    (void)core.install(fast_mode ? fast : strict, fixture::Program::variant_id);
    verify_compare_sequence(core, fast_mode);
    for (const auto &test : fixture::floating_cases) {
      if (test.fast != fast_mode) { continue; }
      const auto address = addresses.at(test.field);
      for (const auto mode1 : {0U, alternate_low, simd_mode, simd_mode | alternate_low}) {
        verify(core, test, {.address = address, .conditional = false}, mode1);
        verify(core, test, {.address = address + conditional_offset, .conditional = true}, mode1);
      }
    }
  }
}

void verify_packet(sharc::Core &core,
                   const fixture::FloatingPacket &packet,
                   std::uint32_t address) {
  constexpr std::array registers{0U, 4U, 8U, 12U};
  constexpr std::array accumulators{sharc::Accumulator{.low = 0x12345678},
                                    sharc::Accumulator{.middle = 0x87654321}};
  const auto bank = [&](unsigned reg) {
    return (packet.mode1 & (reg < 8 ? alternate_low : alternate_high)) != 0 ? 1U : 0U;
  };
  sharc::Registers initial;
  initial.pc = address;
  initial.mode1 = packet.mode1;
  for (std::size_t index = 0; index < initial.pe.size(); ++index) {
    const auto &reference = packet.lanes[index];
    auto &pe = initial.pe[index];
    for (auto &data : pe.data) { data.fill(raw_seed); }
    pe.multiplier = accumulators;
    for (std::size_t source = 0; source < registers.size(); ++source) {
      const auto reg = registers[source];
      pe.data[bank(reg)][reg] = reference.inputs[source];
    }
    pe.astat = reference.old_astat;
    pe.stky = reference.old_stky;
  }
  auto expected = initial;
  for (std::size_t index = 0; index < expected.pe.size(); ++index) {
    const auto &reference = packet.lanes[index];
    auto &pe = expected.pe[index];
    for (std::size_t destination = 0; destination < reference.result.size(); ++destination) {
      const auto reg = registers[destination];
      pe.data[bank(reg)][reg] = reference.result[destination];
    }
    pe.astat = reference.astat;
    pe.stky = reference.stky;
  }
  core.set_registers(initial);
  const auto start = core.save();
  for (const auto instructions : {1U, 2U}) {
    core.restore(start);
    fixture::boundary(expected, {.pc = address + instructions, .clock = instructions});
    if (!fixture::completed(core.run(instructions), instructions, instructions) ||
        core.registers() != expected) {
      std::ostringstream detail;
      detail << std::hex << "floating packet=0x" << packet.word << " MODE1=0x" << packet.mode1
             << " FastFP32=" << packet.fast;
      fixture::check(false, detail.str());
    }
  }
}

void floating_register_move() {
  // IF EQ F0 = F0 * F4, F8 = F8 + F12, F4 = F8 - F12, R0 = R4.
  constexpr std::uint64_t word = 0x704000740800;
  constexpr std::uint64_t one = 0x3f80000000;
  constexpr std::uint64_t two = 0x4000000000;
  constexpr std::uint64_t three = 0x4040000000;
  constexpr std::uint64_t four = 0x4080000000;
  constexpr std::uint64_t five = 0x40a0000000;
  constexpr std::uint64_t extension = 0x40;
  constexpr std::uint32_t floating_flag = 0x400;
  fixture::Program program(std::array{word});
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    for (const auto selected : {0U, 1U}) {
      sharc::Registers before;
      before.pc = fixture::code_address;
      before.mode1 = simd_mode | alternate_low;
      for (auto &pe : before.pe) {
        pe.data[1][0] = two;
        pe.data[1][4] = three | extension;
        pe.data[0][8] = four;
        pe.data[0][12] = one;
      }
      before.pe[selected].astat = zero_flag;
      core.set_registers(before);
      auto expected = before;
      auto &pe = expected.pe[selected];
      pe.data[1][0] = three | extension;
      pe.data[1][4] = three;
      pe.data[0][8] = five;
      pe.astat = floating_flag;
      fixture::boundary(expected, {.pc = before.pc + 1, .clock = 1});
      fixture::check(fixture::completed(core.run(1), 1, 1) && core.registers() == expected,
                     "parallel UREG move lost its old raw source, predicate or write priority");
    }
  }
}

void floating_packets() {
  fixture::Program program(std::array{std::uint64_t{0}});
  auto &variant = program.code.variants.front();
  std::map<std::uint64_t, std::uint32_t> entries;
  for (const auto &packet : fixture::floating_packets) {
    const auto offset = static_cast<std::uint32_t>(entries.size() + 1) * entry_stride;
    const auto address = fixture::code_address + offset;
    if (!entries.try_emplace(packet.word, address).second) { continue; }
    program.add_span(address, std::array{packet.word, std::uint64_t{0}});
    variant.entries.push_back(address);
  }
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    for (const auto &packet : fixture::floating_packets) {
      if (packet.fast == (mode == sharc::ExecutionMode::fast_fp32)) {
        verify_packet(core, packet, entries.at(packet.word));
      }
    }
  }
}
} // namespace

int main() {
  return fixture::run_test([] {
    floating();
    floating_packets();
    floating_register_move();
  });
}
