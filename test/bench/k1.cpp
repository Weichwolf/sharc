// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "../support/word_space.hpp"
#include "reference.hpp"
#include "runner.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/SHA256.h>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using fixture::check;
constexpr unsigned frames = 1024;
constexpr unsigned blocks = 256;
constexpr unsigned samples = frames * blocks;
constexpr unsigned history = bench::fir_taps - 1;
constexpr unsigned word_bytes = sizeof(std::uint32_t);
constexpr unsigned raw_shift = 8;
constexpr std::uint32_t input_address = 0x90000;
constexpr std::uint32_t output_address = 0x100000;
constexpr std::uint32_t coefficient_address = 0xd0000;
constexpr std::uint32_t dm_id = 11;
constexpr std::uint32_t pm_id = 12;
constexpr std::uint32_t circular_enable = 1U << 24;
constexpr std::uint32_t irq2_enable = 1U << 8;
constexpr std::uint32_t interrupt_enable = 1U << 12;
constexpr sharc::InterruptRequest irq{.source = 8, .vector = fixture::code_address + 0x20};
constexpr std::byte guard{0xa5};

// Q1.31 fixture values from the shared DSP56300 benchmark definition.
constexpr std::array<std::uint32_t, bench::fir_taps> coefficients{
    0xfe200000, 0x00400000, 0xfe800000, 0x00a00000, 0xfee00000, 0x01000000, 0xff400000, 0x01600000,
    0xffa00000, 0x01c00000, 0x00000000, 0xfe400000, 0x00600000, 0xfea00000, 0x00c00000, 0xff000000,
    0x01200000, 0xff600000, 0x01800000, 0xffc00000, 0x01e00000, 0x00200000, 0xfe600000, 0x00800000,
    0xfec00000, 0x00e00000, 0xff200000, 0x01400000, 0xff800000, 0x01a00000, 0xffe00000, 0xfe200000,
    0x00400000, 0xfe800000, 0x00a00000, 0xfee00000, 0x01000000, 0xff400000, 0x01600000, 0xffa00000,
    0x01c00000, 0x00000000, 0xfe400000, 0x00600000, 0xfea00000, 0x00c00000, 0xff000000, 0x01200000,
    0xff600000, 0x01800000, 0xffc00000, 0x01e00000, 0x00200000, 0xfe600000, 0x00800000, 0xfec00000,
    0x00e00000, 0xff200000, 0x01400000, 0xff800000, 0x01a00000, 0xffe00000, 0xfe200000, 0x00400000};

constexpr unsigned foreground_sites = 18;
constexpr unsigned handler_sites = 2;
constexpr unsigned sample_overhead = 11;
constexpr unsigned block_overhead = 3;
constexpr std::uint64_t guest_instructions =
    ((bench::fir_taps + sample_overhead) * samples) + (std::uint64_t{blocks} * block_overhead) + 2;
// Each DO has one empty clock and each taken branch has three redirect clocks.
// Independent work between the DEC and NE avoids an ALU interlock.
constexpr std::uint64_t branch_redirect = 3;
constexpr std::uint64_t guest_cycles =
    guest_instructions + samples + (branch_redirect * (samples - 1));
constexpr std::uint64_t horizon_cycles = 256;
constexpr std::uint64_t irq_period = 4096;
constexpr std::uint64_t irq_service_cycles = 4 + 1 + 4; // Entry, INC, RTI.
// The kth request must precede the final clock after the previous k-1 handlers.
constexpr std::uint64_t expected_irqs =
    (guest_cycles - irq_service_cycles - 1) / (irq_period - irq_service_cycles);

enum class Schedule : std::uint8_t { continuous, bounded, interrupts };

constexpr sharc::Progress expected_progress(Schedule schedule) {
  const auto interrupts = schedule == Schedule::interrupts ? expected_irqs : 0;
  return {.instructions = guest_instructions + (handler_sites * interrupts),
          .base_cycles = guest_cycles + (irq_service_cycles * interrupts)};
}

