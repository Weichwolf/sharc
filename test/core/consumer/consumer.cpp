// SPDX-License-Identifier: GPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>

int main() try {
  // Independently authored: R0 = 2; R1 = 3; R2 = R0 + R1.
  constexpr std::array<std::uint64_t, 3> words{0x0f0000000002, 0x0f0100000003, 0x013e00001201};
  constexpr unsigned word_bytes = 6;
  constexpr unsigned byte_bits = 8;
  constexpr std::uint32_t entry = 0x100;
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
  sharc::CodeImage image;
  image.variants.push_back(variant);
  const auto native = sharc::Core::compile(image);
  sharc::Core core{
      sharc::Memory{.regions = {{.storage_id = 1, .bytes = storage, .generation = 1}}, .io = {}}};
  sharc::Registers registers;
  registers.pc = entry;
  core.set_registers(registers);
  (void)core.install(native, 0);
  const auto snapshot = core.save();
  const auto result = core.run(words.size());
  if (result.has_fault || result.progress.instructions != words.size() ||
      core.registers().pe[0].data[0][2] != (5U << byte_bits)) {
    return 1;
  }
  core.restore(snapshot);
  if (sharc::Core::compiler_identity().empty()) { return 1; }
  return core.state().progress.instructions == 0 ? 0 : 1;
} catch (const std::exception &) { return 1; }
