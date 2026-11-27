// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "fixed_cases.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <sharc/core.hpp>
#include <string_view>

namespace {
struct Case {
  std::string_view name;
  std::uint32_t field;
  bool legal;
};

// Hand-authored examples use distinct destinations and sources. No production
// opcode table or decoding helper participates in these expectations.
constexpr std::array cases{
    Case{.name = "fixed add", .field = 0x001123, .legal = true},
    Case{.name = "floating add", .field = 0x081123, .legal = true},
    Case{.name = "fixed COMP", .field = 0x00a023, .legal = true},
    Case{.name = "FIX with scaling", .field = 0x0d9123, .legal = true},
    Case{.name = "ALU hole next to XOR/NOT", .field = 0x044123, .legal = false},
    Case{.name = "reserved third compute unit", .field = 0x301123, .legal = false},
    Case{.name = "fractional rounded multiply", .field = 0x179123, .legal = true},
    Case{.name = "integer rounded multiply", .field = 0x171123, .legal = false},
    Case{.name = "missing plain-multiply destination", .field = 0x172123, .legal = false},
    Case{.name = "integer multiply to MRF", .field = 0x174023, .legal = true},
    Case{.name = "floating multiply", .field = 0x130123, .legal = true},
    Case{.name = "multiplier hole next to floating multiply", .field = 0x131123, .legal = false},
    Case{.name = "SAT MRF integer unsigned", .field = 0x100100, .legal = true},
    Case{.name = "RND MRB", .field = 0x11b100, .legal = true},
    Case{.name = "FDEP shifter opcode colliding with ALU hole", .field = 0x244123, .legal = true},
    Case{.name = "FPACK", .field = 0x290120, .legal = true},
    Case{.name = "ADSP-214xx BITEXT", .field = 0x250120, .legal = true},
    Case{.name = "ASHIFT", .field = 0x204123, .legal = true},
    Case{.name = "unaligned shifter opcode", .field = 0x205123, .legal = false},
    Case{.name = "MR2F read", .field = 0x402100, .legal = true},
    Case{.name = "reserved MR part", .field = 0x403100, .legal = false},
    Case{.name = "MR2B write", .field = 0x416100, .legal = true},
    Case{.name = "multiplier with ALU average", .field = 0x4612e4, .legal = true},
    Case{.name = "multifunction opcode gap", .field = 0x4712e4, .legal = false},
    Case{.name = "floating multiply and FIX", .field = 0x5b12e4, .legal = true},
    Case{.name = "floating multiply and MIN", .field = 0x5f12e4, .legal = true},
    Case{.name = "multiply and dual ALU", .field = 0x6312e4, .legal = true},
    Case{.name = "floating multiply and dual ALU", .field = 0x7312e4, .legal = true},
    Case{.name = "fixed dual ALU high second destination", .field = 0x07f123, .legal = true},
    Case{.name = "floating dual ALU high second destination", .field = 0x0ff123, .legal = true}};

void verify(const sharc::CodeImage &image, bool legal, std::string_view name) {
  try {
    const auto semantic = sharc::Core::lower(image);
    fixture::check(legal && !semantic.bitcode().empty() && !semantic.exports().empty(),
                   "illegal or empty COMPUTE preparation accepted");
    return;
  } catch (const sharc::PreparationError &error) {
    const auto &fault = error.fault();
    const auto expected =
        legal ? sharc::FaultReason::unsupported_instruction : sharc::FaultReason::invalid_encoding;
    fixture::check(fault.reason == expected, name);
    fixture::check(fault.address == fixture::code_address && fault.variant == 7 &&
                       !fault.identity.empty(),
                   "COMPUTE refusal lost guest context");
    if (!legal) {
      fixture::check(fault.detail.contains("COMPUTE"), "missing compute refusal reason");
    }
    return;
  }
}

void computations() {
  constexpr std::uint64_t full_header = 0x013e00000000;
  constexpr std::uint64_t wide_header = 0x018000000000;
  for (const auto &item : cases) {
    for (const auto set : {sharc::InstructionSet::isa, sharc::InstructionSet::visa}) {
      verify(fixture::image(set, full_header | item.field), item.legal, item.name);
    }
    verify(fixture::image(sharc::InstructionSet::visa,
                          wide_header | (std::uint64_t{item.field} << 16),
                          fixture::Width::wide_word),
           item.legal,
           item.name);
  }
  constexpr unsigned compact_prefix = 0xc000;
  constexpr unsigned compact_operands = 0x12;
  for (unsigned opcode = 0; opcode < 16; ++opcode) {
    const auto parcel =
        static_cast<std::uint16_t>(compact_prefix | (opcode << 8) | compact_operands);
    verify(fixture::image(sharc::InstructionSet::visa, {parcel}), true, "compact compute form");
  }
}

void shifts() {
  constexpr std::uint64_t immediate_shift = 0x800000000000;
  constexpr std::uint32_t deposit_immediate = 0x110c23;
  constexpr std::uint32_t reserved_immediate = 0x060c23;
  constexpr std::uint32_t reserved_high_bit = 0x400c23;
  verify(fixture::image(sharc::InstructionSet::isa, immediate_shift | deposit_immediate),
         true,
         "FDEP immediate");
  verify(fixture::image(sharc::InstructionSet::visa, immediate_shift | reserved_immediate),
         false,
         "reserved immediate shifter operation");
  verify(fixture::image(sharc::InstructionSet::isa, immediate_shift | reserved_high_bit),
         false,
         "reserved immediate-shift bit 22");
}

sharc::Accumulator accumulator(const std::array<std::uint32_t, 3> &limbs) {
  return {.low = limbs[0], .middle = limbs[1], .high = static_cast<std::uint16_t>(limbs[2])};
}

void fixed_multiplier() {
  constexpr std::uint64_t compute_true = 0x013e00000000;
  constexpr std::uint32_t simd = 1U << 21;
  constexpr std::uint32_t swap = 1U << 2;
  constexpr sharc::Accumulator other{.low = 0x89abcdef, .middle = 0x01234567, .high = 0xff80};
  std::array<std::uint64_t, fixture::multiplier_cases.size()> words{};
  for (std::size_t i = 0; i < words.size(); ++i) {
    words[i] = compute_true | fixture::multiplier_cases[i].field;
  }
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    for (const auto mode1 : {0U, swap, simd, simd | swap}) {
      core.reset(fixture::code_address);
      for (const auto &test : fixture::multiplier_cases) {
        const auto bank = test.bank ^ ((mode1 & swap) != 0 ? 1U : 0U);
        auto initial = core.registers();
        initial.mode1 = mode1;
        for (std::size_t lane = 0; lane < initial.pe.size(); ++lane) {
          const auto &values = test.lanes[lane];
          auto &pe = initial.pe[lane];
          pe.data[0][0] = values.old_register;
          pe.data[0][1] = values.left;
          pe.data[0][2] = values.right;
          pe.multiplier[bank] = accumulator(values.old_mr);
          pe.multiplier[bank ^ 1U] = other;
          pe.astat = values.old_astat;
          pe.stky = values.old_stky;
        }
        core.set_registers(initial);
        auto expected = initial;
        const auto active = (mode1 & simd) != 0 ? expected.pe.size() : 1U;
        for (std::size_t lane = 0; lane < active; ++lane) {
          const auto &values = test.lanes[lane];
          auto &pe = expected.pe[lane];
          pe.data[0][0] = values.result_register;
          pe.multiplier[bank] = accumulator(values.result_mr);
          pe.astat = values.astat;
          pe.stky = values.stky;
        }
        fixture::boundary(expected, {.pc = initial.pc + 1, .clock = initial.emuclk + 1});
        const auto deadline = core.state().progress.base_cycles + 1;
        fixture::check(fixture::completed(core.run_until(deadline), 1, 1) &&
                           core.registers() == expected,
                       test.name);
      }
    }
  }
}

