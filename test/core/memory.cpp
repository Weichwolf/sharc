// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <span>
#include <string>
#if defined(__linux__)
#include "../support/word_space.hpp"

#include <sys/mman.h>
#endif

namespace {
using fixture::check;
constexpr std::uint32_t ram_address = 0x90000, ram_id = 11;
constexpr std::uint64_t load_r0 = 0xa00000000000, store_r0 = 0xa10000000000;
constexpr unsigned instruction_bits = 48;
constexpr std::array<std::byte, 8> initial_ram{std::byte{0x11},
                                               std::byte{0x22},
                                               std::byte{0x33},
                                               std::byte{0x44},
                                               std::byte{0x55},
                                               std::byte{0x66},
                                               std::byte{0x77},
                                               std::byte{0x88}};

struct Program {
  fixture::Program code;
  std::array<std::byte, 8> ram = initial_ram;

  explicit Program(std::span<const std::uint64_t> words) : code(words) {
    code.code.variants[0].layout.regions.push_back(fixture::normal_region(ram_address, 2, ram_id));
  }

  sharc::Memory memory() { return code.memory(ram_id, ram); }
};

sharc::Registers initial() {
  sharc::Registers result;
  result.pc = fixture::code_address;
  result.dag[0][0].index = ram_address;
  return result;
}

void fault_prefixes() {
  const std::array words{fixture::immediate(2, 5), fixture::add(3, 2, 2), load_r0};
  for (const bool denied : {false, true}) {
    Program program(words);
    auto &layout = program.code.code.variants[0].layout.regions.back();
    layout.readable = !denied;
    layout.initialized = denied;
    const auto native = sharc::Core::compile(program.code.code);
    sharc::Core core(program.memory());
    fixture::install(core, native, initial());
    const auto saved = core.save();
    auto expected = initial();
    expected.pe[0].data[0][2] = 5U << 8;
    expected.pe[0].data[0][3] = 10U << 8;
    fixture::boundary(expected, {.pc = expected.pc + 2, .clock = 2});
    const auto result = core.run(3);
    check(result.has_fault &&
              result.fault.reason == (denied ? sharc::FaultReason::access_fault
                                             : sharc::FaultReason::unsupported_feature) &&
              result.fault.identity == native.report().identity &&
              result.fault.address == expected.pc && result.fault.instruction == load_r0 &&
              result.fault.instruction_bits == instruction_bits &&
              result.fault.detail.contains(std::to_string(ram_address)) &&
              result.progress == sharc::Progress{.instructions = 2, .base_cycles = 2} &&
              core.registers() == expected,
          "memory failure lost identity, instruction, data address or completed native prefix");
    check(core.run(1).has_fault && core.registers() == expected, "memory fault was not latched");
    core.restore(saved);
    check(!core.run(2).has_fault && core.registers() == expected,
          "bound before failing memory changed the prefix");
    const auto partial = core.run(1);
    check(partial.has_fault && partial.progress == sharc::Progress{} &&
              core.registers() == expected,
          "partial memory failure retired an instruction or lost preceding state");
  }
}

void unexecuted_failure() {
  constexpr std::uint64_t jump_eq = 0x060000000000;
  const std::array words{
      jump_eq | (fixture::code_address + 2), load_r0, fixture::immediate(1, 7), std::uint64_t{0}};
  Program program(words);
  program.code.code.variants[0].layout.regions.back().readable = false;
  const auto native = sharc::Core::compile(program.code.code);
  sharc::Core core(program.memory());
  auto expected = initial();
  expected.pe[0].astat = 1;
  fixture::install(core, native, expected);
  const auto saved = core.save();
  const auto result = core.run(3);
  expected.pe[0].data[0][1] = 7U << 8;
  fixture::boundary(expected, {.pc = expected.pc + 4, .clock = 6});
  check(fixture::completed(result, 3, 6) && core.registers() == expected &&
            program.ram == initial_ram,
        "unit memory proof faulted an unexecuted branch");
  core.restore(saved);
  expected = initial();
  core.set_registers(expected);
  const auto fault = core.run(3);
  fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 1});
  check(fault.has_fault && fault.fault.reason == sharc::FaultReason::access_fault &&
            fault.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            core.registers() == expected && program.ram == initial_ram,
        "failed unit proof lost the exact executed branch fault");
}

