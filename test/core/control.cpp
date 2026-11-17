// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sharc/core.hpp>

namespace {
using fixture::check;
constexpr std::uint32_t final_marker = 17;
constexpr std::uint64_t loop_cycles = 23;
constexpr std::uint64_t raw_seed = 0x81234567ef;

void branch_boundaries() {
  // R0 accumulates three increments; R1 counts down; NE returns to the ADD.
  constexpr std::uint64_t decrement_r1 = 0x013e0002a110;
  const std::array words{fixture::immediate(0, 0),
                         fixture::immediate(1, 3),
                         fixture::immediate(2, 1),
                         fixture::add(0, 0, 2),
                         decrement_r1,
                         std::uint64_t{0x072000fffffe},
                         fixture::immediate(3, final_marker),
                         std::uint64_t{0}};
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  sharc::Registers registers;
  registers.pc = fixture::code_address;
  core.set_registers(registers);
  (void)core.install(native, fixture::Program::variant_id);
  const auto initial = core.save();
  const auto complete = core.run(14);
  registers.pe[0].data[0][0] = 3U << 8;
  registers.pe[0].data[0][2] = 1U << 8;
  registers.pe[0].data[0][3] = final_marker << 8;
  registers.pe[0].astat = 9;
  registers.pc += static_cast<std::uint32_t>(words.size());
  registers.faddr = registers.pc + 4;
  registers.daddr = registers.pc + 2;
  registers.emuclk = loop_cycles;
  check(!complete.has_fault &&
            complete.progress == sharc::Progress{.instructions = 14, .base_cycles = loop_cycles} &&
            core.registers() == registers,
        "native backward branch lost loop-carried state or cycles");
  core.restore(initial);
  check(core.run(5).progress == sharc::Progress{.instructions = 5, .base_cycles = 5},
        "compute prefix before conditional branch differs");
  const auto pending_hazard = core.save();
  sharc::Core split(program.memory());
  (void)split.install(native, fixture::Program::variant_id);
  split.restore(pending_hazard);
  auto replaced = split.registers();
  replaced.pe[1].data[1][15] = raw_seed;
  split.set_registers(replaced);
  const auto after_compute = split.run(1);
  check(!after_compute.has_fault &&
            after_compute.progress == sharc::Progress{.instructions = 1, .base_cycles = 5} &&
            split.registers().pc == fixture::code_address + 3,
        "snapshot lost the compute-to-conditional-branch stall");
  check(split.registers().pe[1].data[1][15] == raw_seed,
        "stopped-owner register replacement lost an inactive raw value");
  core.restore(initial);
  const auto crossed = core.run_until(8);
  check(!crossed.has_fault &&
            crossed.progress == sharc::Progress{.instructions = 6, .base_cycles = 10} &&
            core.registers().pc == fixture::code_address + 3,
        "taken branch did not cross a deadline atomically");
  const auto interrupted = core.save();
  const auto before = core.state();
  check(core.run_until(loop_cycles).progress ==
                sharc::Progress{.instructions = 8, .base_cycles = 13} &&
            core.registers() == registers,
        "continued branch loop differs from uninterrupted execution");
  core.restore(interrupted);
  check(core.state() == before, "branch snapshot retained later loop state");
  sharc::Core second(program.memory());
  (void)second.install(native, fixture::Program::variant_id);
  second.restore(interrupted);
  check(second.run_until(11).progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            second.registers().pc == fixture::code_address + 4,
        "loop snapshot could not continue in another Core");
}

void missing_target() {
  constexpr std::uint32_t missing = 0x123456;
  const std::array words{std::uint64_t{0x063e00123456}};
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  sharc::Registers registers;
  registers.pc = fixture::code_address;
  core.set_registers(registers);
  (void)core.install(native, fixture::Program::variant_id);
  const auto result = core.run(2);
  check(result.has_fault && result.fault.reason == sharc::FaultReason::undeclared_entry &&
            result.fault.address == missing &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 4},
        "missing branch target did not preserve the completed prefix");
}
} // namespace

int main() try {
  branch_boundaries();
  missing_target();
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
