// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
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
  layout.byte_stride = fixture::Program::word_bytes;
  layout.writable = false;
  const auto original = program.code.bytes;
  for (const auto view : {sharc::MemoryView::normal_word, sharc::MemoryView::extended}) {
    layout.view = view;
    layout.byte_offset = view == sharc::MemoryView::normal_word ? 2 : 1;
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
          "data projection through a declared code-storage alias differs");
  }
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
  const auto instruction = regions.front();
  regions.push_back(instruction);
  regions.back().initialized = false;
  fixture::throws<sharc::PreparationError>([&] { (void)sharc::Core::lower(program.code.code); },
                                           "overlapping direct and callback code layouts accepted");
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

struct ParallelProgram {
  fixture::Program code;

  ParallelProgram(std::span<const std::uint64_t> words, std::uint32_t first, std::uint64_t count)
      : code(words) {
    auto dm = fixture::normal_region(first, count, ram_id);
    auto pm = dm;
    pm.space = sharc::MemorySpace::program;
    auto &layout = code.code.variants[0].layout;
    layout.regions.push_back(dm);
    layout.regions.push_back(pm);
  }
};

constexpr std::uint32_t simd_bit = 1U << 21, cbufen = 1U << 24;
constexpr std::size_t word_bytes = 4;
constexpr unsigned word_shift = 8;

void short_word_alias() {
  constexpr auto normal_address = ram_address + 16;
  constexpr std::uint64_t load_pm_r2 = 0xb00200000000;
  constexpr std::uint64_t negative_offset = 0xffffffff;
  const std::array words{load_r0, store_r0 | negative_offset, load_pm_r2};
  fixture::Program program(words);
  auto &regions = program.code.variants.front().layout.regions;
  regions.push_back({.view = sharc::MemoryView::short_word,
                     .first = ram_address,
                     .count = 4,
                     .storage_id = ram_id,
                     .byte_stride = 2,
                     .initialized = true});
  auto alias = fixture::normal_region(normal_address, 2, ram_id);
  regions.push_back(alias);
  alias.space = sharc::MemorySpace::program;
  regions.push_back(alias);
  const auto native = sharc::Core::compile(program.code);
  for (const bool sign : {false, true}) {
    constexpr unsigned sse = 1U << 14;
    constexpr std::uint32_t x_word = 0x80014433;
    constexpr std::uint32_t y_word = 0xf1238877;
    constexpr std::uint32_t new_x = 0x80018001;
    constexpr std::uint32_t new_y = 0xf123f123;
    std::array<std::byte, 2 * word_bytes> ram{};
    fixture::put_word(ram.data(), x_word);
    fixture::put_word(ram.data() + word_bytes, y_word);
    sharc::Core core(program.memory(ram_id, ram));
    auto expected = initial();
    expected.mode1 = simd_bit | (sign ? sse : 0);
    expected.dag[0][0].index = ram_address + 1;
    expected.dag[0][8].index = normal_address;
    fixture::install(core, native, expected);
    const auto result = core.run(3);
    constexpr std::array<std::uint64_t, 2> x_raw{0x800100, 0xffff800100};
    constexpr std::array<std::uint64_t, 2> y_raw{0xf12300, 0xfffff12300};
    expected.pe[0].data[0][0] = x_raw[sign ? 1U : 0U];
    expected.pe[1].data[0][0] = y_raw[sign ? 1U : 0U];
    expected.pe[0].data[0][2] = std::uint64_t{new_x} << word_shift;
    expected.pe[1].data[0][2] = std::uint64_t{new_y} << word_shift;
    fixture::boundary(expected, {.pc = expected.pc + 3, .clock = 3});
    check(fixture::completed(result, 3, 3) && core.registers() == expected &&
              fixture::get_word(ram.data()) == new_x &&
              fixture::get_word(ram.data() + word_bytes) == new_y,
          "short-word stride, SSE or PM/DM storage alias differs");
  }
}

