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

void call_snapshot() {
  constexpr auto base = fixture::code_address;
  constexpr std::uint64_t call = 0x06be00000000;
  constexpr std::uint64_t jump = 0x063e00000000;
  constexpr std::uint64_t rts = 0x0a3e00000000;
  constexpr std::uint32_t call_tag = 1U << 24;
  constexpr std::uint32_t empty = 1U << 22;
  const std::array words{call | (base + 5),
                         fixture::immediate(2, 9),
                         jump | (base + 7),
                         std::uint64_t{0},
                         std::uint64_t{0},
                         rts,
                         std::uint64_t{0},
                         std::uint64_t{0}};
  fixture::Program program(words);
  program.code.variants[0].entries.push_back(base + 6);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  sharc::Registers initial;
  initial.pc = base;
  initial.pe[0].stky = empty;
  core.set_registers(initial);
  (void)core.install(native, fixture::Program::variant_id);
  check(!core.run(1).has_fault, "initial CALL faulted");
  const auto saved = core.save();
  check(!core.run(4).has_fault, "static return did not continue through its caller");
  auto expected = initial;
  expected.pe[0].data[0][2] = 9U << 8;
  expected.pc_stack[0] = (base + 1) | call_tag;
  expected.pc = base + 8;
  expected.faddr = expected.pc + 4;
  expected.daddr = expected.pc + 2;
  constexpr unsigned complete_cycles = 14;
  expected.emuclk = complete_cycles;
  check(core.registers() == expected, "static return changed stack cells or complete state");
  sharc::Core second(program.memory());
  (void)second.install(native, fixture::Program::variant_id);
  second.restore(saved);
  check(!second.run(4).has_fault && second.registers() == expected,
        "snapshot inside a callee depends on its original Core");
  second.restore(saved);
  auto redirected = second.registers();
  redirected.pc_stack[0] = (base + 6) | call_tag;
  second.set_registers(redirected);
  const auto result = second.run(2);
  redirected.pc_stack_pointer = 0;
  redirected.pe[0].stky = empty;
  redirected.pc = base + 7;
  redirected.faddr = redirected.pc + 4;
  redirected.daddr = redirected.pc + 2;
  redirected.emuclk = 9;
  check(!result.has_fault &&
            result.progress == sharc::Progress{.instructions = 2, .base_cycles = 5} &&
            second.registers() == redirected,
        "guarded return ignored an altered architectural return address");
  second.restore(saved);
  redirected = second.registers();
  redirected.pc_stack[0] = (base + 4) | call_tag;
  second.set_registers(redirected);
  check(!second.run(1).has_fault && second.registers().pc == base + 4 &&
            second.registers().pc_stack_pointer == 0,
        "return bound did not finish the architectural pop before stopping");
  const auto fault = second.run(1);
  check(fault.has_fault && fault.progress == sharc::Progress{} &&
            fault.fault.reason == sharc::FaultReason::undeclared_entry &&
            fault.fault.address == base + 4,
        "undeclared altered return acquired an implicit execution entry");
}

void refused_stack_prefix() {
  constexpr auto base = fixture::code_address;
  constexpr std::uint64_t call = 0x06be00000000;
  const std::array words{fixture::immediate(0, 2), call | base};
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  sharc::Registers expected;
  expected.pc = base;
  expected.pc_stack_pointer = sharc::Registers::pc_stack_capacity;
  constexpr std::uint32_t full = 1U << 21;
  expected.pe[0].stky = full;
  core.set_registers(expected);
  (void)core.install(native, fixture::Program::variant_id);
  const auto result = core.run(2);
  expected.pe[0].data[0][0] = 2U << 8;
  ++expected.pc;
  expected.faddr = expected.pc + 4;
  expected.daddr = expected.pc + 2;
  expected.emuclk = 1;
  check(result.has_fault && result.fault.reason == sharc::FaultReason::unsupported_feature &&
            result.fault.address == base + 1 &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            core.registers() == expected,
        "unimplemented stack overflow lost its completed straight-line prefix");
}

void call_stack_status() {
  constexpr auto base = fixture::code_address;
  constexpr std::uint64_t call = 0x06be00000000;
  constexpr std::uint32_t call_tag = 1U << 24;
  constexpr std::uint32_t initial_sticky_x = 0xa5000240;
  constexpr std::uint32_t initial_sticky_y = 0x12345678;

  struct Expected {
    unsigned depth;
    std::uint32_t sticky, interrupt;
  };

  constexpr std::array cases{Expected{.depth = 28, .sticky = 0xa5000240, .interrupt = 0x108},
                             Expected{.depth = 29, .sticky = 0xa5200240, .interrupt = 0x100}};
  const std::array words{call | (base + 1), std::uint64_t{0}};
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  for (const auto &row : cases) {
    sharc::Core core(program.memory());
    sharc::Registers expected;
    expected.pc = base;
    expected.pc_stack_pointer = static_cast<std::uint8_t>(row.depth);
    expected.irptl = 0x100;
    expected.pe[0].stky = initial_sticky_x;
    expected.pe[1].stky = initial_sticky_y;
    core.set_registers(expected);
    (void)core.install(native, fixture::Program::variant_id);
    const auto result = core.run(1);
    expected.pc_stack[row.depth] = (base + 1) | call_tag;
    ++expected.pc_stack_pointer;
    expected.irptl = row.interrupt;
    expected.pe[0].stky = row.sticky;
    ++expected.pc;
    expected.faddr = expected.pc + 4;
    expected.daddr = expected.pc + 2;
    expected.emuclk = 4;
    check(!result.has_fault &&
              result.progress == sharc::Progress{.instructions = 1, .base_cycles = 4} &&
              core.registers() == expected,
          "CALL full/empty status, almost-full interrupt or unrelated bits differ");
  }
}

