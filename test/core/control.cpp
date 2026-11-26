// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <span>

namespace {
using fixture::check;
constexpr std::uint32_t final_marker = 17;
constexpr std::uint64_t loop_cycles = 23;
constexpr std::uint64_t raw_seed = 0x81234567ef;
constexpr std::uint64_t rts_eq = 0x0a0000000000;
constexpr std::uint64_t direct_call_eq = 0x068000000000;
constexpr std::uint32_t stack_empty = 1U << 22;
constexpr std::uint32_t stack_full = 1U << 21;

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
  fixture::install(core, native, registers);
  const auto initial = core.save();
  const auto complete = core.run(14);
  registers.pe[0].data[0][0] = 3U << 8;
  registers.pe[0].data[0][2] = 1U << 8;
  registers.pe[0].data[0][3] = final_marker << 8;
  registers.pe[0].astat = 9;
  fixture::boundary(
      registers,
      {.pc = registers.pc + static_cast<std::uint32_t>(words.size()), .clock = loop_cycles});
  check(fixture::completed(complete, 14, loop_cycles) && core.registers() == registers,
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
  check(fixture::completed(after_compute, 1, 5) &&
            split.registers().pc == fixture::code_address + 3,
        "snapshot lost the compute-to-conditional-branch stall");
  check(split.registers().pe[1].data[1][15] == raw_seed,
        "stopped-owner register replacement lost an inactive raw value");
  core.restore(initial);
  const auto crossed = core.run_until(8);
  check(fixture::completed(crossed, 6, 10) && core.registers().pc == fixture::code_address + 3,
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
  fixture::install(core, native, registers);
  const auto result = core.run(2);
  check(result.has_fault && result.fault.reason == sharc::FaultReason::undeclared_entry &&
            result.fault.address == missing &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 4},
        "missing branch target did not preserve the completed prefix");
}

void observed_alu_status() {
  constexpr std::uint64_t add_with_carry = 0x013e00005211;
  constexpr std::uint64_t compare = 0x013e0000a021;
  constexpr std::uint64_t raw_max = 0xffffffff00ULL;
  constexpr std::uint32_t old_history = 0x20000000;
  constexpr std::uint32_t new_history = 0x90000000;
  constexpr std::uint32_t inactive_astat = 0x95012345;
  constexpr std::uint32_t carry_and_zero = 9;
  const std::array words{fixture::add(0, 0, 1), add_with_carry, compare, fixture::add(3, 1, 1)};
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  (void)core.install(native, fixture::Program::variant_id);

  sharc::Registers initial;
  initial.pc = fixture::code_address;
  initial.pe[0].data[0][0] = raw_max;
  initial.pe[0].data[0][1] = 0x100;
  initial.pe[0].astat = old_history;
  initial.pe[1].astat = inactive_astat;
  core.set_registers(initial);
  const auto before = core.save();

  auto expected = initial;
  fixture::boundary(
      expected,
      {.pc = expected.pc + static_cast<std::uint32_t>(words.size()), .clock = words.size()});
  expected.pe[0].data[0][0] = 0;
  expected.pe[0].data[0][2] = 3U << 8;
  expected.pe[0].data[0][3] = 0x200;
  expected.pe[0].astat = new_history;
  const auto complete = core.run(words.size());
  check(!complete.has_fault && core.registers() == expected,
        "admitted ALU sequence lost carry input or comparison history");

  core.restore(before);
  const auto prefix = core.run_until(1);
  check(fixture::completed(prefix, 1, 1) &&
            core.registers().pe[0].astat == (old_history | carry_and_zero),
        "exact prefix did not publish the resolved ALU flags");
  const auto suffix = core.run(words.size() - 1);
  check(!suffix.has_fault && core.registers() == expected,
        "resumed ALU sequence changed carry input or comparison history");
}

void call_snapshot() {
  constexpr auto base = fixture::code_address;
  constexpr std::uint64_t call = 0x06be00000000;
  constexpr std::uint64_t jump = 0x063e00000000;
  constexpr std::uint64_t rts = 0x0a3e00000000;
  constexpr std::uint32_t call_tag = 1U << 24;
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
  initial.pe[0].stky = stack_empty;
  fixture::install(core, native, initial);
  check(!core.run(1).has_fault, "initial CALL faulted");
  const auto saved = core.save();
  check(!core.run(4).has_fault, "static return did not continue through its caller");
  auto expected = initial;
  expected.pe[0].data[0][2] = 9U << 8;
  expected.pc_stack[0] = (base + 1) | call_tag;
  expected.pc = base + 8;
  fixture::fetch_addresses(expected);
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
  redirected.pe[0].stky = stack_empty;
  fixture::boundary(redirected, {.pc = base + 7, .clock = 9});
  check(fixture::completed(result, 2, 5) && second.registers() == redirected,
        "guarded return ignored an altered architectural return address");
  const auto redirected_snapshot = second.save();
  check(!second.run(1).has_fault && second.registers().pc == base + 8,
        "native transfer lost the destination unit continuation");
  sharc::Core resumed(program.memory());
  (void)resumed.install(native, fixture::Program::variant_id);
  resumed.restore(redirected_snapshot);
  check(!resumed.run(1).has_fault && resumed.registers() == second.registers(),
        "snapshot after a native unit transfer retained the source root");
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
  expected.pe[0].stky = stack_full;
  fixture::install(core, native, expected);
  const auto result = core.run(2);
  expected.pe[0].data[0][0] = 2U << 8;
  fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 1});
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
    fixture::install(core, native, expected);
    const auto result = core.run(1);
    expected.pc_stack[row.depth] = (base + 1) | call_tag;
    ++expected.pc_stack_pointer;
    expected.irptl = row.interrupt;
    expected.pe[0].stky = row.sticky;
    fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 4});
    check(fixture::completed(result, 1, 4) && core.registers() == expected,
          "CALL full/empty status, almost-full interrupt or unrelated bits differ");
  }
}

