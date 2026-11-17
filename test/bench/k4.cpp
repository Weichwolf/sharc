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
#include <span>
#include <vector>

namespace {
using fixture::check;
constexpr unsigned transitions = 1U << 20, handler_count = 8, handler_words = 6;
constexpr unsigned word_bytes = 4, byte_bits = 8, raw_shift = 8;
constexpr std::uint32_t table_address = 0x90000, ram_id = 11;
constexpr std::uint32_t handlers_address = 0x200, common_address = 0x300;
constexpr std::uint32_t initial_accumulator = 0x2468ace0, final_accumulator = 0x24d0ace0;
constexpr std::uint32_t call_tag = 1U << 24, stack_empty = 1U << 22;
constexpr std::uint32_t rotate_right = 0xffffffff;
constexpr std::uint64_t guests = (std::uint64_t{transitions} * 18) + 1;
constexpr std::uint64_t cycles = (std::uint64_t{transitions} * 38) - 2;
constexpr unsigned prepared_instructions = 61;
constexpr std::byte guard{0xa5};
constexpr bench::Control initial{.state = 3, .accumulator = initial_accumulator};
constexpr bench::Control answer{.state = 3, .accumulator = final_accumulator, .last_handler = 0};
using Table = std::array<std::uint32_t, handler_count>;

std::uint64_t fixed(unsigned opcode, unsigned destination, unsigned source, unsigned right) {
  constexpr unsigned opcode_shift = 12;
  constexpr unsigned destination_shift = 8;
  constexpr unsigned source_shift = 4;
  constexpr std::uint64_t header = 0x013e00000000;
  return header | (std::uint64_t{opcode} << opcode_shift) |
         (std::uint64_t{destination} << destination_shift) |
         (std::uint64_t{source} << source_shift) | right;
}

std::uint64_t move(unsigned destination) {
  // R2 is the explicit source; split source fields are numerical fixture fields.
  constexpr std::uint64_t source_r2 = 0x703f00000000;
  constexpr unsigned destination_shift = 23;
  return source_r2 | (std::uint64_t{destination} << destination_shift);
}

void append_segment(fixture::Program &program,
                    std::uint32_t address,
                    std::span<const std::uint64_t> words) {
  auto &variant = program.code.variants[0];
  const auto offset = program.bytes.size();
  auto &segment =
      variant.spans.emplace_back(address, sharc::InstructionSet::isa, std::vector<std::uint16_t>{});
  for (const auto word : words) {
    fixture::append(segment, word);
    for (unsigned byte = 0; byte < fixture::Program::word_bytes; ++byte) {
      program.bytes.push_back(static_cast<std::byte>(word >> (byte * byte_bits)));
    }
  }
  variant.layout.regions.push_back({.space = sharc::MemorySpace::program,
                                    .view = sharc::MemoryView::instruction,
                                    .first = address,
                                    .count = words.size(),
                                    .storage_id = fixture::Program::storage_id,
                                    .byte_offset = offset,
                                    .byte_stride = fixture::Program::word_bytes,
                                    .readable = true,
                                    .writable = false,
                                    .initialized = true});
}

fixture::Program kernel() {
  constexpr unsigned i0_code = 0x10;
  constexpr unsigned i8_code = 0x18;
  constexpr std::uint64_t load_r2 = 0xa00200000000;
  constexpr std::uint64_t indirect_call = 0x08be00000000;
  constexpr std::uint64_t decrement = 0x013e0002a440;
  constexpr std::uint64_t backward = 0x072000fffff9;
  constexpr std::uint64_t direct_call = 0x06be00000000;
  constexpr std::uint64_t rts = 0x0a3e00000000;
  constexpr std::uint64_t rotate_r2_r0_r6 = 0x013e00208206;
  constexpr unsigned bit_and = 0x40;
  constexpr unsigned bit_xor = 0x42;
  const std::array main{fixed(bit_and, 0, 0, 7),
                        fixture::add(2, 0, 5),
                        move(i0_code),
                        load_r2,
                        move(i8_code),
                        indirect_call,
                        decrement,
                        backward,
                        std::uint64_t{0}};
  fixture::Program program(main);
  std::vector<std::uint64_t> handlers;
  for (unsigned handler = 0; handler < handler_count; ++handler) {
    program.code.variants[0].entries.push_back(handlers_address + (handler * handler_words));
    const std::array body{fixture::add(1, 1, 0),
                          fixture::immediate(2, handler + 1),
                          fixed(bit_xor, 1, 1, 2),
                          fixture::immediate(0, (handler + 3) & 7),
                          direct_call | common_address,
                          rts};
    handlers.insert(handlers.end(), body.begin(), body.end());
  }
  append_segment(program, handlers_address, handlers);
  const std::array common{rotate_r2_r0_r6, fixed(bit_xor, 1, 1, 2), fixture::add(1, 1, 3), rts};
  append_segment(program, common_address, common);
  return program;
}

Table table() {
  Table result;
  for (unsigned handler = 0; handler < result.size(); ++handler) {
    result[handler] = handlers_address + (handler * handler_words);
  }
  return result;
}

class Storage : public bench::WordSpace {
public:
  void initialize(const Table &input) const {
    std::ranges::fill(at(table_address - 1, sizeof(input) + (std::size_t{2} * word_bytes)), guard);
    auto destination = at(table_address, sizeof(input));
    for (std::size_t word = 0; word < input.size(); ++word) {
      for (unsigned byte = 0; byte < word_bytes; ++byte) {
        destination[(word * word_bytes) + byte] =
            static_cast<std::byte>(input[word] >> (byte * byte_bits));
      }
    }
  }