void wide_words() {
  constexpr auto extended_address = ram_address + 16;
  constexpr std::uint64_t store_s5_i1 = 0xa35500000000;
  constexpr std::uint64_t load_r5_i1 = 0xa20500000000;
  constexpr std::uint64_t store_s6_i2 = 0xa55600000000;
  constexpr std::uint64_t load_r7_i2 = 0xa40700000000;
  constexpr std::uint64_t mixed_packet = 0x22c89a800000; // R4=DM(I1,M3), R5=PM(I10,M11).
  const std::array words{store_s5_i1, load_r5_i1, store_s6_i2, load_r7_i2, mixed_packet};
  fixture::Program program(words);
  auto &regions = program.code.variants.front().layout.regions;
  constexpr unsigned extended_offset = 11;
  regions.push_back({.view = sharc::MemoryView::long_word,
                     .first = ram_address,
                     .count = 1,
                     .storage_id = ram_id,
                     .byte_offset = 1,
                     .byte_stride = 10,
                     .initialized = true});
  regions.push_back({.view = sharc::MemoryView::extended,
                     .first = extended_address,
                     .count = 1,
                     .storage_id = ram_id,
                     .byte_offset = extended_offset,
                     .byte_stride = 6,
                     .initialized = true});
  auto pm = regions.back();
  pm.space = sharc::MemorySpace::program;
  regions.push_back(pm);
  const auto native = sharc::Core::compile(program.code);
  for (const bool broadcast : {false, true}) {
    constexpr std::uint32_t broadcast1 = 1U << 23;
    constexpr std::uint64_t raw_low = 0x2233445501;
    constexpr std::uint64_t raw_high = 0xb1b2b3b4ab;
    constexpr std::uint64_t raw_extended = 0x91a2b3c4d5;
    constexpr auto guard = std::byte{0x67};
    constexpr unsigned ram_bytes = 17;
    std::array<std::byte, ram_bytes> ram;
    ram.fill(guard);
    sharc::Core core(program.memory(ram_id, ram));
    auto expected = initial();
    expected.mode1 = simd_bit | (broadcast ? broadcast1 : 0);
    expected.dag[0][1].index = ram_address;
    expected.dag[0][2].index = extended_address;
    expected.dag[0][10].index = extended_address;
    auto &x = expected.pe[0].data[0];
    auto &y = expected.pe[1].data[0];
    y[4] = raw_high;
    y[5] = raw_low;
    y[6] = raw_extended;
    fixture::install(core, native, expected);
    const auto result = core.run(4);
    x[4] = (raw_high >> word_shift) << word_shift;
    x[5] = (raw_low >> word_shift) << word_shift;
    x[7] = raw_extended;
    if (broadcast) {
      y[4] = x[4];
      y[5] = x[5];
    }
    fixture::boundary(expected, {.pc = expected.pc + 4, .clock = 4});
    constexpr std::array expected_bytes{
        guard,
        std::byte{0x55},
        std::byte{0x44},
        std::byte{0x33},
        std::byte{0x22},
        std::byte{0xb4},
        std::byte{0xb3},
        std::byte{0xb2},
        std::byte{0xb1},
        guard,
        guard,
        std::byte{0xd5},
        std::byte{0xc4},
        std::byte{0xb3},
        std::byte{0xa2},
        std::byte{0x91},
        guard,
    };
    check(fixture::completed(result, 4, 4) && core.registers() == expected && ram == expected_bytes,
          "long or extended word transfer lost width, neighbor, broadcast or guard bytes");
    const auto mixed = core.run(1);
    std::swap(x[4], x[5]);
    if (broadcast) { std::swap(y[4], y[5]); }
    fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 5});
    check(fixture::completed(mixed, 1, 1) && core.registers() == expected && ram == expected_bytes,
          "PM extended-word writeback overrode a DM long-word neighbor");
  }
}

struct Peripheral {
  struct Access {
    sharc::MemorySpace space{};
    std::uint32_t address{};
    std::uint64_t word{};
    bool writing{};
    bool operator==(const Access &) const = default;
  };

  sharc::Core *core{};
  sharc::MemoryView format{sharc::MemoryView::normal_word};
  std::array<std::uint64_t, 2> words{};
  std::array<Access, 4> trace{};
  unsigned calls{}, refusals{}, yield_on{}, fault_on{};

  bool access(sharc::MemorySpace space,
              sharc::MemoryView view,
              std::uint32_t address,
              std::uint64_t &word,
              bool writing) noexcept {
    const auto index = address - ram_address;
    if (view != format || index >= words.size() || calls >= trace.size()) { return false; }
    try {
      (void)core->run(1);
    } catch (const std::logic_error &) { ++refusals; }
    if (!writing) { word = words[index]; }
    trace[calls++] = {.space = space, .address = address, .word = word, .writing = writing};
    if (calls == yield_on) { core->request_yield(); }
    if (calls == fault_on) { return false; }
    if (writing) { words[index] = word; }
    return true;
  }

  sharc::Io binding() noexcept {
    return {.owner = this,
            .read =
                [](void *owner,
                   sharc::MemorySpace space,
                   sharc::MemoryView view,
                   std::uint32_t address,
                   std::uint64_t &value) noexcept {
                  return static_cast<Peripheral *>(owner)->access(
                      space, view, address, value, false);
                },
            .write =
                [](void *owner,
                   sharc::MemorySpace space,
                   sharc::MemoryView view,
                   std::uint32_t address,
                   const std::uint64_t value) noexcept {
                  auto word = value;
                  return static_cast<Peripheral *>(owner)->access(space, view, address, word, true);
                }};
  }
};

void extended_io() {
  const std::array words{load_r0, store_r0 | 1};
  Program program(words);
  auto &region = program.code.code.variants.front().layout.regions.back();
  region.initialized = false;
  region.view = sharc::MemoryView::extended;
  constexpr auto mode = sharc::ExecutionMode::fast_fp32;
  const auto native = fixture::compile(program.code.code, mode);
  constexpr std::uint64_t device_word = 0xaabbcc123456789a;
  constexpr std::uint64_t raw = 0x123456789a;
  Peripheral peripheral{.format = region.view, .words = {device_word, 0}};
  auto memory = program.code.memory();
  memory.io = peripheral.binding();
  sharc::Core core(memory, mode);
  peripheral.core = &core;
  auto expected = initial();
  expected.mode1 = simd_bit;
  fixture::install(core, native, expected);
  const auto result = core.run(2);
  expected.pe[0].data[0][0] = raw;
  fixture::boundary(expected, {.pc = expected.pc + 2, .clock = 2});
  check(fixture::completed(result, 2, 2) && core.registers() == expected && peripheral.calls == 2 &&
            peripheral.words[1] == raw,
        "extended I/O lost its declared width, raw extension or explicit lane");
}

