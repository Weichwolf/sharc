// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bench {
inline constexpr std::size_t fir_taps = 64;

// The input includes fir_taps - 1 history words. All words hold signed Q1.31.
void fir(std::span<const std::uint32_t> input,
         std::span<const std::uint32_t, fir_taps> coefficients,
         std::span<std::uint32_t> output);

inline constexpr std::size_t biquad_sections = 4;
using BiquadCoefficients = std::array<std::array<std::uint32_t, 5>, biquad_sections>;
using BiquadState = std::array<std::uint32_t, biquad_sections * 2>;

BiquadState biquads(std::span<const std::uint32_t> input,
                    const BiquadCoefficients &coefficients,
                    BiquadState state,
                    std::span<std::uint32_t> output);

inline constexpr std::size_t fft_size = 256;
inline constexpr std::size_t fft_words = 2 * fft_size;
using FftTwiddles = std::array<std::uint32_t, fft_size>;

// Input is bit-reversed complex Q1.31. Output is in natural bin order.
void fft(std::span<std::uint32_t, fft_words> values, const FftTwiddles &twiddles);

struct Control {
  std::uint32_t state{}, accumulator{}, last_handler{};
  bool operator==(const Control &) const = default;
};

Control control(std::uint32_t transitions, Control initial);

std::uint32_t copy_checksum(std::span<const std::uint32_t> input,
                            std::span<std::uint32_t> output,
                            std::uint32_t passes);
} // namespace bench