void changing_sparse_indices() {
  constexpr std::uint64_t modify_i0 = 0x160000000001;
  constexpr std::uint64_t load_r1 = load_r0 | (std::uint64_t{1} << 32);
  constexpr std::uint64_t load_r2 = load_r0 | (std::uint64_t{2} << 32);
  constexpr std::uint64_t first_raw = 0x4433221100;
  constexpr std::uint64_t second_raw = 0x8877665500;
  const std::array words{load_r0,
                         modify_i0,
                         std::uint64_t{0},
                         std::uint64_t{0},
                         load_r1,
                         modify_i0,
                         std::uint64_t{0},
                         std::uint64_t{0},
                         load_r2};
  Program program(words);
  const auto native = sharc::Core::compile(program.code.code);
  sharc::Core core(program.memory());
  auto expected = initial();
  fixture::install(core, native, expected);
  check(!core.run(2).has_fault, "sparse index prefix failed");
  const auto saved = core.save();
  const auto result = core.run(7);
  expected.pe[0].data[0][0] = first_raw;
  expected.pe[0].data[0][1] = second_raw;
  expected.dag[0][0].index += 2;
  fixture::boundary(expected, {.pc = expected.pc + 8, .clock = 8});
  check(result.has_fault && result.fault.address == expected.pc &&
            result.progress == sharc::Progress{.instructions = 6, .base_cycles = 6} &&
            core.registers() == expected && program.ram == initial_ram,
        "changing sparse address used an obsolete pointer or lost the exact fault prefix");
  sharc::Core resumed(program.memory());
  (void)resumed.install(native, fixture::Program::variant_id);
  resumed.restore(saved);
  check(resumed.run(7).has_fault && resumed.registers() == expected,
        "DAG snapshot retained a different Core's index continuation");
}

#if defined(__linux__)
void complete_word_space() {
  constexpr std::uint32_t index_before_wrap = 0xfffffffe;
  constexpr unsigned simd_mode_bit = 21;
  constexpr std::uint64_t last_raw = 0x8877665500;
  constexpr std::uint64_t first_raw = 0x4433221100;
  const std::array words{load_r0 | 1, store_r0 | 2};
  Program program(words);
  const fixture::Mapping mapping(fixture::word_space_bytes, PROT_READ | PROT_WRITE);
  auto storage = mapping.bytes();
  const auto last = storage.size() - 4;
  auto &layout = program.code.code.variants[0].layout.regions.back();
  layout.first = 0;
  layout.count = fixture::word_count;
  auto memory = program.memory();
  memory.regions.back().bytes = storage;
  for (const auto mode : fixture::numeric_modes) {
    const auto native = fixture::compile(program.code.code, mode);
    for (const bool simd : {false, true}) {
      std::ranges::copy(initial_ram, storage.begin());
      std::ranges::copy_n(
          initial_ram.begin() + 4, 4, storage.begin() + static_cast<std::ptrdiff_t>(last));
      sharc::Core core(memory, mode);
      auto expected = initial();
      expected.dag[0][0].index = index_before_wrap;
      expected.mode1 = simd ? 1U << simd_mode_bit : 0;
      fixture::install(core, native, expected);
      const auto result = core.run(2);
      expected.pe[0].data[0][0] = last_raw;
      if (simd) { expected.pe[1].data[0][0] = first_raw; }
      fixture::boundary(expected, {.pc = expected.pc + 2, .clock = 2});
      check(fixture::completed(result, 2, 2) && core.registers() == expected &&
                std::ranges::equal(storage.first(4), std::span(initial_ram).last(4)) &&
                std::ranges::equal(storage.subspan(4, 4),
                                   simd ? std::span(initial_ram).first(4)
                                        : std::span(initial_ram).last(4)) &&
                std::ranges::equal(storage.last(4), std::span(initial_ram).last(4)),
            "complete virtual word space lost raw values or wrapped address aliases");
    }
  }
}

