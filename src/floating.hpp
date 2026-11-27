// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <llvm/IR/IRBuilder.h>

namespace sharc::detail {
enum class FloatingOp : std::uint8_t {
  multiply = 0x30,
  add = 0x81,
  subtract = 0x82,
  average = 0x89,
  compare = 0x8a,
  pack_short = 0x90,
  absolute_sum = 0x91,
  absolute_difference = 0x92,
  unpack_short = 0x94,
  pass = 0xa1,
  negate = 0xa2,
  round = 0xa5,
  mantissa = 0xad,
  absolute = 0xb0,
  scale = 0xbd,
  exponent = 0xc1,
  reciprocal_seed = 0xc4,
  inverse_sqrt_seed = 0xc5,
  fix = 0xc9,
  float_fixed = 0xca,
  truncate_fixed = 0xcd,
  fix_scaled = 0xd9,
  float_scaled = 0xda,
  truncate_scaled = 0xdd,
  copy_sign = 0xe0,
  minimum = 0xe1,
  maximum = 0xe2,
  clip = 0xe3
};

[[nodiscard]] bool floating_alu(std::uint8_t opcode);
[[nodiscard]] bool floating_arithmetic(FloatingOp operation);
[[nodiscard]] bool fixed_conversion(FloatingOp operation);

struct FloatingMode {
  bool rnd32{}, truncate{}, fast_fp32{};
  bool saturate{};
};

struct FloatingInputs {
  llvm::Value *left{}, *right{}, *status{}, *sticky{};
};

struct FloatingEffects {
  llvm::Value *raw{}, *status{}, *sticky{};
};

[[nodiscard]] FloatingEffects emit_floating(llvm::IRBuilder<> &builder,
                                            FloatingOp operation,
                                            FloatingMode mode,
                                            FloatingInputs inputs);
} // namespace sharc::detail