void runtime_io() {
  constexpr std::uint64_t device_word = 0x123456789a;
  constexpr std::uint64_t normal = 0x3456789a;
  constexpr unsigned marker = 9;
  const std::array words{load_r0, store_r0 | 1, fixture::immediate(1, marker)};
  Program program(words);
  program.code.code.variants[0].layout.regions.back().initialized = false;
  const auto native = sharc::Core::compile(program.code.code);
  Peripheral peripheral{.words = {device_word, 0}, .yield_on = 1, .fault_on = 2};
  auto memory = program.code.memory();
  memory.io = peripheral.binding();
  sharc::Core core(memory);
  peripheral.core = &core;
  auto expected = initial();
  fixture::install(core, native, expected);
  const auto result = core.run_until(20);
  expected.pe[0].data[0][0] = normal << word_shift;
  fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 1});
  constexpr auto dm = sharc::MemorySpace::data;
  check(result.execution == sharc::ExecutionState::yielded && !result.has_fault &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            core.registers() == expected && peripheral.calls == 1 && peripheral.refusals == 1 &&
            peripheral.trace[0] ==
                Peripheral::Access{.space = dm, .address = ram_address, .word = device_word},
        "I/O read did not complete before the synchronous yield");
  const auto saved = core.save();
  const auto fault = core.run(2);
  check(fault.has_fault && fault.fault.reason == sharc::FaultReason::access_fault &&
            fault.fault.identity == native.report().identity &&
            fault.fault.address == expected.pc && fault.fault.instruction == words[1] &&
            fault.fault.detail.contains(std::to_string(ram_address + 1)) &&
            fault.progress == sharc::Progress{} && core.registers() == expected &&
            peripheral.trace[1] ==
                Peripheral::Access{
                    .space = dm, .address = ram_address + 1, .word = normal, .writing = true} &&
            peripheral.words[1] == 0 && peripheral.refusals == 2,
        "a false I/O result retired the store or lost fault context");
  core.restore(saved);
  peripheral.yield_on = 3;
  const auto retry = core.run(1);
  fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 2});
  check(retry.execution == sharc::ExecutionState::yielded &&
            retry.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            core.registers() == expected && peripheral.words[1] == normal,
        "yield took effect after the instruction budget boundary or lost the store");
  const auto finish = core.run(1);
  check(fixture::completed(finish, 1, 1) && peripheral.calls == 3 && peripheral.refusals == 3 &&
            core.registers().pe[0].data[0][1] == (marker << word_shift),
        "resume after I/O re-executed the effect or skipped ordinary code");
}

void delayed_io() {
  constexpr auto target = fixture::code_address + 4;
  constexpr std::uint64_t jump_with_delay = 0x063e04000000 | target;
  const std::array words{jump_with_delay,
                         fixture::immediate(1, 1),
                         load_r0,
                         fixture::immediate(2, 2),
                         fixture::immediate(3, 3)};
  Program program(words);
  program.code.code.variants[0].layout.regions.back().initialized = false;
  const auto native = sharc::Core::compile(program.code.code);
  Peripheral peripheral{.words = {7, 0}, .yield_on = 1};
  auto memory = program.code.memory();
  memory.io = peripheral.binding();
  sharc::Core core(memory);
  peripheral.core = &core;
  fixture::install(core, native, initial());
  const auto result = core.run(5);
  const auto after = core.registers();
  check(result.execution == sharc::ExecutionState::yielded &&
            result.progress == sharc::Progress{.instructions = 3, .base_cycles = 4} &&
            after.pc == target && after.pe[0].data[0][0] == (7U << word_shift) &&
            after.pe[0].data[0][1] == (1U << word_shift) && after.pe[0].data[0][2] == 0 &&
            after.pe[0].data[0][3] == 0,
        "I/O yield lost the delayed branch or advanced into its target");
  const auto finish = core.run(1);
  check(fixture::completed(finish, 1, 1) && peripheral.calls == 1 &&
            core.registers().pe[0].data[0][3] == (3U << word_shift),
        "delayed I/O yield lost the prepared destination continuation");
}