void changing_complete_space() {
  constexpr unsigned turns = 16;
  constexpr unsigned input_words = 32;
  constexpr unsigned byte_bits = 8;
  constexpr unsigned simd_bit = 21;
  constexpr std::uint32_t output_address = 0xa0000;
  constexpr std::uint32_t old_status = 0x10000000;
  constexpr std::uint64_t modify_i0 = 0x160000000000;
  constexpr std::uint64_t modify_i1 = 0x160100000000;
  constexpr std::uint64_t decrement_r2 = 0x013e0002a220;
  constexpr std::uint64_t loop = 0x072000fffffa;
  constexpr unsigned instructions = (turns * 7) + 1;
  constexpr unsigned cycles = (turns * 11) - 2;
  constexpr unsigned scalar_sum = 136;
  constexpr unsigned x_sum = 256;
  constexpr unsigned y_sum = 272;
  constexpr std::byte untouched{0xa5};
  const fixture::Mapping mapping(fixture::word_space_bytes, PROT_READ | PROT_WRITE);
  auto storage = mapping.bytes();
  auto input = storage.subspan(std::size_t{ram_address} * 4, std::size_t{input_words} * 4);
  auto output =
      storage.subspan(std::size_t{output_address} * 4, (std::size_t{input_words} + 1) * 4);
  for (unsigned i = 0; i < input_words; ++i) {
    for (unsigned byte = 0; byte < 4; ++byte) {
      input[(i * 4) + byte] = static_cast<std::byte>((i + 1) >> (byte * byte_bits));
    }
  }
  for (const bool simd : {false, true}) {
    const unsigned step = simd ? 2U : 1U;
    const unsigned copied = turns * step;
    const auto copied_bytes = std::size_t{copied} * 4;
    const std::array words{load_r0,
                           fixture::add(3, 3, 0),
                           store_r0 | (std::uint64_t{1} << 41),
                           modify_i0 | step,
                           modify_i1 | step,
                           decrement_r2,
                           loop,
                           std::uint64_t{0}};
    Program program(words);
    auto &layout = program.code.code.variants[0].layout.regions.back();
    layout.first = 0;
    layout.count = fixture::word_count;
    auto memory = program.memory();
    memory.regions.back().bytes = storage;
    for (const auto mode : fixture::numeric_modes) {
      const auto native = fixture::compile(program.code.code, mode);
      sharc::Core core(memory, mode);
      auto expected = initial();
      expected.mode1 = simd ? 1U << simd_bit : 0;
      expected.dag[0][1].index = output_address;
      for (auto &lane : expected.pe) {
        lane.data[0][2] = turns << 8;
        lane.astat = old_status;
      }
      fixture::install(core, native, expected);
      std::ranges::fill(output, untouched);
      const auto result = core.run(instructions);
      expected.dag[0][0].index += copied;
      expected.dag[0][1].index += copied;
      expected.pe[0].data[0][0] = std::uint64_t{simd ? input_words - 1 : turns} << 8;
      expected.pe[0].data[0][2] = 0;
      expected.pe[0].data[0][3] = std::uint64_t{simd ? x_sum : scalar_sum} << 8;
      expected.pe[0].astat = old_status | 9;
      if (simd) {
        expected.pe[1].data[0][0] = 32U << 8;
        expected.pe[1].data[0][2] = 0;
        expected.pe[1].data[0][3] = y_sum << 8;
        expected.pe[1].astat = old_status | 9;
      }
      fixture::boundary(
          expected,
          {.pc = expected.pc + static_cast<std::uint32_t>(words.size()), .clock = cycles});
      check(fixture::completed(result, instructions, cycles) && core.registers() == expected &&
                std::ranges::equal(output.first(copied_bytes), input.first(copied_bytes)) &&
                std::ranges::all_of(output.subspan(copied_bytes),
                                    [](std::byte byte) { return byte == untouched; }),
            "whole-turn grant lost changing DAG indices, SIMD copy, checksum or guard bytes");
    }
  }
}
#endif