fixture::Program kernel() {
  constexpr std::array<std::uint64_t, foreground_sites> words{
      fixture::immediate(7, frames), // R7 = 1024
      0x713e09000000,                // I2 = I0
      0x160000000001,                // MODIFY(I0,1)
      0x013e00114000,                // MRF = 0
      0x248092000000,                // R0 = DM(I2,M2), R4 = PM(I10,M10)
      0x0c003f000001,                // LCNTR = 63, DO _tap UNTIL LCE
      0x2480921bc004,                // MRF = MRF + R0 * R4 (SSF), DM/PM loads
      0x013e001bc004,                // MRF = MRF + R0 * R4 (SSF)
      0x013e0011d000,                // MRF = RND MRF (SF)
      0x013e00109300,                // R3 = SAT MRF (SF)
      0xa30300000000,                // DM(0,I1) = R3
      0x013e0002a770,                // R7 = R7 - 1
      0x160100000001,                // MODIFY(I1,1)
      0x072000fffff4,                // IF NE JUMP _sample
      0x013e0002a550,                // R5 = R5 - 1
      fixture::immediate(7, frames), // R7 = 1024
      0x072000fffff1,                // IF NE JUMP _sample
      0x008000000000};               // IDLE
  constexpr std::array<std::uint64_t, handler_sites> handler{0x013e00029660, 0x0b3e00000000};
  fixture::Program program(words);
  program.add_span(irq.vector, handler);
  auto &variant = program.code.variants[0];
  variant.interrupt_vectors.push_back(irq);
  variant.layout.ordinary_regions = true;
  variant.layout.regions.push_back(fixture::normal_region(0, fixture::word_count, dm_id));
  auto pm = fixture::normal_region(0, fixture::word_count, pm_id);
  pm.space = sharc::MemorySpace::program;
  pm.writable = false;
  variant.layout.regions.push_back(pm);
  return program;
}

std::vector<std::uint32_t> signal() {
  constexpr std::uint32_t seed = 0x243f6a88;
  constexpr std::uint32_t multiplier = 1664525;
  constexpr std::uint32_t increment = 1013904223;
  constexpr unsigned pulse_period = 97;
  constexpr auto rail = std::uint32_t{1} << 31;
  std::vector<std::uint32_t> input(samples + history);
  auto state = seed;
  for (unsigned i = 0; i < samples; ++i) {
    state = (state * multiplier) + increment;
    auto sample = static_cast<std::uint32_t>((std::int64_t{state} - rail) / 2);
    if (i % pulse_period == 0) { sample = rail - 1; }
    if (i % pulse_period == 1) { sample = rail; }
    input[i + history] = sample;
  }
  return input;
}

std::string digest(std::span<const std::uint32_t> words) {
  llvm::SHA256 hash;
  for (const auto word : words) {
    std::array<std::uint8_t, word_bytes> bytes{};
    for (unsigned i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<std::uint8_t>(word >> (i * raw_shift));
    }
    hash.update(bytes);
  }
  return llvm::toHex(hash.final(), true);
}

sharc::Registers initial_registers() {
  sharc::Registers registers;
  registers.pc = fixture::code_address;
  registers.mode1 = circular_enable | interrupt_enable;
  registers.imask = irq2_enable;
  registers.dag[0][0].index = input_address + history;
  registers.dag[0][1].index = output_address;
  registers.dag[0][2].modifier = std::numeric_limits<std::uint32_t>::max();
  registers.dag[0][10] = {.index = coefficient_address,
                          .modifier = 1,
                          .length = bench::fir_taps,
                          .base = coefficient_address};
  registers.pe[0].data[0][5] = std::uint64_t{blocks} << raw_shift;
  return registers;
}

bool ended(const sharc::RunResult &result) {
  return result.has_fault || result.execution == sharc::ExecutionState::sleeping;
}

sharc::RunResult step_to_ready(sharc::Core &core, bench::OwnerStats &stats) {
  while (!core.interrupt_ready(irq)) {
    auto result = core.run(1);
    ++stats.calls;
    if (ended(result)) { return result; }
    check(result.progress.instructions == 1, "Pending FIR IRQ makes no progress");
  }
  return {};
}

bench::SampleRun execute(sharc::Core &core, Schedule schedule) {
  if (schedule == Schedule::continuous) {
    return {.result = core.run(guest_instructions), .owner = bench::OwnerStats{.calls = 1}};
  }
  bench::OwnerStats stats;
  auto deadline = horizon_cycles;
  auto next_irq = irq_period;
  sharc::RunResult result;
  for (;;) {
    result = core.run_until(deadline);
    ++stats.calls;
    ++stats.horizons;
    if (ended(result)) { break; }
    const auto now = core.state().progress.base_cycles;
    check(now >= deadline, "FIR returned before its absolute horizon");
    if (schedule == Schedule::interrupts && now >= next_irq) {
      ++stats.requested;
      result = step_to_ready(core, stats);
      if (ended(result)) { break; }
      const auto delay = core.state().progress.base_cycles - next_irq;
      stats.max_delay = std::max(stats.max_delay, delay);
      stats.total_delay += delay;
      core.interrupt(irq);
      ++stats.accepted;
      next_irq += irq_period;
    }
    deadline += horizon_cycles;
  }
  result.progress = core.state().progress;
  return {.result = std::move(result), .owner = stats};
}