void unsupported_stack_operands() {
  constexpr auto base = fixture::code_address;
  constexpr std::uint32_t loop_tag = 1U << 25;
  for (const bool returning : {true, false}) {
    const std::array words{returning ? rts_eq : direct_call_eq | (base + 1), std::uint64_t{0}};
    fixture::Program program(words);
    const auto native = sharc::Core::compile(program.code);
    constexpr std::array return_depths{0U, 1U, 31U};
    constexpr std::array call_depths{unsigned{sharc::Registers::pc_stack_capacity}};
    const auto depths = returning ? std::span<const unsigned>(return_depths)
                                  : std::span<const unsigned>(call_depths);
    for (const auto depth : depths) {
      sharc::Core core(program.memory());
      sharc::Registers expected;
      expected.pc = base;
      expected.pc_stack_pointer = static_cast<std::uint8_t>(depth);
      expected.pc_stack[0] = returning ? loop_tag | (base + 1) : 0;
      expected.pe[0].stky = returning ? stack_empty : stack_full;
      fixture::install(core, native, expected);
      const auto skipped = core.run(1);
      fixture::boundary(expected, {.pc = base + 1, .clock = 1});
      check(fixture::completed(skipped, 1, 1) && core.registers() == expected,
            "false CALL or RTS predicate inspected an unsupported stack state");
      if (!returning) { continue; }
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
}

void stack_interlock() {
  constexpr auto base = fixture::code_address;
  constexpr std::uint32_t call_tag = 1U << 24;
  for (const bool returning : {false, true}) {
    const auto operation = returning ? rts_eq : direct_call_eq | (base + 3);
    const std::array words{fixture::add(0, 0, 0), operation, std::uint64_t{0}, std::uint64_t{0}};
    fixture::Program program(words);
    program.code.variants[0].entries.push_back(base + 3);
    const auto native = sharc::Core::compile(program.code);
    sharc::Core core(program.memory());
    sharc::Registers expected;
    expected.pc = base;
    expected.pc_stack_pointer = returning ? 1 : 0;
    expected.pe[0].stky = returning ? 0 : stack_empty;
    if (returning) { expected.pc_stack[0] = (base + 3) | call_tag; }
    fixture::install(core, native, expected);
    const auto saved = core.save();
    const auto result = core.run(2);
    expected.pe[0].astat = 1;
    expected.pe[0].stky = returning ? stack_empty : 0;
    expected.pc_stack_pointer = returning ? 0 : 1;
    if (!returning) { expected.pc_stack[0] = (base + 2) | call_tag; }
    fixture::boundary(expected, {.pc = base + 3, .clock = 6});
    check(fixture::completed(result, 2, 6) && core.registers() == expected,
          "CALL/RTS used stale flags or omitted its compute interlock");
    core.restore(saved);
    check(!core.run(1).has_fault, "compute prefix before CALL/RTS failed");
    const auto continued = core.run_until(2);
    check(fixture::completed(continued, 1, 5) && core.registers() == expected,
          "CALL/RTS interlock did not survive an exact owner bound");
  }
}

void stalled_stack_refusal() {
  constexpr auto base = fixture::code_address;
  constexpr unsigned b0_code = 0x40;
  constexpr std::uint32_t ram_address = 0x90000;
  constexpr std::uint32_t ram_id = 11;
  constexpr std::uint32_t value = 0x12345678;
  constexpr std::uint64_t load_r1_i0 = 0xa00100000000;
  constexpr std::uint64_t call = 0x06be00000000;
  std::array<std::byte, 4> ram{};
  for (unsigned byte = 0; byte < ram.size(); ++byte) {
    ram[byte] = static_cast<std::byte>(value >> (byte * 8));
  }
  const std::array words{
      fixture::immediate(b0_code, 0), load_r1_i0, call | (base + 3), std::uint64_t{0}};
  fixture::Program program(words);
  auto region = fixture::normal_region(ram_address, 1, ram_id);
  region.writable = false;
  program.code.variants[0].layout.regions.push_back(region);
  auto memory = program.memory(ram_id, ram);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    for (const bool split : {false, true}) {
      sharc::Core core(memory, mode);
      sharc::Core resumed(memory, mode);
      sharc::Registers expected;
      expected.pc = base;
      expected.pc_stack_pointer = sharc::Registers::pc_stack_capacity;
      expected.pe[0].stky = stack_full;
      expected.dag[0][0] = {.index = ram_address, .modifier = 0, .length = 0, .base = 1};
      fixture::install(core, native, expected);
      (void)resumed.install(native, fixture::Program::variant_id);
      if (split) { check(!core.run(1).has_fault, "DAG-load refusal prefix failed"); }
      resumed.restore(core.save());
      const auto result = resumed.run(4);
      expected.dag[0][0].base = 0;
      expected.pe[0].data[0][1] = std::uint64_t{value} << 8;
      fixture::boundary(expected, {.pc = expected.pc + 2, .clock = 4});
      check(result.has_fault && result.fault.reason == sharc::FaultReason::unsupported_feature &&
                result.fault.address == base + 2 && resumed.registers() == expected &&
                result.progress == sharc::Progress{.instructions = split ? 1U : 2U,
                                                   .base_cycles = split ? 3U : 4U},
            "stack refusal dropped retired DAG stalls or committed part of the failing CALL");
    }
  }
}

void unretired_branch_stalls() {
  constexpr unsigned i8_code = 0x18;
  constexpr std::uint64_t call_eq = 0x088000000000;

  struct Refused {
    std::uint64_t prefix{}, branch{};
    bool returning{}, load{};
  };

  const std::array cases{
      Refused{.prefix = fixture::add(0, 0, 0), .branch = call_eq},
      Refused{.prefix = fixture::add(0, 0, 0), .branch = rts_eq, .returning = true},
      Refused{.prefix = fixture::immediate(i8_code, fixture::code_address + 2),
              .branch = call_eq,
              .load = true}};
  for (const auto &row : cases) {
    const std::array words{row.prefix, row.branch, std::uint64_t{0}};
    fixture::Program program(words);
    for (const auto mode : fixture::numeric_modes) {
      const auto native = fixture::compile(program.code, mode);
      sharc::Core core(program.memory(), mode);
      sharc::Registers expected;
      expected.pc = fixture::code_address;
      expected.pe[0].astat = unsigned{row.load};
      expected.pe[0].stky = row.returning ? stack_empty : stack_full;
      expected.pc_stack_pointer = row.returning ? 0 : sharc::Registers::pc_stack_capacity;
      fixture::install(core, native, expected);
      const auto saved = core.save();
      expected.pe[0].astat = 1;
      if (row.load) { expected.dag[0][8].index = fixture::code_address + 2; }
      fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 1});
      const auto result = core.run(3);
      check(result.has_fault && core.registers() == expected &&
                result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1},
            "refused CALL/RTS charged an unretired instruction's pending stalls");
      core.restore(saved);
      check(!core.run(1).has_fault, "refused branch prefix failed");
      const auto tail = core.run(1);
      check(tail.has_fault && tail.progress == sharc::Progress{} && core.registers() == expected,
            "refused CALL/RTS changed its exact owner-bound prefix");
    }
  }
}

} // namespace

int main() {
  return fixture::run_test([] {
    branch_boundaries();
    missing_target();
    observed_alu_status();
    call_snapshot();
    refused_stack_prefix();
    call_stack_status();
    unsupported_stack_operands();
    stack_interlock();
    stalled_stack_refusal();
    unretired_branch_stalls();
  });
}