void parallel_io() {
  constexpr std::uint64_t packet = 0x34c89a001401; // DM store, PM load and ADD.
  const std::array words{packet, fixture::immediate(1, 9)};
  ParallelProgram program(words, ram_address, 2);
  auto &regions = program.code.code.variants[0].layout.regions;
  for (auto &region : std::span(regions).subspan(1)) { region.initialized = false; }
  const auto native = sharc::Core::compile(program.code.code);
  for (const bool reject : {false, true}) {
    Peripheral peripheral{.yield_on = reject ? 0U : 1U, .fault_on = reject ? 4U : 0U};
    auto memory = program.code.memory();
    memory.io = peripheral.binding();
    sharc::Core core(memory);
    peripheral.core = &core;
    auto expected = initial();
    expected.mode1 = simd_bit;
    expected.dag[0][2].index = ram_address;
    expected.dag[0][10].index = ram_address;
    expected.dag[0][3].modifier = 2;
    expected.dag[0][11].modifier = 2;
    constexpr std::array<std::uint64_t, 2> stored{11, 12};
    constexpr unsigned extension_byte = 0xff;
    for (unsigned lane = 0; lane < stored.size(); ++lane) {
      expected.pe[lane].data[0][4] = (stored[lane] << word_shift) | extension_byte;
    }
    fixture::install(core, native, expected);
    const auto result = core.run(2);
    constexpr auto dm = sharc::MemorySpace::data;
    constexpr auto pm = sharc::MemorySpace::program;
    constexpr std::array<Peripheral::Access, 4> trace{{
        {.space = dm, .address = ram_address, .word = stored[0], .writing = true},
        {.space = dm, .address = ram_address + 1, .word = stored[1], .writing = true},
        {.space = pm, .address = ram_address, .word = stored[0]},
        {.space = pm, .address = ram_address + 1, .word = stored[1]},
    }};
    check(peripheral.trace == trace && peripheral.calls == trace.size() &&
              peripheral.words == stored,
          "parallel callbacks lost DM-before-PM or explicit-before-implicit ordering");
    if (reject) {
      check(result.has_fault && result.fault.reason == sharc::FaultReason::access_fault &&
                result.fault.detail.contains(std::to_string(ram_address + 1)) &&
                result.progress == sharc::Progress{} && core.registers() == expected,
            "late callback rejection committed register or DAG effects");
    } else {
      for (unsigned lane = 0; lane < stored.size(); ++lane) {
        expected.pe[lane].data[0][4] = stored[lane] << word_shift;
      }
      expected.dag[0][2].index += 2;
      expected.dag[0][10].index += 2;
      fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 1});
      check(result.execution == sharc::ExecutionState::yielded &&
                result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
                core.registers().pc == expected.pc && core.registers().dag == expected.dag &&
                core.registers().pe[0].data == expected.pe[0].data &&
                core.registers().pe[1].data == expected.pe[1].data,
            "yield in the first lane interrupted the memory packet");
    }
  }
}

void parallel_writeback() {
  // Both spaces alias. The load/load packet computes R6 = R4 + R1.
  constexpr std::array<std::uint64_t, 4> words{
      0x34c89a001401, 0x24c89a001641, 0x24e89a001401, 0x34e89a801401};
  constexpr std::array<unsigned, 2> old_r4{11, 12};
  constexpr std::array<unsigned, 2> old_r5{21, 22};
  constexpr unsigned extension_byte = 0xff;
  constexpr std::array<std::uint32_t, 4> initial_words{31, 32, 41, 42};
  constexpr std::array<std::array<std::uint32_t, 4>, words.size()> stored{
      {{11, 12, 41, 42}, {31, 32, 41, 42}, {31, 32, 11, 12}, {21, 22, 41, 42}}};
  constexpr std::array<std::array<unsigned, 2>, words.size()> r4{
      {{11, 12}, {31, 32}, {31, 32}, {5, 5}}};
  ParallelProgram program(words, ram_address, initial_words.size());
  auto &variant = program.code.code.variants[0];
  for (unsigned i = 1; i < words.size(); ++i) {
    variant.entries.push_back(fixture::code_address + i);
  }
  for (const auto mode : fixture::numeric_modes) {
    variant.layout.ordinary_regions = mode == sharc::ExecutionMode::fast_fp32;
    const auto native = fixture::compile(program.code.code, mode);
    for (unsigned i = 0; i < words.size(); ++i) {
      std::array<std::byte, word_bytes * initial_words.size()> ram{};
      for (std::size_t j = 0; j < initial_words.size(); ++j) {
        fixture::put_word(ram.data() + (word_bytes * j), initial_words[j]);
      }
      sharc::Core core(program.code.memory(ram_id, ram), mode);
      auto expected = initial();
      expected.pc += i;
      expected.mode1 = simd_bit;
      expected.dag[0][2].index = ram_address;
      expected.dag[0][10].index = ram_address + (i == 1 || i == 2 ? 2U : 0U);
      expected.dag[0][3].modifier = 2;
      expected.dag[0][11].modifier = 2;
      for (unsigned lane = 0; lane < 2; ++lane) {
        auto &regs = expected.pe[lane].data[0];
        regs[0] = 2U << word_shift;
        regs[1] = 3U << word_shift;
        regs[4] = (std::uint64_t{old_r4[lane]} << word_shift) | extension_byte;
        regs[5] = (std::uint64_t{old_r5[lane]} << word_shift) | extension_byte;
      }
      fixture::install(core, native, expected);
      const auto result = core.run_until(1);
      for (unsigned lane = 0; lane < 2; ++lane) {
        expected.pe[lane].data[0][4] = std::uint64_t{r4[i][lane]} << word_shift;
        if (i == 1) {
          expected.pe[lane].data[0][6] = (std::uint64_t{old_r4[lane]} + 3) << word_shift;
        }
      }
      expected.dag[0][2].index += 2;
      expected.dag[0][10].index += 2;
      fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 1});
      check(fixture::completed(result, 1, 1) && core.registers() == expected,
            "parallel packet lost old operands, DAG progress or register priority");
      for (std::size_t j = 0; j < initial_words.size(); ++j) {
        check(fixture::get_word(ram.data() + (word_bytes * j)) == stored[i][j],
              "parallel packet reordered contended PM/DM accesses or stored a new operand");
      }
    }
  }
}

