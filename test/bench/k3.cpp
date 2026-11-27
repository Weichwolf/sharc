// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "../support/word_space.hpp"
#include "reference.hpp"
#include "runner.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <span>
#include <string>
#include <vector>

namespace {
using fixture::check;
constexpr unsigned stages = 8;
constexpr std::size_t fields_per_stage = 4;
constexpr std::size_t transforms = 512;
constexpr unsigned word_bytes = sizeof(std::uint32_t);
constexpr unsigned raw_shift = 8;
constexpr std::uint32_t data_address = 0xa0000;
constexpr std::uint32_t twiddle_address = 0xd0000;
constexpr std::uint32_t stage_address = 0xe0000;
constexpr std::uint32_t dm_id = 11;
constexpr std::uint32_t pm_id = 12;
constexpr std::byte guard{0xa5};
using ComplexWords = std::array<std::uint32_t, bench::fft_words>;

constexpr unsigned foreground_sites = 46;
constexpr unsigned butterfly_sites = 23;
constexpr unsigned group_overhead = 8;
constexpr std::size_t stage_overhead = 9;
constexpr unsigned transform_overhead = 5;
constexpr auto butterflies = (bench::fft_size / 2) * stages;
constexpr auto groups = bench::fft_size - 1;
constexpr auto instructions_per_transform = (butterfly_sites * butterflies) +
                                            (group_overhead * groups) + (stage_overhead * stages) +
                                            transform_overhead;
constexpr auto guest_instructions = (instructions_per_transform * transforms) + 1;
// One compute interlock at each group and stage branch. Every taken branch
// incurs three redirect clocks, with one fallthrough per loop.
constexpr unsigned branch_redirect = 3;
constexpr auto cycles_per_transform =
    instructions_per_transform + groups + stages + (branch_redirect * (groups - 1));
constexpr auto guest_cycles =
    (cycles_per_transform * transforms) + (branch_redirect * (transforms - 1)) + 1;

fixture::Program kernel() {
  constexpr std::array<std::uint64_t, foreground_sites> words{
      0x71be8c000000,                 // I8 = I9
      fixture::immediate(14, stages), // R14 = 8
      0x713f88800000,                 // I1 = I3
      0xb00800000000,                 // R8 = PM(0,I8)
      0xb00900000001,                 // R9 = PM(1,I8)
      0xb00a00000002,                 // R10 = PM(2,I8)
      0xb00b00000003,                 // R11 = PM(3,I8)
      0x70bf94000000,                 // M8 = R11
      0x164000000004,                 // MODIFY(I8,4)
      0x713e89000000,                 // I2 = I1
      0x713e86800000,                 // R13 = I1
      fixture::add(13, 13, 10),       // R13 = R13 + R10
      0x71bf8d000000,                 // I10 = I11
      0x70fe88800000,                 // I1 = R13
      0x0d0900000017,                 // LCNTR = R9, DO _butterfly_end UNTIL LCE
      0xb40500000001,                 // R5 = PM(1,I10)
      0x240082000000,                 // R0 = DM(I2,M0), R4 = PM(I10,M8)
      0xa40100000001,                 // R1 = DM(1,I2)
      0xa20200000000,                 // R2 = DM(0,I1)
      0xa20300000001,                 // R3 = DM(1,I1)
      0x013e0017c024,                 // MRF = R2 * R4 (SSF)
      0x013e001fc035,                 // MRF = MRF - R3 * R5 (SSF)
      0x013e00119600,                 // R6 = RND MRF (SF)
      0x013e0017c025,                 // MRF = R2 * R5 (SSF)
      0x013e001bc034,                 // MRF = MRF + R3 * R4 (SSF)
      0x013e00119700,                 // R7 = RND MRF (SF)
      0x013e00022260,                 // R2 = -R6
      0x013e00022370,                 // R3 = -R7
      0x013e00009202,                 // R2 = (R0 + R2)/2
      0x013e00009006,                 // R0 = (R0 + R6)/2
      0x013e00009313,                 // R3 = (R1 + R3)/2
      0x013e00009117,                 // R1 = (R1 + R7)/2
      0xa50000000000,                 // DM(0,I2) = R0
      0xa50100000001,                 // DM(1,I2) = R1
      0xa30200000000,                 // DM(0,I1) = R2
      0xa30300000001,                 // DM(1,I1) = R3
      0x160200000002,                 // MODIFY(I2,2)
      0x160100000002,                 // MODIFY(I1,2)
      0x013e0002a880,                 // R8 = R8 - 1
      0x072000ffffe2,                 // IF NE JUMP _group
      0x013e0002aee0,                 // R14 = R14 - 1
      0x072000ffffd9,                 // IF NE JUMP _stage
      0x013e0002acc0,                 // R12 = R12 - 1
      0x713e89800000,                 // I3 = I1
      0x072000ffffd4,                 // IF NE JUMP _k3
      0x008000000000};                // IDLE
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

ComplexWords signal() {
  constexpr unsigned mask = 4095;
  constexpr int center = 2048;
  constexpr std::int32_t scale = 1 << 16; // Preserve the normalized Q1.15 fixture inputs.
  constexpr unsigned real_step = 97;
  constexpr unsigned imag_step = 53;
  constexpr unsigned imag_start = 7;
  ComplexWords result{};
  for (unsigned i = 0; i < bench::fft_size; ++i) {
    std::size_t reverse{};
    for (unsigned bit = 0; bit < stages; ++bit) { reverse = (reverse << 1) | ((i >> bit) & 1); }
    const auto real = static_cast<int>(((i * real_step) + 13) & mask) - center;
    const auto imag = static_cast<int>(((i * imag_step) + imag_start) & mask) - center;
    result[reverse * 2] = static_cast<std::uint32_t>(real * scale);
    result[(reverse * 2) + 1] = static_cast<std::uint32_t>(imag * scale);
  }
  return result;
}

bench::FftTwiddles twiddle_table() {
  const auto quantize = [](long double x) {
    constexpr auto unit = std::int64_t{1} << 31;
    constexpr auto rail = unit - 1;
    const auto word = static_cast<std::int64_t>(std::llround(x * unit));
    return static_cast<std::uint32_t>(std::clamp(word, -rail, rail));
  };
  bench::FftTwiddles result{};
  for (std::size_t j = 0; j < bench::fft_size / 2; ++j) {
    const auto phase = -2 * std::numbers::pi_v<long double> * j / bench::fft_size;
    result[j * 2] = quantize(std::cos(phase));
    result[(j * 2) + 1] = quantize(std::sin(phase));
  }
  return result;
}

constexpr auto stage_table() {
  constexpr std::uint32_t points = bench::fft_size;
  std::array<std::uint32_t, stages * fields_per_stage> result{};
  for (unsigned stage = 0; stage < stages; ++stage) {
    const auto offset = stage * fields_per_stage;
    const auto half = 1U << stage;
    const auto half_words = 2 * half;
    result[offset] = points / half_words;
    result[offset + 1] = half;
    result[offset + 2] = half_words;
    result[offset + 3] = points / half;
  }
  return result;
}

sharc::Registers initial_registers() {
  sharc::Registers registers;
  registers.pc = fixture::code_address;
  registers.dag[0][3].index = data_address;
  registers.dag[0][11].index = twiddle_address;
  registers.dag[0][9].index = stage_address;
  registers.pe[0].data[0][12] = std::uint64_t{transforms} << raw_shift;
  return registers;
}

void validate_progress(const sharc::Core &core, const sharc::RunResult &result) {
  check(!result.has_fault, result.fault.detail);
  const auto registers = core.registers();
  check(fixture::completed(result, guest_instructions, guest_cycles) &&
            result.execution == sharc::ExecutionState::sleeping &&
            core.state().progress == result.progress && registers.emuclk == guest_cycles &&
            registers.pc == fixture::code_address + foreground_sites &&
            registers.dag[0][3].index == data_address + (transforms * bench::fft_words) &&
            registers.pe[0].data[0][12] == 0 && registers.pc_stack_pointer == 0 &&
            registers.loop_address_depth == 0 && registers.loop_counter_depth == 0,
        "K3 final state or progress differs: " + std::to_string(result.progress.instructions) +
            "/" + std::to_string(result.progress.base_cycles));
}

void run(sharc::ExecutionMode mode, bench::Options options) {
  const auto input = signal();
  const auto twiddles = twiddle_table();
  constexpr auto descriptors = stage_table();
  auto expected = input;
  bench::fft(expected, twiddles);
  const bench::WordSpace dm;
  const bench::WordSpace pm;
  pm.write_words(twiddle_address, twiddles);
  pm.write_words(stage_address, descriptors);
  pm.protect_read_only();
  auto program = kernel();
  auto memory = program.memory(dm_id, dm.all());
  memory.regions.push_back({.storage_id = pm_id, .bytes = pm.all(), .generation = 1});
  const auto native = fixture::compile(program.code, mode, options.memory_mode);
  sharc::Core core(memory, mode);
  fixture::install(core, native, initial_registers());
  const auto initial = core.save();
  constexpr std::uint32_t total_words = transforms * bench::fft_words;
  const auto reset = [&] {
    core.restore(initial);
    for (std::uint32_t offset = 0; offset < total_words; offset += bench::fft_words) {
      dm.write_words(data_address + offset, input);
    }
    for (const auto address : {data_address - 1, data_address + total_words}) {
      std::ranges::fill(dm.at(address, word_bytes), guard);
    }
  };
  reset();
  std::vector<ComplexWords> portable(transforms);
  bench::sample(
      {.name = "K3",
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
        for (std::uint32_t offset = 0; offset < total_words; offset += bench::fft_words) {
          dm.check_words(data_address + offset,
                         expected,
                         "K3 guest output differs from the complete C++ transform");
        }
        dm.check_guards(
            data_address - 1, data_address + total_words, guard, "K3 guest changed a data guard");
        pm.check_words(twiddle_address, twiddles, "K3 guest changed its twiddles");
        pm.check_words(stage_address, descriptors, "K3 guest changed its stage descriptors");
      },
      [&] {
        std::ranges::fill(portable, input);
        const auto start = bench::Clock::now();
        for (auto &values : portable) { bench::fft(values, twiddles); }
        const auto end = bench::Clock::now();
        check(std::ranges::all_of(portable, [&](const auto &values) { return values == expected; }),
              "K3 timed C++ output differs");
        return bench::nanoseconds(start, end);
      });
}
} // namespace

int main(int argc, char **argv) {
  return bench::run_modes(argc, argv, run);
}
