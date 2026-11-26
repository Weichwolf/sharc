// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <array>
#include <cstdint>
#include <sharc/core.hpp>
#include <stdexcept>

namespace {
using fixture::check;
constexpr auto base = fixture::code_address;
constexpr std::uint32_t irq2_mask = 1U << 8;
constexpr std::uint32_t p6_latch = 1U, p6_enable = 1U << 10, p6_active = 1U << 20;
constexpr std::uint32_t interrupt_enable = 1U << 12, nesting_enable = 1U << 11;
constexpr std::uint32_t simd = 1U << 21;
constexpr std::uint32_t irq_tag = 1U << 25, status_empty = 1U << 24, pc_empty = 1U << 22;
constexpr std::uint64_t rti = 0x0b3e00000000, rti_db = 0x0b3e04000000;
constexpr std::uint64_t idle = 0x008000000000, jump_db = 0x063e04000000;
constexpr sharc::InterruptRequest irq2{.source = 8, .vector = base + 0x10};
constexpr sharc::InterruptRequest p6{.source = 17, .vector = base + 0x20};
constexpr std::uint64_t after_outer_entry = 8;
constexpr std::uint64_t after_inner_entry = 13;
constexpr std::uint64_t after_inner_return = 18;
constexpr std::uint64_t after_outer_return = 22;
constexpr std::uint64_t asleep = 24;

void nested_service(sharc::Core &core,
                    const sharc::NativeImage &native,
                    fixture::Program &program,
                    sharc::ExecutionMode mode) {
  check(fixture::completed(core.run_until(after_outer_entry + 1), 1, 1) &&
            core.interrupt_ready(irq2) && !core.interrupt_ready(p6),
        "higher priority IRQ did not become eligible after the first vector instruction");
  auto registers = core.registers();
  const auto interrupted_mode = registers.mode1;
  registers.mode1 &= ~nesting_enable;
  core.set_registers(registers);
  check(!core.interrupt_ready(irq2), "disabled nesting allowed a higher priority IRQ");
  registers.mode1 = interrupted_mode;
  core.set_registers(registers);
  const auto before_irq2 = core.save();
  core.interrupt(irq2);
  const auto entry = core.state();
  const auto &entered = entry.registers;
  check(entry.progress == sharc::Progress{.instructions = 4, .base_cycles = after_inner_entry} &&
            entered.emuclk == after_inner_entry && entered.pc == irq2.vector &&
            entered.pc_stack_pointer == 2 && entered.pc_stack[1] == (irq_tag | (p6.vector + 1)) &&
            entered.status_depth == 1 &&
            entered.status_stack[0] == sharc::StatusFrame{.mode1 = registers.mode1,
                                                          .astat_x = registers.pe[0].astat,
                                                          .astat_y = registers.pe[1].astat} &&
            entered.mode1 == (registers.mode1 & ~simd) && entered.imaskp == irq2_mask &&
            entered.lirptl == (p6_enable | p6_active) && (entered.irptl & irq2_mask) == 0,
        "nested IRQ2 did not capture its interrupt and status stacks");
  core.restore(before_irq2);
  core.interrupt(irq2);
  check(core.state() == entry, "interrupt entry differed across replay");
  const auto accepted = core.save();
  check(fixture::completed(core.run_until(after_inner_entry + 1), 1, 1), "IRQ2 body failed");
  auto invalid = core.registers();
  const auto before_rti = core.save();
  invalid.status_depth = 0;
  core.set_registers(invalid);
  const auto failure = core.run(1);
  check(failure.has_fault && failure.fault.instruction == rti &&
            failure.progress == sharc::Progress{} && core.registers() == invalid,
        "RTI without its status frame consumed the return address or progress");
  core.restore(before_rti);
  check(fixture::completed(core.run_until(after_inner_return - 3), 1, 4), "IRQ2 RTI failed");
  auto returned = core.registers();
  check(returned.pc == p6.vector + 1 && returned.pc_stack_pointer == 1 &&
            returned.status_depth == 0 && (returned.pe[0].stky & status_empty) != 0 &&
            returned.mode1 == registers.mode1 && returned.pe[0].astat == registers.pe[0].astat &&
            returned.pe[1].astat == registers.pe[1].astat && returned.imaskp == 0 &&
            returned.lirptl == (p6_enable | p6_active) && returned.pe[0].data[0][6] == 7U << 8 &&
            returned.pe[1].data[0][6] == 0,
        "IRQ2 RTI failed to restore the outer service boundary or its numerical mode");
  sharc::Core replay(program.memory(), mode);
  (void)replay.install(native, fixture::Program::variant_id);
  replay.restore(accepted);
  check(fixture::completed(replay.run_until(after_inner_return), 2, 5) &&
            replay.state() == core.state(),
        "cross-Core replay lost the nested return continuation");
}

void irq_deadlines(sharc::ExecutionMode mode) {
  constexpr std::uint32_t pex_astat = 0x20000001U;
  constexpr std::uint32_t pey_astat = 0x40000001U;
  const std::array main{jump_db | (base + 4),
                        fixture::immediate(1, 3),
                        fixture::immediate(2, 4),
                        fixture::immediate(0, 99),
                        fixture::add(3, 1, 2),
                        idle,
                        fixture::immediate(4, 11)};
  const std::array high{fixture::add(6, 1, 2), rti};
  const std::array low{
      std::uint64_t{0}, rti_db, fixture::immediate(5, 5), fixture::immediate(7, 7)};
  fixture::Program program(main);
  program.add_span(irq2.vector, high);
  program.add_span(p6.vector, low);
  program.code.variants.front().interrupt_vectors = {irq2, p6};
  const auto native = fixture::compile(program.code, mode);
  sharc::Core core(program.memory(), mode);
  (void)core.install(native, fixture::Program::variant_id);
  core.reset(base);
  auto registers = core.registers();
  registers.imask |= irq2_mask;
  registers.irptl = irq2_mask;
  registers.lirptl = p6_enable | p6_latch;
  registers.pe[0].astat = pex_astat;
  registers.pe[1].astat = pey_astat;
  core.set_registers(registers);
  check(!core.interrupt_ready(irq2), "IRQ ignored its global mask");
  registers.mode1 = interrupt_enable | nesting_enable | simd;
  core.set_registers(registers);
  const sharc::InterruptRequest wrong{.source = irq2.source, .vector = p6.vector};
  check(!core.interrupt_ready(wrong), "IRQ with the wrong declared vector was eligible");
  fixture::throws<sharc::PreparationError>([&] { core.interrupt(wrong); },
                                           "undeclared IRQ route was accepted");
  for (const auto deadline : {1U, 2U}) {
    check(fixture::completed(core.run_until(deadline), 1, 1) && !core.interrupt_ready(p6),
          "IRQ became eligible inside a taken delayed jump");
    fixture::throws<std::invalid_argument>([&] { core.interrupt(p6); },
                                           "delayed jump accepted an ineligible IRQ");
  }
  check(fixture::completed(core.run_until(3), 1, 2) && core.interrupt_ready(p6),
        "second delay slot did not complete before IRQ entry");
  core.interrupt(p6);
  check(core.registers().pc == p6.vector && core.registers().status_depth == 0 &&
            core.registers().pc_stack[0] == (irq_tag | (base + 4)) && !core.interrupt_ready(irq2) &&
            fixture::completed(core.run_until(after_outer_entry), 0, 0),
        "P6 vector entry charged the wrong time or allowed immediate nesting");
  nested_service(core, native, program, mode);

  check(fixture::completed(core.run_until(after_inner_return + 1), 1, 1) &&
            core.registers().pc_stack_pointer == 0 && core.registers().lirptl == p6_enable &&
            !core.interrupt_ready(irq2),
        "RTI (DB) did not clear the active source before the protected delay slots");
  registers = core.registers();
  registers.lirptl |= p6_latch;
  core.set_registers(registers);
  const auto delayed_return = core.save();
  check(fixture::completed(core.run_until(after_outer_return), 2, 3) &&
            core.registers().pc == base + 4 && core.registers().lirptl == (p6_enable | p6_latch),
        "RTI (DB) did not reach the interrupted continuation after its slots");
  const auto resumed = core.state();
  core.restore(delayed_return);
  check(fixture::completed(core.run_until(after_outer_return), 2, 3) && core.state() == resumed,
        "snapshot lost a pending delayed interrupt return");
  const auto sleeping = core.run_until(asleep + 1);
  registers = core.registers();
  check(fixture::completed(sleeping, 2, 2) &&
            sleeping.execution == sharc::ExecutionState::sleeping && registers.pc == base + 6 &&
            registers.pe[0].data[0][0] == 0 && registers.pe[0].data[0][3] == 7U << 8 &&
            registers.pe[0].data[0][5] == 5U << 8 && registers.pe[0].data[0][7] == 7U << 8 &&
            core.interrupt_ready(irq2),
        "ISR slots or resumed main program failed to reach a wakeable IDLE");
  core.interrupt(irq2);
  check(fixture::completed(core.run_until(asleep + 9), 2, 5) && core.registers().pc == base + 6 &&
            fixture::completed(core.run(1), 1, 1) &&
            core.registers().pe[0].data[0][4] == 11U << 8 &&
            (core.registers().pe[0].stky & pc_empty) != 0,
        "interrupt wake did not resume the main word following IDLE");
}

void irq_in_counted_loop(sharc::ExecutionMode mode) {
  constexpr unsigned length = 8;
  constexpr unsigned turns = 2;
  constexpr std::uint64_t counted_twice = 0x0c0002000000;
  constexpr std::uint64_t before_irq = 7;
  constexpr std::uint64_t event_cost = 9;
  constexpr std::uint64_t main_cycles = 1 + (length * turns) + 1;
  std::array<std::uint64_t, length + 2> main{};
  main[0] = counted_twice | length;
  main[length] = fixture::add(0, 0, 1);
  main.back() = idle;
  const std::array handler{fixture::immediate(2, 3), rti};
  fixture::Program program(main);
  program.add_span(irq2.vector, handler);
  program.code.variants.front().interrupt_vectors = {irq2};
  const auto native = fixture::compile(program.code, mode);
  sharc::Core core(program.memory(), mode);
  (void)core.install(native, fixture::Program::variant_id);
  core.reset(base);
  auto registers = core.registers();
  registers.mode1 = interrupt_enable;
  registers.imask |= irq2_mask;
  registers.pe[0].data[0][1] = 1U << 8;
  core.set_registers(registers);
  check(fixture::completed(core.run_until(before_irq), before_irq, before_irq) &&
            core.registers().pc == base + before_irq && core.registers().faddr == base + 3 &&
            core.registers().loop_counter_stack[0] == 1 && core.interrupt_ready(irq2),
        "IRQ fixture did not reach the already-sampled loop suffix");
  const auto interrupted = core.save();
  const auto before_entry = core.registers();
  core.interrupt(irq2);
  check(core.registers().pc_stack_pointer == 2 && core.registers().loop_counter_depth == 1 &&
            fixture::completed(core.run_until(before_irq + event_cost), 2, 5),
        "IRQ in a loop lost the underlying frames or ISR accounting");
  const auto returned = core.registers();
  check(returned.pc == before_entry.pc && returned.faddr == before_entry.faddr &&
            returned.daddr == before_entry.daddr && returned.pc_stack_pointer == 1 &&
            returned.loop_counter_stack == before_entry.loop_counter_stack &&
            returned.loop_address_stack == before_entry.loop_address_stack,
        "RTI changed the loop sample, PC, or prefetch continuation");
  const auto result = core.run_until(main_cycles + event_cost);
  const auto with_irq = core.registers();
  check(fixture::completed(result, main_cycles - before_irq, main_cycles - before_irq) &&
            result.execution == sharc::ExecutionState::sleeping &&
            with_irq.pe[0].data[0][0] == turns << 8 && with_irq.pe[0].data[0][2] == 3U << 8 &&
            with_irq.loop_counter_depth == 0 && with_irq.pc_stack_pointer == 0,
        "interrupted loop did not finish both iterations");
  core.restore(interrupted);
  check(fixture::completed(
            core.run_until(main_cycles), main_cycles - before_irq, main_cycles - before_irq) &&
            core.registers().pe[0].data[0][0] == with_irq.pe[0].data[0][0] &&
            core.registers().loop_counter_stack == with_irq.loop_counter_stack,
        "interrupt changed the loop outcome relative to plain continuation");
}

void irq_stack_limits() {
  const std::array main{std::uint64_t{0}};
  const std::array handler{rti};
  fixture::Program program(main);
  program.add_span(irq2.vector, handler);
  program.code.variants.front().interrupt_vectors = {irq2};
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  (void)core.install(native, fixture::Program::variant_id);
  core.reset(base);
  auto registers = core.registers();
  registers.mode1 = interrupt_enable;
  registers.imask |= irq2_mask;
  const auto full_status = static_cast<std::uint8_t>(registers.status_stack.size());
  const auto full_pc = static_cast<std::uint8_t>(sharc::Registers::pc_stack_capacity);
  for (const auto &full : {sharc::Registers{.pc_stack_pointer = full_pc},
                           sharc::Registers{.status_depth = full_status}}) {
    registers.pc_stack_pointer = full.pc_stack_pointer;
    registers.status_depth = full.status_depth;
    core.set_registers(registers);
    const auto before = core.state();
    check(!core.interrupt_ready(irq2), "IRQ accepted a full architectural stack");
    fixture::throws<std::invalid_argument>([&] { core.interrupt(irq2); },
                                           "IRQ wrote a full architectural stack");
    check(core.state() == before, "refused vector partially changed core state");
  }
  constexpr std::uint32_t stack_overflow_latch = 1U << 3;
  registers.pc_stack_pointer = full_pc - 2;
  registers.status_depth = full_status - 1;
  core.set_registers(registers);
  core.interrupt(irq2);
  auto entered = core.registers();
  check(entered.status_depth == full_status && entered.pc_stack_pointer == full_pc - 1 &&
            (entered.irptl & stack_overflow_latch) != 0,
        "valid vector lost the nearly-full PC stack latch");
  const auto saved = core.save();
  entered.pc_stack[entered.pc_stack_pointer - 1] &= ~irq_tag;
  core.set_registers(entered);
  const auto invalid = core.run(1);
  check(invalid.has_fault && invalid.fault.instruction == rti &&
            invalid.progress == sharc::Progress{} && core.registers() == entered,
        "untagged RTI popped a frame or changed progress");
  core.restore(saved);
  check(fixture::completed(core.run(1), 1, 4) && core.registers().pc == base &&
            core.registers().pc_stack_pointer == registers.pc_stack_pointer &&
            core.registers().status_depth == registers.status_depth,
        "RTI near the stack limits failed to restore the prior depths");
}
} // namespace

int main() {
  return fixture::run_test([] {
    for (const auto mode : fixture::numeric_modes) {
      irq_deadlines(mode);
      irq_in_counted_loop(mode);
    }
    irq_stack_limits();
  });
}