void validate_progress(const sharc::Core &core, const bench::SampleRun &run, Schedule schedule) {
  const auto registers = core.registers();
  const auto &result = run.result;
  const auto &owner = run.owner;
  const auto progress = expected_progress(schedule);
  const auto interrupts = schedule == Schedule::interrupts ? expected_irqs : 0;
  const auto horizons = schedule == Schedule::continuous
                            ? 0
                            : (progress.base_cycles + horizon_cycles - 1) / horizon_cycles;
  check(!result.has_fault, result.fault.detail);
  check(owner.requested == interrupts && owner.accepted == interrupts &&
            owner.horizons == horizons && owner.calls >= owner.horizons &&
            owner.max_delay < horizon_cycles &&
            registers.pe[0].data[0][6] == (interrupts << raw_shift),
        "FIR owner schedule or guest IRQ handler count changed");
  check(fixture::completed(result, progress.instructions, progress.base_cycles) &&
            result.execution == sharc::ExecutionState::sleeping &&
            core.state().progress == progress && registers.emuclk == progress.base_cycles &&
            registers.pc == fixture::code_address + foreground_sites &&
            registers.dag[0][0].index == input_address + history + samples &&
            registers.dag[0][1].index == output_address + samples &&
            registers.dag[0][10].index == coefficient_address && registers.pe[0].data[0][5] == 0 &&
            registers.pe[0].data[0][7] == (std::uint64_t{frames} << raw_shift) &&
            registers.pc_stack_pointer == 0 && registers.loop_address_depth == 0 &&
            registers.loop_counter_depth == 0 && registers.status_depth == 0,
        "FIR final state or progress differs from the authored instruction count: " +
            std::to_string(result.progress.instructions) + "/" +
            std::to_string(result.progress.base_cycles));
}

void run(sharc::ExecutionMode mode, bench::Options options) {
  const auto input = signal();
  std::vector<std::uint32_t> expected(samples);
  bench::fir(input, coefficients, expected);
  check(digest(expected) == "e0dd28045a3afa7150f68203e73f97539933139b0823efd64705cc1c63ea7462",
        "K1 independent C++ output does not match the common Q1.31 hash");
  const bench::WordSpace dm;
  const bench::WordSpace pm;
  dm.write_words(input_address, input);
  pm.write_words(coefficient_address, coefficients);
  pm.protect_read_only();
  auto program = kernel();
  auto memory = program.memory(dm_id, dm.all());
  memory.regions.push_back({.storage_id = pm_id, .bytes = pm.all(), .generation = 1});
  const auto native = fixture::compile(program.code, mode, options.memory_mode);
  sharc::Core core(memory, mode);
  fixture::install(core, native, initial_registers());
  const auto initial = core.save();
  const auto clear_output = [&] {
    std::ranges::fill(dm.at(output_address - 1, std::size_t{samples + 2} * word_bytes), guard);
  };
  const auto reset = [&] {
    core.restore(initial);
    clear_output();
  };
  std::vector<std::uint32_t> portable(samples);
  const auto reference = [&] {
    const auto start = bench::Clock::now();
    bench::fir(input, coefficients, portable);
    const auto end = bench::Clock::now();
    check(portable == expected, "FIR timed C++ output differs");
    return bench::nanoseconds(start, end);
  };
  for (const auto schedule : {Schedule::continuous, Schedule::bounded, Schedule::interrupts}) {
    constexpr std::array<std::string_view, 3> schedule_names{"continuous", "bounded", "interrupts"};
    reset();
    bench::sample_execution(
        {.name = schedule == Schedule::interrupts ? "K6" : "K1",
         .mode = mode,
         .memory_mode = options.memory_mode,
         .progress = expected_progress(schedule),
         .prepared_instructions = foreground_sites + handler_sites},
        native,
        options.measure,
        reset,
        [&](const bench::SampleRun &result) {
          validate_progress(core, result, schedule);
          dm.check_words(
              output_address, expected, "Guest FIR differs from the complete Q1.31 output");
          dm.check_words(input_address, input, "Guest FIR changed input history or samples");
          dm.check_guards(output_address - 1,
                          output_address + samples,
                          guard,
                          "Guest FIR changed an output guard");
          pm.check_words(coefficient_address, coefficients, "Guest FIR changed its coefficients");
        },
        reference,
        [&] { return execute(core, schedule); },
        schedule_names[static_cast<unsigned>(schedule)]);
  }
}
} // namespace

int main(int argc, char **argv) {
  return bench::run_modes(argc, argv, run);
}
