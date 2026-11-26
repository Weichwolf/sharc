// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "../support/prefix.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <sharc/core.hpp>
#include <vector>

namespace {
using fixture::check;
using fixture::Prefix;
using fixture::verify_prefixes;
constexpr auto base = fixture::code_address;
constexpr std::uint64_t db = 0x000004000000;
constexpr std::uint64_t jump_eq = 0x060000000000, jump_ne = 0x062000000000;
constexpr std::uint64_t jump_true = 0x063e00000000, call_true = 0x06be00000000;
constexpr std::uint64_t relative_call = 0x09be00000000, indirect_call = 0x08be00000000;
constexpr std::uint64_t rts = 0x0a3e00000000;
constexpr std::uint32_t pc_mask = 0xffffff, call_tag = 1U << 24, empty = 1U << 22;
constexpr std::uint32_t simd_bit = 1U << 21, az = 1;
constexpr unsigned raw_shift = 8;

struct Transfer {
  std::uint32_t branch_pc, target;
};

void set_slot(sharc::Registers &registers, Transfer transfer, bool first) {
  const auto [branch_pc, target] = transfer;
  // In the first slot the sequential third word is suppressed in Decode.
  // In the second slot the target is in Decode. Fetch1 has already redirected.
  ++registers.emuclk;
  registers.pc = (branch_pc + (first ? 1U : 2U)) & pc_mask;
  registers.faddr = (target + (first ? 1U : 2U)) & pc_mask;
  registers.daddr = first ? (branch_pc + 3) & pc_mask : target;
}

constexpr unsigned first_marker = 11;
constexpr unsigned second_marker = 22;
constexpr unsigned sequential_marker = 33;
constexpr unsigned taken_marker = 44;

std::array<Prefix, 4> jump_prefixes(sharc::Registers registers, bool taken, std::uint32_t target) {
  std::array<Prefix, 4> prefixes{};
  const Transfer transfer{.branch_pc = base, .target = target};
  const auto boundary = [&](bool first) {
    if (taken) {
      set_slot(registers, transfer, first);
    } else {
      fixture::boundary(registers, {.pc = base + (first ? 1U : 2U), .clock = registers.emuclk + 1});
    }
    return Prefix{.registers = registers, .cycles = registers.emuclk};
  };
  prefixes[0] = boundary(true);
  for (auto &lane : registers.pe) { lane.data[0][2] = first_marker << raw_shift; }
  prefixes[1] = boundary(false);
  for (auto &lane : registers.pe) { lane.data[0][3] = second_marker << raw_shift; }
  const auto next = taken ? target : base + 3;
  fixture::boundary(registers, {.pc = next, .clock = taken ? 4U : 3U});
  prefixes[2] = {.registers = registers, .cycles = registers.emuclk};
  for (auto &lane : registers.pe) {
    lane.data[0][taken ? 5 : 4] = (taken ? taken_marker : sequential_marker) << raw_shift;
  }
  fixture::boundary(registers, {.pc = next + 1, .clock = registers.emuclk + 1});
  prefixes[3] = {.registers = registers, .cycles = registers.emuclk};
  return prefixes;
}

void predicated_jump() {
  constexpr auto target = base + 5;
  const std::array words{jump_eq | db | target,
                         fixture::immediate(2, first_marker),
                         fixture::immediate(3, second_marker),
                         fixture::immediate(4, sequential_marker),
                         std::uint64_t{0},
                         fixture::immediate(5, taken_marker)};
  fixture::Program program(words);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    for (const bool taken : {false, true}) {
      sharc::Registers registers;
      registers.pc = base;
      registers.mode1 = simd_bit;
      registers.pe[0].astat = az;
      registers.pe[1].astat = taken ? az : 0;
      sharc::Core core(program.memory(), mode);
      sharc::Core replay(program.memory(), mode);
      fixture::install(core, native, registers);
      (void)replay.install(native, fixture::Program::variant_id);
      verify_prefixes(core, replay, jump_prefixes(registers, taken, target));
    }
  }
}