void rebound_snapshot() {
  const std::array words{load_r0, std::uint64_t{0}};
  Program first(words);
  Program second(words);
  constexpr std::byte changed_byte{0xff};
  second.ram[0] = changed_byte;
  const auto native = sharc::Core::compile(first.code.code);
  sharc::Core core(first.memory());
  fixture::install(core, native, initial());
  const auto saved = core.save();
  constexpr std::uint64_t first_raw = 0x4433221100;
  constexpr std::uint64_t second_raw = 0x443322ff00;
  check(!core.run(1).has_fault && core.registers().pe[0].data[0][0] == first_raw,
        "direct load byte order differs");
  core.restore(saved);
  core.set_memory(second.memory());
  check(core.run(1).has_fault, "memory replacement retained a native binding");
  core.restore(saved);
  (void)core.install(native, fixture::Program::variant_id);
  check(!core.run(1).has_fault && core.registers().pe[0].data[0][0] == second_raw,
        "snapshot or install retained an obsolete host storage pointer");
}

void code_alias_read() {
  const std::array words{load_r0, std::uint64_t{0}};
  Program program(words);
  auto &layout = program.code.code.variants[0].layout.regions.back();
  layout.storage_id = fixture::Program::storage_id;
  layout.byte_offset = 2;
  layout.byte_stride = fixture::Program::word_bytes;
  layout.writable = false;
  const auto original = program.code.bytes;
  const auto native = sharc::Core::compile(program.code.code);
  sharc::Core core(program.memory());
  auto expected = initial();
  fixture::install(core, native, expected);
  const auto result = core.run(1);
  constexpr std::uint64_t projected_opcode = 0xa000000000;
  expected.pe[0].data[0][0] = projected_opcode;
  fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 1});
  check(fixture::completed(result, 1, 1) && core.registers() == expected &&
            program.code.bytes == original,
        "normal-word projection through a declared code-storage alias differs");
}

void layout_validation() {
  const std::array words{load_r0};
  Program program(words);
  auto &regions = program.code.code.variants[0].layout.regions;
  const auto normal = regions.back();
  regions.push_back(normal);
  fixture::throws<sharc::PreparationError>([&] { (void)sharc::Core::lower(program.code.code); },
                                           "ambiguous data layout accepted");
  regions.pop_back();
  regions.back().count = std::uint64_t{1} << 32;
  fixture::throws<sharc::PreparationError>([&] { (void)sharc::Core::lower(program.code.code); },
                                           "wrapping memory layout accepted");
  regions.back() = normal;
  const auto native = sharc::Core::compile(program.code.code);
  for (unsigned invalid = 0; invalid < 3; ++invalid) {
    auto memory = program.memory();
    if (invalid == 0) { memory.regions.pop_back(); }
    if (invalid == 1) { memory.regions.back().bytes = std::span(program.ram).first(3); }
    if (invalid == 2) { memory.regions.back().bytes = {}; }
    sharc::Core core(memory);
    core.set_registers(initial());
    fixture::throws<sharc::PreparationError>(
        [&] { (void)core.install(native, fixture::Program::variant_id); },
        "invalid direct storage installed");
    check(core.registers() == initial() && core.state().progress == sharc::Progress{},
          "failed install changed guest state");
  }
}

void ordinary_region_alias() {
  constexpr std::uint64_t load_pm_r1 = 0xb00100000000;
  constexpr std::uint64_t raw = 0x12345678ab;
  constexpr std::uint64_t loaded = 0x1234567800;
  const std::array words{store_r0, load_pm_r1};
  Program program(words);
  auto &image = program.code.code;
  auto &layout = image.variants[0].layout;
  auto pm = layout.regions.back();
  pm.space = sharc::MemorySpace::program;
  layout.regions.push_back(pm);
  const auto generic = sharc::Core::lower(image);
  layout.ordinary_regions = true;
  fixture::throws<sharc::PreparationError>(
      [&] { (void)sharc::Core::load(image, {}, generic.artifact()); },
      "ordinary layout accepted an artifact for an unspecified loan");

  const auto native = sharc::Core::compile(image);
  sharc::Core core(program.memory());
  auto registers = initial();
  registers.dag[0][8].index = ram_address;
  registers.pe[0].data[0][0] = raw;
  fixture::install(core, native, registers);
  const auto result = core.run(2);
  registers.pe[0].data[0][1] = loaded;
  fixture::boundary(registers, {.pc = registers.pc + 2, .clock = 2});
  check(fixture::completed(result, 2, 2) && core.registers() == registers,
        "ordinary PM and DM lost their declared physical alias");
}

