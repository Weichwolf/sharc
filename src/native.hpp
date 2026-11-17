// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "decode.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sharc/core.hpp>
#include <span>
#include <string>
#include <vector>

namespace sharc::detail {
// Private generated-code ABI: arrays of pointers and numeric control words.
// Public Registers/State object layouts are never embedded in bitcode.
inline constexpr std::size_t physical_data_count = 64;
inline constexpr std::size_t astat_slot = physical_data_count;
inline constexpr std::size_t stky_slot = astat_slot + 2;
inline constexpr std::size_t pc_slot = stky_slot + 2;
inline constexpr std::size_t faddr_slot = pc_slot + 1;
inline constexpr std::size_t daddr_slot = faddr_slot + 1;
inline constexpr std::size_t clock_slot = daddr_slot + 1;
inline constexpr std::size_t hazard_slot = clock_slot + 1;
inline constexpr std::size_t register_pointer_count = hazard_slot + 1;
inline constexpr std::uint32_t high_bank_mode = 1U << 7;
inline constexpr std::uint32_t low_bank_mode = 1U << 10;
inline constexpr std::uint32_t saturation_mode = 1U << 13;
inline constexpr std::uint32_t truncation_mode = 1U << 15;
inline constexpr std::uint32_t simd_mode = 1U << 21;

enum ControlSlot : std::uint8_t {
  remaining_instructions,
  remaining_cycles,
  retired_instructions,
  retired_cycles,
  exit_reason,
  control_count
};
enum class NativeExit : std::uint8_t { bound, undeclared };
using NativeEntry = void (*)(void **, std::uint64_t *);
using RegisterPointers = std::array<void *, register_pointer_count>;
using ControlWords = std::array<std::uint64_t, control_count>;

struct NativeUnit {
  std::uint32_t variant{}, entry{}, mask{}, context{};
  std::string symbol;
};

struct PreparedImage {
  CodeImage code;
  CompileOptions options;
  std::string identity;
  std::vector<std::byte> bitcode;
  std::vector<std::string> exports;
  std::vector<NativeUnit> units;
};

struct CompiledSymbols {
  NativeSymbols symbols;
  std::size_t native_bytes{};
};

[[nodiscard]] std::shared_ptr<const PreparedImage> lower_image(const CodeImage &image,
                                                               const CompileOptions &options);
[[nodiscard]] std::vector<std::byte> serialize_image(const PreparedImage &image);
[[nodiscard]] std::shared_ptr<const PreparedImage> load_image(const CodeImage &code,
                                                              const CompileOptions &options,
                                                              std::span<const std::byte> artifact);
[[nodiscard]] CompiledSymbols compile_image(const PreparedImage &image);
void validate_code_storage(const PreparedImage &image, std::uint32_t variant, const Memory &memory);
} // namespace sharc::detail