  void validate(const Table &input) const {
    const auto source = at(table_address, sizeof(input));
    for (std::size_t word = 0; word < input.size(); ++word) {
      for (unsigned byte = 0; byte < word_bytes; ++byte) {
        check(source[(word * word_bytes) + byte] ==
                  static_cast<std::byte>(input[word] >> (byte * byte_bits)),
              "K4 modified a table word");
      }
    }
    for (const auto address : {table_address - 1, table_address + handler_count}) {
      check(std::ranges::all_of(at(address, word_bytes),
                                [](std::byte byte) { return byte == guard; }),
            "K4 changed a table guard");
    }
  }
};

sharc::Registers registers() {
  sharc::Registers state;
  state.pc = fixture::code_address;
  state.pe[0].data[0][0] = std::uint64_t{initial.state} << raw_shift;
  state.pe[0].data[0][1] = std::uint64_t{initial.accumulator} << raw_shift;
  state.pe[0].data[0][3] = std::uint64_t{3} << raw_shift;
  state.pe[0].data[0][4] = std::uint64_t{transitions} << raw_shift;
  state.pe[0].data[0][5] = std::uint64_t{table_address} << raw_shift;
  state.pe[0].data[0][6] = std::uint64_t{rotate_right} << raw_shift;
  state.pe[0].data[0][7] = std::uint64_t{7} << raw_shift;
  state.pe[0].stky = stack_empty;
  return state;
}

void validate(const sharc::Core &core, const sharc::RunResult &result) {
  constexpr std::uint32_t rotated_state = 0x80000001;
  auto expected = registers();
  expected.pe[0].data[0][0] = std::uint64_t{answer.state} << raw_shift;
  expected.pe[0].data[0][1] = std::uint64_t{answer.accumulator} << raw_shift;
  expected.pe[0].data[0][2] = std::uint64_t{rotated_state} << raw_shift;
  expected.pe[0].data[0][4] = 0;
  expected.pe[0].astat = 9;
  expected.dag[0][0].index = table_address + answer.last_handler;
  expected.dag[0][8].index = handlers_address + (answer.last_handler * handler_words);
  expected.pc_stack[0] = (fixture::code_address + 6) | call_tag;
  expected.pc_stack[1] = (handlers_address + (answer.last_handler * handler_words) + 5) | call_tag;
  expected.pc = fixture::code_address + 9;
  expected.faddr = expected.pc + 4;
  expected.daddr = expected.pc + 2;
  expected.emuclk = cycles;
  check(!result.has_fault && result.execution == sharc::ExecutionState::stopped &&
            core.registers() == expected &&
            result.progress == sharc::Progress{.instructions = guests, .base_cycles = cycles} &&
            core.state().progress == result.progress,
        "K4 complete registers, output, stack, pointers, flags or exact progress differ");
}

std::uint64_t reference_run() {
  const auto begin = bench::Clock::now();
  const auto result = bench::control(transitions, initial);
  const auto end = bench::Clock::now();
  check(result == answer, "K4 independent native reference differs from the fixed output");
  return bench::nanoseconds(begin, end);
}

void run(sharc::ExecutionMode mode, bool measure) {
  const auto input = table();
  const Storage storage;
  storage.initialize(input);
  storage.protect_read_only();
  auto program = kernel();
  program.code.variants[0].layout.regions.push_back({.space = sharc::MemorySpace::data,
                                                     .view = sharc::MemoryView::normal_word,
                                                     .first = 0,
                                                     .count = std::uint64_t{1} << 32,
                                                     .storage_id = ram_id,
                                                     .byte_offset = 0,
                                                     .byte_stride = word_bytes,
                                                     .readable = true,
                                                     .writable = false,
                                                     .initialized = true});
  auto memory = program.memory();
  memory.regions.push_back({.storage_id = ram_id, .bytes = storage.all(), .generation = 1});
  const auto native = sharc::Core::compile(program.code, {.mode = mode, .callbacks = {}});
  sharc::Core core(memory, mode);
  core.set_registers(registers());
  (void)core.install(native, fixture::Program::variant_id);
  const auto checkpoint = core.save();
  bench::sample(
      {.name = "K4",
       .mode = mode,
       .progress = {.instructions = guests, .base_cycles = cycles},
       .prepared_instructions = prepared_instructions},
      native,
      core,
      measure,
      [&] { core.restore(checkpoint); },
      [&](const sharc::RunResult &result) {
        validate(core, result);
        storage.validate(input);
      },
      reference_run);
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