void parallel_refusal() {
  constexpr std::uint64_t packet = 0x34c89a001401; // DM store, PM load and ADD.
  const std::array words{fixture::immediate(2, 5), packet};
  ParallelProgram program(words, ram_address, 2);
  program.code.code.variants[0].layout.regions.back().count = 1;
  auto ram = initial_ram;
  const auto native = sharc::Core::compile(program.code.code);
  sharc::Core core(program.code.memory(ram_id, ram));
  auto expected = initial();
  expected.mode1 = simd_bit;
  expected.dag[0][2].index = ram_address;
  expected.dag[0][10].index = ram_address;
  expected.dag[0][3].modifier = 2;
  expected.dag[0][11].modifier = 2;
  fixture::install(core, native, expected);
  const auto result = core.run(2);
  for (auto &pe : expected.pe) { pe.data[0][2] = 5U << word_shift; }
  fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 1});
  check(result.has_fault && result.fault.reason == sharc::FaultReason::unsupported_feature &&
            result.fault.address == expected.pc && result.fault.instruction == packet &&
            result.fault.detail.contains(std::to_string(ram_address + 1)) &&
            result.fault.detail.contains("space 0") &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            core.registers() == expected && ram == initial_ram,
        "refused PM lane changed a parallel DM store, compute or DAG update");
}

void parallel_circular(std::uint32_t dm_base, std::uint32_t pm_base) {
  // R0 = DM(I7,M6), R1 = PM(I15,M14).
  constexpr std::array<std::uint64_t, 1> words{0x2f81f0800000};
  constexpr unsigned length = 4;
  constexpr std::uint32_t high_dag_banks = (1U << 3) | (1U << 5);
  constexpr std::uint32_t circular_sticky = (1U << 17) | (1U << 18);
  constexpr std::uint32_t circular_interrupts = (1U << 20) | (1U << 21);
  constexpr std::uint32_t first_word = 71;
  constexpr std::uint32_t last_word = 91;
  ParallelProgram program(words, dm_base, length);
  program.code.code.variants[0].layout.regions.back().first = pm_base;
  std::array<std::byte, word_bytes * length> ram{};
  fixture::put_word(ram.data(), first_word);
  fixture::put_word(ram.data() + (word_bytes * (length - 1)), last_word);
  const auto native = sharc::Core::compile(program.code.code);
  for (const bool enabled : {false, true}) {
    sharc::Core core(program.code.memory(ram_id, ram));
    auto expected = initial();
    expected.mode1 = high_dag_banks | (enabled ? cbufen : 0);
    expected.dag[1][7] = {.index = dm_base + length - 1, .length = length, .base = dm_base};
    expected.dag[1][15] = {.index = pm_base, .length = length, .base = pm_base};
    expected.dag[1][6].modifier = 2;
    expected.dag[1][14].modifier = static_cast<std::uint32_t>(-1);
    fixture::install(core, native, expected);
    const auto saved = core.save();
    expected.pe[0].data[0][0] = last_word << word_shift;
    expected.pe[0].data[0][1] = first_word << word_shift;
    expected.dag[1][7].index = dm_base + (enabled ? 1U : length + 1);
    expected.dag[1][15].index = enabled ? pm_base + length - 1 : pm_base - 1;
    if (enabled) {
      expected.pe[0].stky = circular_sticky;
      expected.irptl = circular_interrupts;
    }
    fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 1});
    check(fixture::completed(core.run(1), 1, 1) && core.registers() == expected,
          "parallel circular packet lost old addresses, wraps, banks or overflow state");
    core.restore(saved);
    check(fixture::completed(core.run_until(1), 1, 1) && core.registers() == expected,
          "snapshot replay changed parallel circular post-modification");
  }
}

void parallel_reverse() {
  // R0 = DM(I0,M0), R1 = PM(I8,M8). Reversal changes the address bus only.
  constexpr std::uint64_t packet = 0x200000800000;
  constexpr std::array words{packet, packet};
  constexpr std::uint32_t first = 0xc1000;
  constexpr std::uint32_t reversed_first = 0x83000;
  constexpr unsigned dm_step = 32;
  constexpr unsigned pm_step = 16;
  constexpr unsigned dm_step_reversed = 0x04000000;
  constexpr unsigned pm_step_reversed = 0x08000000;
  constexpr std::uint32_t reverse_both = 3;
  ParallelProgram program(words, first, dm_step + 2);
  std::array<std::byte, word_bytes *(dm_step + 2)> ram{};
  for (const auto offset : {0U, 1U, pm_step, pm_step + 1, dm_step, dm_step + 1}) {
    fixture::put_word(ram.data() + (offset * word_bytes), offset + 1);
  }
  const auto lowered = sharc::Core::lower(program.code.code);
  const auto cached = sharc::Core::load(program.code.code, {}, lowered.artifact());
  check(cached.identity() == lowered.identity() && !cached.bitcode().empty(),
        "cached semantic image lost the BR0/BR8 contexts");
  const auto native = sharc::Core::compile(program.code.code);
  sharc::Core core(program.code.memory(ram_id, ram));
  auto expected = initial();
  expected.mode1 = simd_bit | reverse_both;
  expected.dag[0][0] = {.index = reversed_first, .modifier = dm_step_reversed};
  expected.dag[0][8] = {.index = reversed_first, .modifier = pm_step_reversed};
  fixture::install(core, native, expected);
  for (const auto step : {0U, 1U}) {
    for (unsigned lane = 0; lane < 2; ++lane) {
      expected.pe[lane].data[0][0] = (dm_step * step + lane + 1) << word_shift;
      expected.pe[lane].data[0][1] = (pm_step * step + lane + 1) << word_shift;
    }
    expected.dag[0][0].index += dm_step_reversed;
    expected.dag[0][8].index += pm_step_reversed;
    fixture::boundary(expected, {.pc = expected.pc + 1, .clock = step + 1});
    check(fixture::completed(core.run(1), 1, 1) && core.registers() == expected,
          "bit-reversed PM/DM address or paired lane offset changed the raw index update");
  }
}

