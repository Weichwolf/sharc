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
  absolute_sum = 0x91,
  absolute_difference = 0x92,
  pass = 0xa1,
  negate = 0xa2,
  round = 0xa5,
  absolute = 0xb0,
  scale = 0xbd,
  float_fixed = 0xca,
  float_scaled = 0xda,
  copy_sign = 0xe0,
  minimum = 0xe1,
  maximum = 0xe2,
  clip = 0xe3
};

[[nodiscard]] bool floating_alu(std::uint8_t opcode);
[[nodiscard]] bool floating_arithmetic(FloatingOp operation);

struct FloatingMode {
  bool rnd32{}, truncate{}, fast_fp32{};
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