void call_and_return(std::uint64_t operation) {
  constexpr std::uint32_t target = base + 10;
  constexpr unsigned i8 = 0x18;
  const std::array caller{operation | db,
                          fixture::immediate(i8, base + 4),
                          fixture::immediate(2, 9),
                          fixture::immediate(3, 11),
                          std::uint64_t{0}};
  const std::array callee{rts | db, fixture::immediate(4, 13), fixture::immediate(5, 15)};
  fixture::Program program(caller);
  program.add_span(target, callee);
  program.code.variants[0].entries.push_back(target);
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code, mode);
    sharc::Core core(program.memory(), mode);
    sharc::Core replay(program.memory(), mode);
    sharc::Registers registers;
    registers.pc = base;
    registers.dag[0][8].index = target;
    registers.pe[0].stky = empty;
    fixture::install(core, native, registers);
    (void)replay.install(native, fixture::Program::variant_id);
    std::vector<Prefix> prefixes;
    const auto record = [&] {
      prefixes.push_back({.registers = registers, .cycles = registers.emuclk});
    };
    registers.pc_stack_pointer = 1;
    registers.pc_stack[0] = (base + 3) | call_tag;
    registers.pe[0].stky = 0;
    set_slot(registers, {.branch_pc = base, .target = target}, true);
    record();
    registers.dag[0][8].index = base + 4;
    set_slot(registers, {.branch_pc = base, .target = target}, false);
    record();
    registers.pe[0].data[0][2] = 9U << raw_shift;
    fixture::boundary(registers, {.pc = target, .clock = 4});
    record();
    registers.pc_stack_pointer = 0;
    registers.pe[0].stky = empty;
    set_slot(registers, {.branch_pc = target, .target = base + 3}, true);
    record();
    registers.pe[0].data[0][4] = 13U << raw_shift;
    set_slot(registers, {.branch_pc = target, .target = base + 3}, false);
    record();
    registers.pe[0].data[0][5] = 15U << raw_shift;
    fixture::boundary(registers, {.pc = base + 3, .clock = 8});
    record();
    registers.pe[0].data[0][3] = 11U << raw_shift;
    fixture::boundary(registers, {.pc = base + 4, .clock = 9});
    record();
    verify_prefixes(core, replay, prefixes);

    sharc::Registers full_stack;
    full_stack.pc = base;
    full_stack.dag[0][8].index = target;
    full_stack.pc_stack_pointer = sharc::Registers::pc_stack_capacity;
    constexpr std::uint32_t pc_stack_full = 1U << 21;
    full_stack.pe[0].stky = pc_stack_full;
    fixture::install(core, native, full_stack);
    const auto overflow = core.run(4);
    check(overflow.has_fault && overflow.fault.reason == sharc::FaultReason::unsupported_feature &&
              overflow.fault.address == base && overflow.progress == sharc::Progress{} &&
              core.registers() == full_stack,
          "full delayed CALL retired a slot or changed the refused stack");
  }
}

void adjacent_jumps() {
  constexpr auto eq_target = base + 4;
  constexpr auto ne_target = base + 5;
  constexpr unsigned eq_marker = 23;
  constexpr unsigned ne_marker = 47;
  const std::array words{jump_eq | db | eq_target,
                         jump_ne | db | ne_target,
                         fixture::immediate(1, eq_marker),
                         fixture::immediate(2, ne_marker),
                         std::uint64_t{0},
                         std::uint64_t{0}};
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  for (const bool zero : {false, true}) {
    sharc::Core core(program.memory());
    sharc::Registers registers;
    registers.pc = base;
    registers.pe[0].astat = zero ? az : 0;
    fixture::install(core, native, registers);
    const auto result = core.run(zero ? 3 : 4);
    registers.pe[0].data[0][1] = eq_marker << raw_shift;
    if (!zero) { registers.pe[0].data[0][2] = ne_marker << raw_shift; }
    fixture::boundary(registers, {.pc = zero ? eq_target : ne_target, .clock = zero ? 4U : 5U});
    check(fixture::completed(result, zero ? 3U : 4U, registers.emuclk) &&
              core.registers() == registers,
          "adjacent complementary delayed JUMPs retired the wrong slots");
  }
}

void bad_slots() {
  constexpr auto target = base + 3;
  const std::array missing{jump_true | db | target, std::uint64_t{0}};
  const fixture::Program undeclared(missing);
  const auto hole = fixture::refusal(undeclared.code, "missing delay slot was prepared");
  check(hole.reason == sharc::FaultReason::undeclared_entry && hole.address == base &&
            hole.instruction == missing.front() && !hole.identity.empty(),
        "missing slot refusal lost the branch PC, instruction or identity");
  for (const bool first : {false, true}) {
    const auto branch = jump_true | target;
    const std::array words{jump_true | db | target,
                           first ? branch : std::uint64_t{0},
                           first ? std::uint64_t{0} : branch,
                           std::uint64_t{0}};
    const fixture::Program program(words);
    const auto fault = fixture::refusal(program.code, "branch inside a delay slot was prepared");
    check(fault.reason == sharc::FaultReason::invalid_encoding &&
              fault.address == base + (first ? 1U : 2U) && fault.instruction == branch,
          "illegal slot instruction lost its fault provenance");
  }
}

