// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/SHA256.h>
#include <span>
#include <string>
#include <vector>

namespace bench {
inline constexpr unsigned filter_frames = 1024;
inline constexpr unsigned filter_blocks = 256;
inline constexpr unsigned filter_samples = filter_frames * filter_blocks;

inline std::vector<std::uint32_t> filter_signal(std::size_t history = 0) {
  constexpr std::uint32_t seed = 0x243f6a88;
  constexpr std::uint32_t multiplier = 1664525;
  constexpr std::uint32_t increment = 1013904223;
  constexpr unsigned pulse_period = 97;
  constexpr auto rail = std::uint32_t{1} << 31;
  std::vector<std::uint32_t> input(filter_samples + history);
  auto state = seed;
  for (unsigned i = 0; i < filter_samples; ++i) {
    state = (state * multiplier) + increment;
    auto sample = static_cast<std::uint32_t>((std::int64_t{state} - rail) / 2);
    if (i % pulse_period == 0) { sample = rail - 1; }
    if (i % pulse_period == 1) { sample = rail; }
    input[i + history] = sample;
  }
  return input;
}

inline std::string word_digest(std::span<const std::uint32_t> words) {
  constexpr unsigned byte_bits = 8;
  llvm::SHA256 hash;
  for (const auto word : words) {
    std::array<std::uint8_t, sizeof(word)> bytes{};
    for (unsigned i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<std::uint8_t>(word >> (i * byte_bits));
    }
    hash.update(bytes);
  }
  return llvm::toHex(hash.final(), true);
}
} // namespace bench
