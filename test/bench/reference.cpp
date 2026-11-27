// SPDX-License-Identifier: GPL-3.0-only
#include "reference.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace {
std::int64_t signed_word(std::uint32_t word) {
  return std::bit_cast<std::int32_t>(word);
}

std::uint32_t rounded_q31(std::int64_t sum) {
  constexpr unsigned fraction_bits = 31;
  constexpr auto unit = std::uint64_t{1} << fraction_bits;
  // Unsigned subtraction also handles the full negative signed magnitude.
  const auto bits = static_cast<std::uint64_t>(sum);
  const auto magnitude = sum < 0 ? 0 - bits : bits;
  auto whole = magnitude >> fraction_bits;
  const auto fraction = magnitude & (unit - 1);
  if (fraction > unit / 2 || (fraction == unit / 2 && (whole & 1) != 0)) { ++whole; }
  const auto rounded = static_cast<std::int64_t>(whole);
  return static_cast<std::uint32_t>(
      std::clamp(sum < 0 ? -rounded : rounded,
                 std::int64_t{std::numeric_limits<std::int32_t>::min()},
                 std::int64_t{std::numeric_limits<std::int32_t>::max()}));
}
} // namespace

void bench::fir(std::span<const std::uint32_t> input,
                std::span<const std::uint32_t, fir_taps> coefficients,
                std::span<std::uint32_t> output) {
  assert(input.size() == output.size() + fir_taps - 1);
  for (std::size_t i = 0; i < output.size(); ++i) {
    std::int64_t sum{};
    // Bounds for the shared coefficients fit a signed 64-bit dot product.
    for (std::size_t tap = 0; tap < coefficients.size(); ++tap) {
      const auto sample = std::bit_cast<std::int32_t>(input[i + fir_taps - 1 - tap]);
      const auto coefficient = std::bit_cast<std::int32_t>(coefficients[tap]);
      sum += std::int64_t{sample} * coefficient;
    }
    output[i] = rounded_q31(sum);
  }
}

bench::BiquadState bench::biquads(std::span<const std::uint32_t> input,
                                  const BiquadCoefficients &coefficients,
                                  BiquadState state,
                                  std::span<std::uint32_t> output) {
  assert(input.size() == output.size());
  constexpr auto unit = std::int64_t{1} << 31;
  for (std::size_t i = 0; i < input.size(); ++i) {
    auto x = signed_word(input[i]);
    for (std::size_t stage = 0; stage < coefficients.size(); ++stage) {
      const auto &[b0, b1, b2, a1, a2] = coefficients[stage];
      auto &s0 = state[stage * 2];
      auto &s1 = state[(stage * 2) + 1];
      const auto y = signed_word(rounded_q31((signed_word(s0) * unit) + (signed_word(b0) * x)));
      s0 = rounded_q31((signed_word(s1) * unit) + (signed_word(b1) * x) - (signed_word(a1) * y));
      s1 = rounded_q31((signed_word(b2) * x) - (signed_word(a2) * y));
      x = y;
    }
    output[i] = static_cast<std::uint32_t>(x);
  }
  return state;
}

void bench::fft(std::span<std::uint32_t, fft_words> values, const FftTwiddles &twiddles) {
  constexpr auto half_unit = std::int64_t{1} << 30;
  for (std::size_t half = 1; half < fft_size; half *= 2) {
    const auto stride = fft_size / (half * 2);
    // Iterate the twiddle first. The guest walks one contiguous group at a time.
    for (std::size_t j = 0; j < half; ++j) {
      const auto tw = 2 * j * stride;
      const auto wr = signed_word(twiddles[tw]);
      const auto wi = signed_word(twiddles[tw + 1]);
      for (std::size_t k = j; k < fft_size; k += half * 2) {
        const auto a = k * 2;
        const auto b = (k + half) * 2;
        const auto ar = signed_word(values[a]);
        const auto ai = signed_word(values[a + 1]);
        const auto br = signed_word(values[b]);
        const auto bi = signed_word(values[b + 1]);
        const auto real = signed_word(rounded_q31((br * wr) - (bi * wi)));
        const auto imag = signed_word(rounded_q31((br * wi) + (bi * wr)));
        values[a] = rounded_q31((ar + real) * half_unit);
        values[a + 1] = rounded_q31((ai + imag) * half_unit);
        values[b] = rounded_q31((ar - real) * half_unit);
        values[b + 1] = rounded_q31((ai - imag) * half_unit);
      }
    }
  }
}

std::uint32_t bench::copy_checksum(std::span<const std::uint32_t> input,
                                   std::span<std::uint32_t> output,
                                   std::uint32_t passes) {
  std::uint32_t checksum{};
  for (std::uint32_t pass = 0; pass < passes; ++pass) {
    for (std::size_t i = 0; i < input.size(); ++i) {
      const auto value = input[i];
      output[i] = value;
      checksum += value;
    }
  }
  return checksum;
}

bench::Control bench::control(std::uint32_t transitions, Control initial) {
  for (std::uint32_t index = 0; index < transitions; ++index) {
    const auto selected = initial.state & 7;
    initial.accumulator += selected;
    initial.accumulator ^= selected + 1;
    initial.state = (selected + 3) & 7;
    initial.accumulator ^= std::rotr(initial.state, 1);
    initial.accumulator += 3;
    initial.last_handler = selected;
  }
  return initial;
}
