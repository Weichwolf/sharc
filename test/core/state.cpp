// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <sharc/core.hpp>
#include <stdexcept>

namespace {
constexpr std::uint64_t data_seed = 0x8123456701;
constexpr unsigned accumulator_low = 0xdeadbeef;
constexpr unsigned accumulator_high = 0x8123;
constexpr unsigned fifo_occupancy = 63;
constexpr unsigned execute_pc = 0x123456;
constexpr unsigned fetch_pc = 0x12345a;
constexpr unsigned decode_pc = 0x123458;
constexpr unsigned pc_stack_mask = 0x3ffffff;
constexpr unsigned full_pc_stack = 30;
constexpr unsigned instruction_limit = 100;
constexpr std::uint64_t exchange_bits = 0xfedcba9876543210;
constexpr unsigned mode1_bits = 0x01a00400;
constexpr unsigned mask_bits = 0x1234;
constexpr unsigned flag_bits = 0x5678;
constexpr unsigned timer_period = 0xffffffff;
constexpr unsigned timer_count = 123;
constexpr std::uint64_t clock_bits = 0x123456789abcdef0;
}

int main() try {
  using fixture::check;
  sharc::Core core;
  sharc::Registers registers;
  std::uint64_t pattern = data_seed;
  for (auto &pe : registers.pe) {
    for (auto &bank : pe.data) {
      for (auto &value : bank) { value = pattern++; }
    }
    for (auto &accumulator : pe.multiplier) {
      accumulator = {.low = accumulator_low,
                     .middle = static_cast<std::uint32_t>(pattern++),
                     .high = accumulator_high};
    }
    pe.bit_fifo = pattern++ << 16;
    pe.bit_fifo_write_pointer = fifo_occupancy;
    pe.astat = static_cast<std::uint32_t>(pattern++);
    pe.stky = static_cast<std::uint32_t>(pattern++);
  }
  for (auto &bank : registers.dag) {
    for (auto &dag : bank) {
      dag.index = static_cast<std::uint32_t>(pattern++);
      dag.modifier = static_cast<std::uint32_t>(pattern++);
      dag.length = static_cast<std::uint32_t>(pattern++);
      dag.base = static_cast<std::uint32_t>(pattern++);
    }
  }
  registers.pc = execute_pc;
  registers.faddr = fetch_pc;
  registers.daddr = decode_pc;
  for (auto &pc : registers.pc_stack) {
    pc = static_cast<std::uint32_t>(pattern++) & pc_stack_mask;
  }
  registers.pc_stack_pointer = full_pc_stack;
  registers.loop_address_stack = {1, 2, 3, 4, 5, 6};
  registers.loop_counter_stack = {7, 8, 9, 10, 11, 12};
  registers.loop_address_depth = 6;
  registers.loop_counter_depth = 5;
  for (auto &frame : registers.status_stack) {
    frame = {.mode1 = static_cast<std::uint32_t>(pattern++),
             .astat_x = static_cast<std::uint32_t>(pattern++),
             .astat_y = static_cast<std::uint32_t>(pattern++)};
  }
  registers.status_depth = 15;
  registers.lcntr = instruction_limit;
  registers.px = exchange_bits;
  registers.mode1 = mode1_bits;
  registers.mode2 = 0x00000020;
  registers.mmask = mask_bits;
  registers.flags = flag_bits;
  registers.irptl = 0x100;
  registers.imask = 0x200;
  registers.imaskp = 0x400;
  registers.lirptl = 0x800;
  registers.ustat = {0x1000, 0x2000, 0x4000, 0x8000};
  registers.tperiod = timer_period;
  registers.tcount = timer_count;
  registers.emuclk = clock_bits;
  core.set_registers(registers);
  check(core.registers() == registers, "register inspection lost architectural bits or banks");
  const auto saved_state = core.state();
  const auto snapshot = core.save();
  core.set_registers({});
  check(core.registers() == sharc::Registers{}, "set_registers retained prior bank state");
  const auto result = core.run(instruction_limit);
  check(result.has_fault && result.fault.reason == sharc::FaultReason::unprepared &&
            result.progress == sharc::Progress{} && core.state().progress == sharc::Progress{},
        "unprepared execution fabricated progress");
  core.restore(snapshot);
  check(core.state() == saved_state, "restore retained the later fault or register writes");
  core.restore(snapshot);
  check(core.state() == saved_state, "snapshot was consumed or mutated by restore");
  sharc::Core second;
  second.restore(snapshot);
  check(second.state() == saved_state, "snapshot depends on its originating Core address");
  sharc::Core fast({}, sharc::ExecutionMode::fast_fp32);
  fixture::throws<std::invalid_argument>([&] { fast.restore(snapshot); },
                                         "cross-mode restore accepted");
  fixture::throws<std::invalid_argument>([&] { core.restore({}); }, "empty snapshot accepted");
  registers.pe[1].data[1][15] = 0x10000000000;
  fixture::throws<std::invalid_argument>([&] { core.set_registers(registers); },
                                         "oversized raw register accepted");
  check(core.state() == saved_state, "failed register replacement partially changed state");
  const auto declined = core.run_until(1000);
  check(declined.has_fault && declined.progress == sharc::Progress{},
        "unprepared deadline retired code");
  const auto faulted = core.state();
  core.set_memory({});
  check(core.state() == faulted, "storage rebinding changed guest state");
  fixture::throws<sharc::PreparationError>([&] { (void)core.install({}, 7); },
                                           "empty install accepted");
  fixture::throws<sharc::PreparationError>([] { (void)sharc::SemanticImage{}.artifact(); },
                                           "empty semantic image serialized");
  fixture::throws<sharc::PreparationError>([] { (void)sharc::NativeImage{}.report(); },
                                           "empty native image reports success");
  fixture::throws<sharc::PreparationError>([&] { core.reset(); },
                                           "unimplemented reset reports success");
  check(core.state() == faulted, "failed installation or reset changed guest state");
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
