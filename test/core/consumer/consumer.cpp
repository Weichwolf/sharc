// SPDX-License-Identifier: GPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>

int main() try {
  // R0 = 2; call a leaf that sets R1 = 3; return; R3 = 9; jump to R2 = R0 + R1.
  constexpr std::array<std::uint64_t, 12> words{0x0f0000000002,
                                                0x06be00000104,
                                                0x0f0300000009,
                                                0x063e00000106,
                                                0x0f0100000003,
                                                0x0a3e00000000,
                                                0x013e00001201,
                                                0xa00400000000,
                                                0x160000000001,
                                                0,
                                                0,
                                                0xa00500000000};
  constexpr unsigned word_bytes = 6;
  constexpr unsigned byte_bits = 8;
  constexpr std::uint32_t entry = 0x100;
  constexpr std::uint32_t ram_address = 0x90000;
  constexpr std::uint32_t ram_id = 2;
  constexpr std::uint64_t expected_ram = 0x1234567800;
  constexpr std::uint64_t expected_next = 0x90abcdef00;
  constexpr std::array ram_initial{std::byte{0x78},
                                   std::byte{0x56},
                                   std::byte{0x34},
                                   std::byte{0x12},
                                   std::byte{0xef},
                                   std::byte{0xcd},
                                   std::byte{0xab},
                                   std::byte{0x90}};
  auto ram = ram_initial;
  std::array<std::byte, words.size() * word_bytes> storage{};
  sharc::CodeSpan span{
      .address = entry, .instruction_set = sharc::InstructionSet::isa, .parcels = {}};
  for (std::size_t i = 0; i < words.size(); ++i) {
    for (unsigned parcel = 3; parcel != 0; --parcel) {
      span.parcels.push_back(static_cast<std::uint16_t>(words[i] >> ((parcel - 1) * 16)));
    }
    for (unsigned byte = 0; byte < word_bytes; ++byte) {
      storage[(i * word_bytes) + byte] = static_cast<std::byte>(words[i] >> (byte * byte_bits));
    }
  }
  sharc::CodeVariant variant;
  variant.entries = {entry};
  variant.spans.push_back(span);
  variant.layout.regions.push_back({.space = sharc::MemorySpace::program,
                                    .view = sharc::MemoryView::instruction,
                                    .first = entry,
                                    .count = words.size(),
                                    .storage_id = 1,
                                    .byte_offset = 0,
                                    .byte_stride = word_bytes,
                                    .readable = true,
                                    .writable = false,
                                    .initialized = true});
  variant.layout.regions.push_back({.space = sharc::MemorySpace::data,
                                    .view = sharc::MemoryView::normal_word,
                                    .first = ram_address,
                                    .count = 2,
                                    .storage_id = ram_id,
                                    .byte_offset = 0,
                                    .byte_stride = 4,
                                    .readable = true,
                                    .writable = false,
                                    .initialized = true});
  sharc::CodeImage image;
  image.variants.push_back(variant);
  const auto native = sharc::Core::compile(image);
  sharc::Core core{sharc::Memory{.regions = {{.storage_id = 1, .bytes = storage, .generation = 1},
                                             {.storage_id = ram_id, .bytes = ram, .generation = 1}},
                                 .io = {}}};
  sharc::Registers registers;
  registers.pc = entry;
  registers.dag[0][0].index = ram_address;
  core.set_registers(registers);
  (void)core.install(native, 0);
  const auto snapshot = core.save();
  constexpr unsigned prefix = 2;
  constexpr unsigned suffix = words.size() - prefix;
  constexpr std::uint32_t call_tag = 1U << 24;
  constexpr std::uint32_t empty = 1U << 22;
  const auto called = core.run(prefix);
  if (called.has_fault ||
      called.progress != sharc::Progress{.instructions = prefix, .base_cycles = 5} ||
      core.registers().pc != entry + 4 || core.registers().pc_stack_pointer != 1 ||
      core.registers().pc_stack[0] != ((entry + 2) | call_tag)) {
    return 1;
  }
  const auto callee = core.save();
  const auto result = core.run(suffix);
  constexpr unsigned suffix_cycles = 16;
  const auto final = core.state();
  if (result.has_fault ||
      result.progress != sharc::Progress{.instructions = suffix, .base_cycles = suffix_cycles} ||
      final.registers.pe[0].data[0][2] != (5U << byte_bits) ||
      final.registers.pe[0].data[0][3] != (9U << byte_bits) ||
      final.registers.pe[0].data[0][4] != expected_ram ||
      final.registers.pe[0].data[0][5] != expected_next ||
      final.registers.dag[0][0].index != ram_address + 1 || final.registers.pe[0].stky != empty ||
      final.registers.pc != entry + words.size() || final.registers.pc_stack_pointer != 0 ||
      final.registers.pc_stack[0] != ((entry + 2) | call_tag)) {
    return 1;
  }
  core.restore(callee);
  if (core.run(suffix).has_fault || core.state() != final) { return 1; }
  core.restore(snapshot);
  if (sharc::Core::compiler_identity().empty()) { return 1; }
  return core.state().progress.instructions == 0 ? 0 : 1;
} catch (const std::exception &) { return 1; }