void parallel_broadcast() {
  // R4 = DM(I1,M1), R4 = PM(I9,M9).
  constexpr std::array<std::uint64_t, 1> words{0x22484a000000};
  constexpr std::uint32_t dm_broadcast = 1U << 23;
  constexpr std::uint32_t pm_broadcast = 1U << 22;
  constexpr std::uint64_t first_raw = 0x4433221100;
  constexpr std::uint64_t second_raw = 0x8877665500;
  ParallelProgram program(words, ram_address, 2);
  auto ram = initial_ram;
  const auto native = sharc::Core::compile(program.code.code);
  for (const bool broadcast_dm : {false, true}) {
    sharc::Core core(program.code.memory(ram_id, ram));
    auto expected = initial();
    expected.mode1 = pm_broadcast | (broadcast_dm ? dm_broadcast : 0);
    expected.dag[0][1].index = ram_address;
    expected.dag[0][9].index = ram_address + 1;
    fixture::install(core, native, expected);
    expected.pe[0].data[0][4] = first_raw;
    expected.pe[1].data[0][4] = broadcast_dm ? first_raw : second_raw;
    fixture::boundary(expected, {.pc = expected.pc + 1, .clock = 1});
    check(fixture::completed(core.run(1), 1, 1) && core.registers() == expected,
          "SISD broadcast lost the complementary register or per-lane DM priority");
  }
}

