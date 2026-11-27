// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "../support/word_space.hpp"
#include "filter.hpp"
#include "reference.hpp"
#include "runner.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <string>
#include <vector>

namespace {
using fixture::check;
constexpr auto frames = bench::filter_frames;
constexpr auto blocks = bench::filter_blocks;
constexpr auto samples = bench::filter_samples;
constexpr unsigned word_bytes = sizeof(std::uint32_t);
constexpr unsigned raw_shift = 8;
constexpr std::uint32_t input_address = 0x90000;
constexpr std::uint32_t output_address = 0x100000;
constexpr std::uint32_t state_address = 0x150000;
constexpr std::uint32_t coefficient_address = 0xd0000;
constexpr std::uint32_t dm_id = 11;
constexpr std::uint32_t pm_id = 12;
constexpr std::uint32_t circular_enable = 1U << 24;
constexpr std::byte guard{0xa5};

// Q1.31 fixture values from the shared DSP56300 benchmark definition.
constexpr bench::BiquadCoefficients coefficients{{
    {0x20000000, 0x10000000, 0x08000000, 0xc0000000, 0x10000000},
    {0x40000000, 0xe0000000, 0x10000000, 0x20000000, 0x10000000},
    {0x10000000, 0x20000000, 0x10000000, 0xa0000000, 0x20000000},
    {0x40000000, 0x20000000, 0x10000000, 0xc0000000, 0x20000000},
}};
constexpr bench::BiquadState initial_state{
    0x7fffffff, 0x80000000, 0x80000000, 0x7fffffff, 0x60000000, 0xc0000000, 0xa0000000, 0x40000000};
constexpr unsigned foreground_sites = 38;
constexpr unsigned stage_sites = 26;
constexpr unsigned sample_overhead = 7;
constexpr unsigned block_overhead = 3;
constexpr std::uint64_t branch_redirect = 3;
constexpr std::uint64_t guest_instructions =
    ((stage_sites * bench::biquad_sections + sample_overhead) * samples) +
    (std::uint64_t{blocks} * block_overhead) + 2;
constexpr std::uint64_t guest_cycles = guest_instructions + (branch_redirect * (samples - 1));
constexpr std::uint32_t coefficient_words = coefficients.size() * coefficients[0].size();

fixture::Program kernel() {
  constexpr std::array<std::uint64_t, foreground_sites> words{
      fixture::immediate(8, frames), // R8 = 1024
      0xa00000000000,                // R0 = DM(0,I0)
      0x160000000001,                // MODIFY(I0,1)
      0x0c000400001a,                // LCNTR = 4, DO _stage_end UNTIL LCE
      0x248292000000,                // R1 = DM(I2,M2), R4 = PM(I10,M10)
      0x268492800000,                // R2 = DM(I3,M2), R5 = PM(I10,M10)
      0x013e00114000,                // MRF = 0
      0x013e00411100,                // MR1F = R1
      0x013e001bc004,                // MRF = MRF + R0 * R4 (SSF)
      0xb40600000000,                // R6 = PM(0,I10)
      0xb40400000001,                // R4 = PM(1,I10)
      0xb40700000002,                // R7 = PM(2,I10)
      0x013e0011d000,                // MRF = RND MRF (SF)
      0x013e00109300,                // R3 = SAT MRF (SF)
      0x013e00114000,                // MRF = 0
      0x013e00411200,                // MR1F = R2
      0x013e001bc005,                // MRF = MRF + R0 * R5 (SSF)
      0x013e001fc034,                // MRF = MRF - R3 * R4 (SSF)
      0x013e0011d000,                // MRF = RND MRF (SF)
      0x013e00109100,                // R1 = SAT MRF (SF)
      0xa50100000000,                // DM(0,I2) = R1
      0x013e0017c006,                // MRF = R0 * R6 (SSF)
      0x013e001fc037,                // MRF = MRF - R3 * R7 (SSF)
      0x013e0011d000,                // MRF = RND MRF (SF)
      0x013e00109200,                // R2 = SAT MRF (SF)
      0xa70200000000,                // DM(0,I3) = R2
      0x160200000002,                // MODIFY(I2,2)
      0x160300000002,                // MODIFY(I3,2)
      0x164200000003,                // MODIFY(I10,3)
      0x013e00021030,                // R0 = PASS R3
      0xa30000000000,                // DM(0,I1) = R0
      0x013e0002a880,                // R8 = R8 - 1
      0x160100000001,                // MODIFY(I1,1)
      0x072000ffffe0,                // IF NE JUMP _sample
      0x013e0002a990,                // R9 = R9 - 1
      fixture::immediate(8, frames), // R8 = 1024
      0x072000ffffdd,                // IF NE JUMP _sample
      0x008000000000};               // IDLE
  fixture::Program program(words);
  auto &layout = program.code.variants[0].layout;
  layout.ordinary_regions = true;
  layout.regions.push_back(fixture::normal_region(0, fixture::word_count, dm_id));
  auto pm = fixture::normal_region(0, fixture::word_count, pm_id);
  pm.space = sharc::MemorySpace::program;
  pm.writable = false;
  layout.regions.push_back(pm);
  return program;
}

sharc::Registers initial_registers() {
  sharc::Registers registers;
  registers.pc = fixture::code_address;
  registers.mode1 = circular_enable;
  registers.dag[0][0].index = input_address;
  registers.dag[0][1].index = output_address;
  registers.dag[0][2] = {
      .index = state_address, .length = initial_state.size(), .base = state_address};
  registers.dag[0][3] = registers.dag[0][2];
  ++registers.dag[0][3].index;
  registers.dag[0][10] = {.index = coefficient_address,
                          .modifier = 1,
                          .length = coefficient_words,
                          .base = coefficient_address};
  registers.pe[0].data[0][9] = std::uint64_t{blocks} << raw_shift;
  return registers;
}

void validate_progress(const sharc::Core &core, const sharc::RunResult &result) {
  check(!result.has_fault, result.fault.detail);
  const auto registers = core.registers();
  check(fixture::completed(result, guest_instructions, guest_cycles) &&
            result.execution == sharc::ExecutionState::sleeping &&
            core.state().progress == result.progress && registers.emuclk == guest_cycles &&
            registers.pc == fixture::code_address + foreground_sites &&
            registers.dag[0][0].index == input_address + samples &&
            registers.dag[0][1].index == output_address + samples &&
            registers.dag[0][2].index == state_address &&
            registers.dag[0][3].index == state_address + 1 &&
            registers.dag[0][10].index == coefficient_address &&
            registers.pe[0].data[0][8] == (std::uint64_t{frames} << raw_shift) &&
            registers.pe[0].data[0][9] == 0 && registers.pc_stack_pointer == 0 &&
            registers.loop_address_depth == 0 && registers.loop_counter_depth == 0,
        "K2 final state or progress differs: " + std::to_string(result.progress.instructions) +
            "/" + std::to_string(result.progress.base_cycles));
}

void run(sharc::ExecutionMode mode, bench::Options options) {
  const auto input = bench::filter_signal();
  std::vector<std::uint32_t> expected(samples);
  const auto state = bench::biquads(input, coefficients, initial_state, expected);
  check(bench::word_digest(expected) ==
            "ab87d136a23ea837637b11274be389f0f31473c1d447d4f6fc752e823b20fe3a",
        "K2 independent C++ output does not match the common Q1.31 hash");
  check(bench::word_digest(state) ==
            "7fa60e5ab99a82116e669d32aec3e3af0cba013e494fd3b2af331d553c27e94f",
        "K2 independent C++ state does not match the common Q1.31 hash");
  std::vector<std::uint32_t> pm_words;
  pm_words.reserve(coefficient_words);
  for (const auto &stage : coefficients) {
    pm_words.insert(pm_words.end(), stage.begin(), stage.end());
  }
  const bench::WordSpace dm;
  const bench::WordSpace pm;
  dm.write_words(input_address, input);
  pm.write_words(coefficient_address, pm_words);
  pm.protect_read_only();
  auto program = kernel();
  auto memory = program.memory(dm_id, dm.all());
  memory.regions.push_back({.storage_id = pm_id, .bytes = pm.all(), .generation = 1});
  const auto native = fixture::compile(program.code, mode, options.memory_mode);
  sharc::Core core(memory, mode);
  fixture::install(core, native, initial_registers());
  const auto initial = core.save();
  const auto reset = [&] {
    core.restore(initial);
    std::ranges::fill(dm.at(output_address - 1, std::size_t{samples + 2} * word_bytes), guard);
    std::ranges::fill(dm.at(state_address - 1, (initial_state.size() + 2) * word_bytes), guard);
    dm.write_words(state_address, initial_state);
  };
  reset();
  std::vector<std::uint32_t> portable(samples);
  bench::sample(
      {.name = "K2",
       .mode = mode,
       .memory_mode = options.memory_mode,
       .progress = {.instructions = guest_instructions, .base_cycles = guest_cycles},
       .prepared_instructions = foreground_sites},
      native,
      core,
      options.measure,
      reset,
      [&](const sharc::RunResult &result) {
        validate_progress(core, result);
        dm.check_words(
            output_address, expected, "K2 guest output differs from the complete Q1.31 output");
        dm.check_words(state_address, state, "K2 guest state differs from the eight Q1.31 words");
        dm.check_words(input_address, input, "K2 guest changed input samples");
        dm.check_guards(output_address - 1,
                        output_address + samples,
                        guard,
                        "K2 guest changed an output guard");
        dm.check_guards(state_address - 1,
                        state_address + initial_state.size(),
                        guard,
                        "K2 guest changed a state guard");
        pm.check_words(coefficient_address, pm_words, "K2 guest changed its coefficients");
      },
      [&] {
        const auto start = bench::Clock::now();
        const auto final = bench::biquads(input, coefficients, initial_state, portable);
        const auto end = bench::Clock::now();
        check(portable == expected && final == state, "K2 timed C++ output or final state differs");
        return bench::nanoseconds(start, end);
      });
}
} // namespace

int main(int argc, char **argv) {
  return bench::run_modes(argc, argv, run);
}
