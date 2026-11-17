// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "reference.hpp"
#include "runner.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>

namespace {
using fixture::check;
using Clock = bench::Clock;
constexpr unsigned word_count = 4096, passes = 512;
constexpr unsigned word_bytes = 4, byte_bits = 8, raw_shift = 8;
constexpr std::uint32_t input_address = 0x90000, output_address = 0xa0000, ram_id = 11;
constexpr std::uint32_t expected_checksum = 0xd9dd7e00, last_input = 0xc54bba88;
constexpr std::uint32_t positive_rail = 0x7fffffff;
constexpr std::uint64_t guest_instructions = 14'682'625, guest_cycles = 23'071'742;
constexpr std::byte guard{0xa5};
using Words = std::array<std::uint32_t, word_count>;

// Literal encoding of the authored classic ISA in k5.asm. I0/I1, R3 and R4 are initialized by the
// caller.
fixture::Program kernel() {
  constexpr std::uint64_t load = 0xa00000000000;
  constexpr std::uint64_t store = 0xa30000000000;
  constexpr std::uint64_t modify_i0 = 0x160000000000;
  constexpr std::uint64_t modify_i1 = 0x160100000000;
  constexpr std::uint64_t decrement_r2 = 0x013e0002a220;
  constexpr std::uint64_t decrement_r4 = 0x013e0002a440;
  constexpr std::uint64_t inner_ne = 0x072000fffffa;
  constexpr std::uint64_t outer_ne = 0x072000fffff5;
  const std::array instructions{
      fixture::immediate(2, word_count), // block: R2 = 4096
      load,                              // word: R0 = DM(0,I0)
      fixture::add(3, 3, 0),             // R3 = R3 + R0
      store,                             // DM(0,I1) = R0
      modify_i0 | 1,
      modify_i1 | 1,
      decrement_r2,
      inner_ne,
      modify_i0 | static_cast<std::uint32_t>(-static_cast<std::int32_t>(word_count)),
      modify_i1 | static_cast<std::uint32_t>(-static_cast<std::int32_t>(word_count)),
      decrement_r4,
      outer_ne,
      std::uint64_t{0}};
  return fixture::Program(instructions);
}

Words inputs() {
  constexpr std::uint32_t seed = 0x243f6a88;
  constexpr std::uint32_t multiplier = 1664525;
  constexpr std::uint32_t increment = 1013904223;
  Words result;
  auto value = seed;
  for (auto &word : result) {
    value = (value * multiplier) + increment;
    word = value;
  }
  result[0] = positive_rail; // The first two additions independently establish sticky overflow.
  result[1] = 1;
  check(result.back() == last_input, "K5 input generator changed");
  std::uint64_t sum{};
  for (const auto word : result) { sum += word; }
  check(static_cast<std::uint32_t>(sum * passes) == expected_checksum,
        "K5 checksum anchor changed");
  return result;
}

class Storage : public bench::WordSpace {
public:
  void initialize(const Words &input) const {
    auto target = at(input_address, sizeof(input));
    for (std::size_t i = 0; i < input.size(); ++i) {
      for (unsigned byte = 0; byte < word_bytes; ++byte) {
        target[(i * word_bytes) + byte] = static_cast<std::byte>(input[i] >> (byte * byte_bits));
      }
    }
    clear_output();
  }

  void clear_output() const {
    std::ranges::fill(at(output_address - 1, sizeof(Words) + (std::size_t{2} * word_bytes)), guard);
  }

  void validate(const Words &input) const {
    const auto source = at(input_address, sizeof(input));
    const auto target = at(output_address, sizeof(input));
    check(std::ranges::equal(source, target), "K5 did not copy the complete output");
    for (std::size_t i = 0; i < input.size(); ++i) {
      for (unsigned byte = 0; byte < word_bytes; ++byte) {
        check(source[(i * word_bytes) + byte] ==
                  static_cast<std::byte>(input[i] >> (byte * byte_bits)),
              "K5 changed source storage");
      }
    }
    for (const auto address : {output_address - 1, output_address + word_count}) {
      check(std::ranges::all_of(at(address, word_bytes),
                                [](std::byte byte) { return byte == guard; }),
            "K5 changed an output guard word");
    }
  }
};

void validate(const sharc::Core &core, const sharc::RunResult &result) {
  const auto registers = core.registers();
  check(!result.has_fault && result.execution == sharc::ExecutionState::stopped &&
            result.progress ==
                sharc::Progress{.instructions = guest_instructions, .base_cycles = guest_cycles} &&
            core.state().progress == result.progress &&
            registers.pc == fixture::code_address + 13 && registers.emuclk == guest_cycles &&
            registers.dag[0][0].index == input_address &&
            registers.dag[0][1].index == output_address &&
            registers.pe[0].data[0][0] == (std::uint64_t{last_input} << raw_shift) &&
            registers.pe[0].data[0][2] == 0 && registers.pe[0].data[0][4] == 0 &&
            registers.pe[0].data[0][3] == (std::uint64_t{expected_checksum} << raw_shift) &&
            registers.pe[0].astat == 9 && registers.pe[0].stky == 4,
        "K5 progress, checksum, counters, pointers, status or final PC differs");
}

std::uint64_t reference_run(const Words &input) {
  Words output;
  const auto start = Clock::now();
  const auto checksum = bench::copy_checksum(input, output, passes);
  const auto end = Clock::now();
  check(checksum == expected_checksum && output == input, "K5 native reference output differs");
  return bench::nanoseconds(start, end);
}

void run(sharc::ExecutionMode mode, bool measure) {
  const auto input = inputs();
  const Storage storage;
  storage.initialize(input);
  auto program = kernel();
  program.code.variants[0].layout.regions.push_back({.space = sharc::MemorySpace::data,
                                                     .view = sharc::MemoryView::normal_word,
                                                     .first = 0,
                                                     .count = std::uint64_t{1} << 32,
                                                     .storage_id = ram_id,
                                                     .byte_offset = 0,
                                                     .byte_stride = word_bytes,
                                                     .readable = true,
                                                     .writable = true,
                                                     .initialized = true});
  auto memory = program.memory();
  memory.regions.push_back({.storage_id = ram_id, .bytes = storage.all(), .generation = 1});
  const auto native = sharc::Core::compile(program.code, {.mode = mode, .callbacks = {}});
  sharc::Core core(memory, mode);
  sharc::Registers registers;
  registers.pc = fixture::code_address;
  registers.dag[0][0].index = input_address;
  registers.dag[0][1].index = output_address;
  registers.pe[0].data[0][4] = std::uint64_t{passes} << raw_shift;
  core.set_registers(registers);
  (void)core.install(native, fixture::Program::variant_id);
  const auto initial = core.save();
  bench::sample(
      {.name = "K5",
       .mode = mode,
       .progress = {.instructions = guest_instructions, .base_cycles = guest_cycles},
       .prepared_instructions = 13},
      native,
      core,
      measure,
      [&] {
        core.restore(initial);
        storage.clear_output();
      },
      [&](const sharc::RunResult &result) {
        validate(core, result);
        storage.validate(input);
      },
      [&] { return reference_run(input); });
}
} // namespace

int main(int argc, char **argv) try {
  const bool measure = bench::start(argc, argv);
  run(sharc::ExecutionMode::strict, measure);
  run(sharc::ExecutionMode::fast_fp32, measure);
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