void unsupported_returns() {
  constexpr auto base = fixture::code_address;
  constexpr std::uint64_t rts_eq = 0x0a0000000000;
  constexpr std::uint32_t empty = 1U << 22;
  constexpr std::uint32_t loop_tag = 1U << 25;
  const std::array words{rts_eq, std::uint64_t{0}};
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  for (const unsigned depth : {0U, 1U, 31U}) {
    sharc::Core core(program.memory());
    sharc::Registers expected;
    expected.pc = base;
    expected.pc_stack_pointer = static_cast<std::uint8_t>(depth);
    expected.pc_stack[0] = loop_tag | (base + 1);
    expected.pe[0].stky = empty;
    core.set_registers(expected);
    (void)core.install(native, fixture::Program::variant_id);
    const auto skipped = core.run(1);
    ++expected.pc;
    expected.faddr = expected.pc + 4;
    expected.daddr = expected.pc + 2;
    expected.emuclk = 1;
    check(!skipped.has_fault &&
              skipped.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
              core.registers() == expected,
          "false RTS predicate touched or inspected an unsupported stack state");
    expected.pc = base;
    expected.pe[0].astat = 1;
    core.set_registers(expected);
    const auto saved = core.registers();
    const auto refused = core.run(1);
    check(refused.has_fault && refused.fault.reason == sharc::FaultReason::unsupported_feature &&
              refused.fault.address == base && refused.progress == sharc::Progress{} &&
              core.registers() == saved,
          "unimplemented empty/overflowed/loop-tagged RTS changed architectural state");
  }
}

void skipped_full_call() {
  constexpr auto base = fixture::code_address;
  constexpr std::uint64_t call_eq = 0x068000000000;
  constexpr std::uint32_t full = 1U << 21;
  const std::array words{call_eq | (base + 1), std::uint64_t{0}};
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  sharc::Registers expected;
  expected.pc = base;
  expected.pc_stack_pointer = sharc::Registers::pc_stack_capacity;
  expected.pe[0].stky = full;
  core.set_registers(expected);
  (void)core.install(native, fixture::Program::variant_id);
  const auto result = core.run(1);
  ++expected.pc;
  expected.faddr = expected.pc + 4;
  expected.daddr = expected.pc + 2;
  expected.emuclk = 1;
  check(!result.has_fault &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            core.registers() == expected,
        "false CALL predicate inspected or changed the full PC stack");
}

void stack_interlock() {
  constexpr auto base = fixture::code_address;
  constexpr std::uint64_t call_eq = 0x068000000000;
  constexpr std::uint64_t rts_eq = 0x0a0000000000;
  constexpr std::uint32_t empty = 1U << 22;
  constexpr std::uint32_t call_tag = 1U << 24;
  for (const bool returning : {false, true}) {
    const auto operation = returning ? rts_eq : call_eq | (base + 3);
    const std::array words{fixture::add(0, 0, 0), operation, std::uint64_t{0}, std::uint64_t{0}};
    fixture::Program program(words);
    program.code.variants[0].entries.push_back(base + 3);
    const auto native = sharc::Core::compile(program.code);
    sharc::Core core(program.memory());
    sharc::Registers expected;
    expected.pc = base;
    expected.pc_stack_pointer = returning ? 1 : 0;
    expected.pe[0].stky = returning ? 0 : empty;
    if (returning) { expected.pc_stack[0] = (base + 3) | call_tag; }
    core.set_registers(expected);
    (void)core.install(native, fixture::Program::variant_id);
    const auto saved = core.save();
    const auto result = core.run(2);
    expected.pe[0].astat = 1;
    expected.pe[0].stky = returning ? empty : 0;
    expected.pc_stack_pointer = returning ? 0 : 1;
    if (!returning) { expected.pc_stack[0] = (base + 2) | call_tag; }
    expected.pc = base + 3;
    expected.faddr = expected.pc + 4;
    expected.daddr = expected.pc + 2;
    expected.emuclk = 6;
    check(!result.has_fault &&
              result.progress == sharc::Progress{.instructions = 2, .base_cycles = 6} &&
              core.registers() == expected,
          "CALL/RTS used stale flags or omitted its compute interlock");
    core.restore(saved);
    check(!core.run(1).has_fault, "compute prefix before CALL/RTS failed");
    const auto continued = core.run_until(2);
    check(!continued.has_fault &&
              continued.progress == sharc::Progress{.instructions = 1, .base_cycles = 5} &&
              core.registers() == expected,
          "CALL/RTS interlock did not survive an exact owner bound");
  }
}
} // namespace

int main() try {
  branch_boundaries();
  missing_target();
  call_snapshot();
  refused_stack_prefix();
  call_stack_status();
  unsupported_returns();
  skipped_full_call();
  stack_interlock();
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