void wrapped_slots() {
  constexpr auto target = 8U;
  const std::array branch{jump_true | db | target};
  const std::array slots{fixture::immediate(1, 1), fixture::immediate(2, 2)};
  const std::array landing{fixture::immediate(3, 3)};
  fixture::Program program(branch, pc_mask);
  program.add_span(0, slots);
  program.add_span(target, landing);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  sharc::Core replay(program.memory());
  sharc::Registers registers;
  registers.pc = pc_mask;
  fixture::install(core, native, registers);
  (void)replay.install(native, fixture::Program::variant_id);
  set_slot(registers, {.branch_pc = pc_mask, .target = target}, true);
  const Prefix first{.registers = registers, .cycles = 1};
  registers.pe[0].data[0][1] = 1U << raw_shift;
  set_slot(registers, {.branch_pc = pc_mask, .target = target}, false);
  const Prefix second{.registers = registers, .cycles = 2};
  registers.pe[0].data[0][2] = 2U << raw_shift;
  fixture::boundary(registers, {.pc = target, .clock = 4});
  verify_prefixes(
      core, replay, std::array{first, second, Prefix{.registers = registers, .cycles = 4}});
}

void slot_dag_interlock() {
  constexpr auto target = base + 4;
  constexpr unsigned i0 = 0x10;
  constexpr std::uint64_t load_r0 = 0xa00000000000;
  constexpr std::uint32_t data_address = 0x90000;
  constexpr std::uint32_t storage_id = 11;
  const std::array words{jump_true | db | target,
                         std::uint64_t{0},
                         fixture::immediate(i0, data_address),
                         std::uint64_t{0},
                         load_r0};
  fixture::Program program(words);
  program.code.variants.front().layout.regions.push_back(
      fixture::normal_region(data_address, 1, storage_id));
  std::array ram{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const auto memory = program.memory(storage_id, ram);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(memory);
  sharc::Core replay(memory);
  sharc::Registers registers;
  registers.pc = base;
  fixture::install(core, native, registers);
  (void)replay.install(native, fixture::Program::variant_id);
  const Transfer transfer{.branch_pc = base, .target = target};
  set_slot(registers, transfer, true);
  const Prefix first{.registers = registers, .cycles = 1};
  set_slot(registers, transfer, false);
  const Prefix second{.registers = registers, .cycles = 2};
  registers.dag[0][0].index = data_address;
  fixture::boundary(registers, {.pc = target, .clock = 4});
  const Prefix refill{.registers = registers, .cycles = 4};
  constexpr std::uint64_t loaded_raw = 0x0403020100;
  registers.pe[0].data[0][0] = loaded_raw;
  fixture::boundary(registers, {.pc = target + 1, .clock = 6});
  const Prefix load{.registers = registers, .cycles = 6};
  verify_prefixes(core, replay, std::array{first, second, refill, load});
}

void slot_fault() {
  constexpr auto target = base + 4;
  constexpr std::uint64_t load_r0 = 0xa00000000000;
  constexpr std::uint32_t data_address = 0x90000;
  const std::array words{jump_true | db | target,
                         load_r0,
                         fixture::immediate(2, 1),
                         std::uint64_t{0},
                         std::uint64_t{0}};
  fixture::Program program(words);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  sharc::Registers registers;
  registers.pc = base;
  registers.dag[0][0].index = data_address;
  fixture::install(core, native, registers);
  const auto result = core.run(4);
  set_slot(registers, {.branch_pc = base, .target = target}, true);
  check(result.has_fault && result.fault.address == base + 1 &&
            result.fault.instruction == load_r0 && !result.fault.identity.empty() &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            core.registers() == registers,
        "fault in the first delay slot changed the exact prefix or published the target");
}
} // namespace

void fixture::delayed_control_flow() {
  predicated_jump();
  call_and_return(call_true | (base + 10));
  constexpr unsigned displacement_shift = 27;
  call_and_return(relative_call | (std::uint64_t{10} << displacement_shift));
  call_and_return(indirect_call);
  adjacent_jumps();
  bad_slots();
  wrapped_slots();
  slot_fault();
  slot_dag_interlock();
}