void parallel_interlock() {
  // R0 = DM(I2,M4), R1 = PM(I10,M10) immediately after writing the alternate M4.
  constexpr unsigned m4_ureg = 0x24;
  constexpr std::uint64_t packet = 0x250090800000;
  constexpr std::array words{fixture::immediate(m4_ureg, 2), packet};
  constexpr unsigned alternate_dag1_high = 1U << 3;
  constexpr std::uint64_t first_raw = 0x4433221100;
  constexpr std::uint64_t second_raw = 0x8877665500;
  ParallelProgram program(words, ram_address, 2);
  auto ram = initial_ram;
  const auto native = sharc::Core::compile(program.code.code);
  sharc::Core core(program.code.memory(ram_id, ram));
  auto expected = initial();
  expected.mode1 = alternate_dag1_high;
  expected.dag[0][2].index = ram_address;
  expected.dag[0][10].index = ram_address + 1;
  fixture::install(core, native, expected);
  check(fixture::completed(core.run(1), 1, 1), "modifier load did not retire");
  const auto saved = core.save();
  expected.dag[1][4].modifier = 2;
  expected.dag[0][2].index += 2;
  expected.pe[0].data[0][0] = first_raw;
  expected.pe[0].data[0][1] = second_raw;
  fixture::boundary(expected, {.pc = expected.pc + 2, .clock = 4});
  check(fixture::completed(core.run_until(2), 1, 3) && core.registers() == expected,
        "parallel packet lost cross-bank M4 interlock or crossed an incomplete bound");
  core.restore(saved);
  check(fixture::completed(core.run(1), 1, 3) && core.registers() == expected,
        "snapshot lost the modifier interlock before a parallel DAG packet");
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

enum class CodeStorage : std::uint8_t { aliased, callback, undeclared };

struct CodeWriter {
  static constexpr auto next = fixture::code_address + 1;
  static constexpr unsigned replacement_variant = 8;
  static constexpr unsigned before = 7;
  static constexpr unsigned after = 12;
  static constexpr auto offset = fixture::Program::word_bytes;
  fixture::Program program;

  explicit CodeWriter(std::uint64_t store)
      : program(std::array{store, fixture::immediate(1, before), std::uint64_t{0}}) {
    auto alias = fixture::normal_region(ram_address, 1, fixture::Program::storage_id);
    alias.byte_offset = offset;
    program.code.variants.front().layout.regions.push_back(alias);
  }

  void allow(std::uint32_t entry = next) {
    auto target = program.code.variants.front();
    target.id = replacement_variant;
    target.entries = {entry};
    constexpr auto low_parcel_of_second_word = 5;
    target.spans.front().parcels[low_parcel_of_second_word] = after;
    program.code.variants.push_back(target);
    program.code.transitions.push_back({.from_variant = fixture::Program::variant_id,
                                        .address = entry,
                                        .to_variants = {replacement_variant}});
  }
};

void rewrite_and_resume() {
  constexpr std::uint64_t store_pm_r0 = 0xb10000000000;
  for (const bool program_bus : {false, true}) {
    CodeWriter writer(program_bus ? store_pm_r0 : store_r0);
    auto &program = writer.program;
    if (program_bus) {
      auto &alias = program.code.variants.front().layout.regions.back();
      alias.space = sharc::MemorySpace::program;
      alias.view = sharc::MemoryView::short_word;
      alias.byte_stride = 2;
    }
    writer.allow();
    const auto native = sharc::Core::compile(program.code);
    sharc::Core core(program.memory());
    auto start = initial();
    start.dag[0][8].index = ram_address;
    start.pe[0].data[0][0] = CodeWriter::before << word_shift;
    fixture::install(core, native, start);
    const auto original = program.bytes;
    check(fixture::completed(core.run(2), 2, 2) && program.bytes == original &&
              core.registers().pe[0].data[0][1] == (CodeWriter::before << word_shift),
          "unchanged code alias store invalidated the active variant");

    start.pe[0].data[0][0] = CodeWriter::after << word_shift;
    core.set_registers(start);
    const auto before = core.save();
    const auto result = core.run(10);
    check(result.execution == sharc::ExecutionState::code_transfer && !result.has_fault &&
              result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
              core.registers().pc == CodeWriter::next && core.registers().pe[0].data[0][1] == 0 &&
              fixture::get_word(program.bytes.data() + CodeWriter::offset) == CodeWriter::after,
          "code-alias write did not stop before the replaced native instruction");
    const auto pending = core.save();
    core.restore(before);
    check(core.run(1).fault.reason == sharc::FaultReason::unprepared,
          "snapshot recovered the obsolete binding after a code change");
    fixture::throws<sharc::PreparationError>(
        [&] { (void)core.install(native, fixture::Program::variant_id); },
        "installation accepted the old opcode after the code change");
    core.restore(pending);
    (void)core.install(native, CodeWriter::replacement_variant);
    check(fixture::completed(core.run(1), 1, 1) &&
              core.registers().pe[0].data[0][1] == (CodeWriter::after << word_shift),
          "code change did not resume from the prepared replacement");
  }
}

void rewrite_without_target() {
  CodeWriter writer(store_r0);
  writer.allow(CodeWriter::next + 1);
  auto &program = writer.program;
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  auto start = initial();
  start.pe[0].data[0][0] = CodeWriter::after << word_shift;
  fixture::install(core, native, start);
  const auto saved = core.save();
  const auto result = core.run(10);
  check(result.has_fault && result.fault.reason == sharc::FaultReason::unsupported_feature &&
            result.fault.address == CodeWriter::next &&
            result.fault.identity == native.report().identity &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            fixture::get_word(program.bytes.data() + CodeWriter::offset) == CodeWriter::after,
        "code write accepted a transition for the wrong continuation");
  core.restore(saved);
  check(core.run(1).fault.reason == sharc::FaultReason::unprepared,
        "unsupported code rewrite retained an outdated binding");
}

void code_padding_store() {
  CodeWriter writer(store_r0);
  auto &program = writer.program;
  auto &layouts = program.code.variants.front().layout.regions;
  constexpr std::array padding{std::byte{0}, std::byte{0}};
  layouts.front().byte_stride = CodeWriter::offset + padding.size();
  constexpr auto second_end = std::ptrdiff_t{2} * CodeWriter::offset;
  program.bytes.insert(program.bytes.begin() + second_end, padding.begin(), padding.end());
  program.bytes.insert(program.bytes.begin() + CodeWriter::offset, padding.begin(), padding.end());
  layouts.back().view = sharc::MemoryView::short_word;
  layouts.back().byte_stride = padding.size();
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  auto start = initial();
  constexpr unsigned padding_value = 0x5511;
  constexpr std::array written_padding{std::byte{0x11}, std::byte{0x55}};
  start.pe[0].data[0][0] = padding_value << word_shift;
  fixture::install(core, native, start);
  check(fixture::completed(core.run(2), 2, 2) &&
            std::ranges::equal(std::span(program.bytes).subspan(CodeWriter::offset, padding.size()),
                               written_padding) &&
            core.registers().pe[0].data[0][1] == (CodeWriter::before << word_shift),
        "a store in physical instruction padding was treated as changing code");
}

void rewrite_before_io_fault() {
  constexpr std::uint64_t store_dm_read_pm = 0x34c89a000000;
  CodeWriter writer(store_dm_read_pm);
  writer.allow();
  auto &program = writer.program;
  auto peripheral_alias = fixture::normal_region(ram_address, 1, ram_id);
  peripheral_alias.space = sharc::MemorySpace::program;
  peripheral_alias.initialized = false;
  program.code.variants.front().layout.regions.push_back(peripheral_alias);
  auto memory = program.memory();
  Peripheral peripheral{.fault_on = 1};
  memory.io = peripheral.binding();
  sharc::Core core(memory);
  peripheral.core = &core;
  auto start = initial();
  start.dag[0][2].index = ram_address;
  start.dag[0][10].index = ram_address;
  start.pe[0].data[0][4] = CodeWriter::after << word_shift;
  const auto native = sharc::Core::compile(program.code);
  fixture::install(core, native, start);
  const auto saved = core.save();
  const auto fault = core.run(1);
  check(fault.has_fault && fault.fault.reason == sharc::FaultReason::access_fault &&
            fault.fault.address == start.pc && fault.progress == sharc::Progress{} &&
            core.registers() == start && peripheral.calls == 1 &&
            fixture::get_word(program.bytes.data() + CodeWriter::offset) == CodeWriter::after,
        "late peripheral refusal lost the earlier code store or retired the packet");
  core.restore(saved);
  check(core.run(1).fault.reason == sharc::FaultReason::unprepared,
        "late peripheral refusal retained a modified source binding");
}

void rewrite_inside_delay(bool compatible) {
  constexpr auto slot = fixture::code_address + 2;
  constexpr auto taken = slot + 2;
  constexpr std::uint64_t delayed_jump = 0x063e04000000 | taken;
  constexpr auto slot_offset = 2 * fixture::Program::word_bytes;
  const std::array words{delayed_jump,
                         store_r0,
                         fixture::immediate(1, CodeWriter::before),
                         fixture::immediate(2, 1),
                         fixture::immediate(3, 2)};
  fixture::Program program(words);
  auto alias = fixture::normal_region(ram_address, 1, fixture::Program::storage_id);
  alias.byte_offset = slot_offset;
  program.code.variants.front().layout.regions.push_back(alias);
  auto target = program.code.variants.front();
  target.id = CodeWriter::replacement_variant;
  target.entries = {slot};
  target.spans.front().parcels[8] = CodeWriter::after;
  if (!compatible) { target.spans.front().parcels[2] ^= 1; }
  program.code.variants.push_back(target);
  program.code.transitions.push_back(
      {.from_variant = fixture::Program::variant_id, .address = slot, .to_variants = {target.id}});
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  auto start = initial();
  start.pe[0].data[0][0] = CodeWriter::after << word_shift;
  fixture::install(core, native, start);
  const auto result = core.run(10);
  check(result.progress == sharc::Progress{.instructions = 2, .base_cycles = 2} &&
            core.registers().pc == slot && core.registers().pe[0].data[0][1] == 0 &&
            fixture::get_word(program.bytes.data() + slot_offset) == CodeWriter::after,
        "a code store executed the stale second delay slot");
  if (!compatible) {
    check(result.has_fault && result.fault.reason == sharc::FaultReason::unsupported_feature &&
              result.fault.address == slot && result.fault.detail.contains("no compatible delay"),
          "a code store accepted a destination with a different delayed branch origin");
    return;
  }
  check(result.execution == sharc::ExecutionState::code_transfer && !result.has_fault,
        "rewrite did not hand off with a live delayed branch");
  sharc::Core replay(program.memory());
  replay.restore(core.save());
  (void)replay.install(native, target.id);
  const auto second_slot = replay.run(1);
  const auto boundary = replay.registers();
  check(fixture::completed(second_slot, 1, 2) && boundary.pc == taken &&
            boundary.pe[0].data[0][1] == (CodeWriter::after << word_shift) &&
            boundary.pe[0].data[0][2] == 0,
        "the replaced slot lost the latched redirect or its refill cycle");
  check(fixture::completed(replay.run(1), 1, 1) &&
            replay.registers().pe[0].data[0][3] == (2U << word_shift),
        "the replaced slot did not continue in the prepared target code");
}

void refused_code_store(CodeStorage storage) {
  const std::array words{store_r0};
  Program program(words);
  auto &regions = program.code.code.variants[0].layout.regions;
  auto memory = program.memory();
  auto registers = initial();
  if (storage == CodeStorage::aliased) {
    regions.back().storage_id = fixture::Program::storage_id;
    regions.back().count = 1;
    constexpr std::uint64_t raw = 0x12345678ab;
    registers.pe[0].data[0][0] = raw;
  } else {
    if (storage == CodeStorage::callback) {
      regions.front().initialized = false;
    } else {
      regions.erase(regions.begin());
    }
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
        storage == CodeStorage::aliased ? "store through a declared code alias changed state"
                                        : "store ran without proven physical code storage");
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
    refused_code_store(CodeStorage::aliased);
    code_alias_read();
    layout_validation();
    ordinary_region_alias();
    incomplete_simd_store();
    short_word_alias();
    wide_words();
    extended_io();
    runtime_io();
    delayed_io();
    parallel_io();
    parallel_writeback();
    parallel_refusal();
    parallel_circular(ram_address, ram_address);
    parallel_circular(std::numeric_limits<std::uint32_t>::max() - 3, 0);
    parallel_reverse();
    parallel_broadcast();
    parallel_interlock();
    hidden_storage_alias();
    rewrite_and_resume();
    rewrite_without_target();
    code_padding_store();
    rewrite_before_io_fault();
    rewrite_inside_delay(true);
    rewrite_inside_delay(false);
    refused_code_store(CodeStorage::callback);
    refused_code_store(CodeStorage::undeclared);
  });
}