void incomplete_simd_store() {
  const std::array words{fixture::immediate(1, 9), store_r0};
  Program program(words);
  program.code.code.variants[0].layout.regions.back().count = 1;
  const auto native = sharc::Core::compile(program.code.code);
  sharc::Core core(program.memory());
  auto expected = initial();
  constexpr std::uint32_t simd = 1U << 21;
  constexpr std::uint64_t raw_x = 0x12345678ab;
  constexpr std::uint64_t raw_y = 0xfedcba9867;
  expected.mode1 = simd;
  expected.pe[0].data[0][0] = raw_x;
  expected.pe[1].data[0][0] = raw_y;
  fixture::install(core, native, expected);
  const auto result = core.run(2);
  expected.pe[0].data[0][1] = 9U << 8;
  expected.pe[1].data[0][1] = 9U << 8;
  fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 1});
  check(result.has_fault && result.fault.reason == sharc::FaultReason::unsupported_feature &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            core.registers() == expected && program.ram == initial_ram,
        "missing implicit SIMD storage allowed a partial direct-memory store");
}

void hidden_storage_alias() {
  const std::array words{store_r0};
  Program program(words);
  program.code.code.variants[0].layout.regions.back().count = 1;
  const auto native = sharc::Core::compile(program.code.code);
  for (unsigned alias = 0; alias < 3; ++alias) {
    auto memory = program.memory();
    memory.regions.back().bytes = program.code.bytes;
    if (alias != 0) { memory.regions.back().bytes = memory.regions.back().bytes.subspan(2); }
    if (alias == 2) { std::ranges::reverse(memory.regions); }
    sharc::Core core(memory);
    core.set_registers(initial());
    try {
      (void)core.install(native, fixture::Program::variant_id);
      check(false, "overlapping host storage with distinct ids was installed");
    } catch (const sharc::PreparationError &error) {
      check(error.fault().reason == sharc::FaultReason::invalid_image &&
                error.fault().detail.contains("canonical storage id") &&
                core.registers() == initial() && core.state().progress == sharc::Progress{},
            "ambiguous physical storage did not fail before changing guest state");
    }
  }
}

bool read_store(void * /*owner*/,
                sharc::MemorySpace space,
                sharc::MemoryView view,
                std::uint32_t address,
                std::uint64_t &value) noexcept {
  if (space != sharc::MemorySpace::program || view != sharc::MemoryView::instruction ||
      address != fixture::code_address) {
    return false;
  }
  value = store_r0;
  return true;
}

void refused_code_store(bool declared_alias) {
  const std::array words{store_r0};
  Program program(words);
  auto &regions = program.code.code.variants[0].layout.regions;
  auto memory = program.memory();
  auto registers = initial();
  if (declared_alias) {
    regions.back().storage_id = fixture::Program::storage_id;
    regions.back().count = 1;
    constexpr std::uint64_t raw = 0x12345678ab;
    registers.pe[0].data[0][0] = raw;
  } else {
    regions.erase(regions.begin());
    memory.io.read = read_store;
  }
  const auto original = program.code.bytes;
  const auto native = sharc::Core::compile(program.code.code);
  sharc::Core core(memory);
  fixture::install(core, native, registers);
  const auto result = core.run(1);
  check(result.has_fault && result.fault.reason == sharc::FaultReason::unsupported_feature &&
            result.fault.detail.contains("instruction storage") &&
            result.progress == sharc::Progress{} && core.registers() == registers &&
            program.code.bytes == original && program.ram == initial_ram,
        declared_alias ? "store through a declared code alias changed state"
                       : "store ran without a declared physical code-storage dependency");
}
} // namespace

int main() {
  return fixture::run_test([] {
    fault_prefixes();
    unexecuted_failure();
    changing_sparse_indices();
#if defined(__linux__)
    complete_word_space();
    changing_complete_space();
#endif
    rebound_snapshot();
    refused_code_store(true);
    code_alias_read();
    layout_validation();
    ordinary_region_alias();
    incomplete_simd_store();
    hidden_storage_alias();
    refused_code_store(false);
  });
}