void fixed_multifunction() {
  constexpr std::uint32_t swap = 1U << 2;
  std::array<std::uint64_t, fixture::multifunction_cases.size()> words{};
  for (std::size_t i = 0; i < words.size(); ++i) {
    words[i] = fixture::multifunction_cases[i].word;
  }
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    (void)core.install(native, fixture::Program::variant_id);
    core.reset(fixture::code_address);
    for (const auto &test : fixture::multifunction_cases) {
      auto initial = core.registers();
      initial.mode1 = test.mode1;
      const auto bank = (test.mode1 & swap) != 0 ? 1U : 0U;
      auto expected = initial;
      for (std::size_t lane = 0; lane < initial.pe.size(); ++lane) {
        const auto &reference = test.lanes[lane];
        auto &pe = initial.pe[lane];
        pe.data = reference.old_registers;
        pe.multiplier[bank] = accumulator(reference.old_mr);
        pe.astat = reference.old_astat;
        pe.stky = reference.old_stky;
        auto &finished = expected.pe[lane];
        finished.data = reference.result_registers;
        finished.multiplier[bank] = accumulator(reference.result_mr);
        finished.astat = reference.astat;
        finished.stky = reference.stky;
      }
      core.set_registers(initial);
      fixture::boundary(expected, {.pc = initial.pc + 1, .clock = initial.emuclk + 1});
      fixture::check(fixture::completed(core.run(1), 1, 1) && core.registers() == expected,
                     test.name);
    }
  }
}

void accumulator_continuation() {
  constexpr std::uint64_t clear = 0x013e00114000;
  constexpr std::uint64_t mac = 0x013e001bc012;
  constexpr std::uint64_t clear_if_eq = 0x010000114000;
  constexpr std::uint64_t read_mr1 = 0x013e00401300;
  constexpr std::uint64_t half = 0x4000000000;
  constexpr std::uint32_t simd = 1U << 21;
  const std::array words{clear, mac, mac, clear_if_eq, read_mr1};
  fixture::Program program(words);
  const auto semantic = sharc::Core::lower(program.code);
  const auto loaded = sharc::Core::load(program.code, {}, semantic.artifact());
  fixture::check(loaded.identity() == semantic.identity(), "MR context failed artifact load");
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  sharc::Registers initial;
  initial.pc = fixture::code_address;
  initial.mode1 = simd;
  for (auto &pe : initial.pe) {
    pe.data[0][1] = half;
    pe.data[0][2] = half;
  }
  fixture::install(core, native, initial);
  const auto start = core.save();
  fixture::check(fixture::completed(core.run(words.size()), words.size(), words.size()),
                 "whole MAC sequence failed");
  for (const auto &pe : core.registers().pe) {
    constexpr sharc::Accumulator expected{.low = 0, .middle = 0x40000000, .high = 0};
    fixture::check(pe.multiplier[0] == expected && pe.data[0][3] == half,
                   "MAC sequence lost the old accumulator or retired a false clear");
  }
  const auto final = core.state();
  core.restore(start);
  for (std::uint64_t deadline = 1; deadline <= words.size(); ++deadline) {
    fixture::check(fixture::completed(core.run_until(deadline), 1, 1),
                   "MAC sequence failed at an absolute deadline");
  }
  fixture::check(core.state() == final, "MAC sequence changed across owner spans and replay");
}
} // namespace

int main() {
  return fixture::run_test([] {
    computations();
    shifts();
    fixed_multiplier();
    fixed_multifunction();
    accumulator_continuation();
  });
}
