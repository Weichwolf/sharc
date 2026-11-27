// SPDX-License-Identifier: GPL-3.0-only
#include "floating.hpp"

#include <cstdint>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>

namespace sharc::detail {
namespace {
constexpr unsigned exponent_shift = 31;
constexpr unsigned sign_shift = 39;
constexpr unsigned extension_bits = 8;
constexpr unsigned wide_bits = 64;
constexpr unsigned extended_precision = 32;
constexpr unsigned single_precision = 24;
constexpr unsigned exponent_limit = 255;
constexpr std::uint64_t implicit_bit = std::uint64_t{1} << exponent_shift;
constexpr std::uint64_t sign_mask = std::uint64_t{1} << sign_shift;
constexpr std::uint64_t fraction_mask = implicit_bit - 1;
constexpr std::uint64_t magnitude_mask = sign_mask - 1;
constexpr std::uint64_t infinity = std::uint64_t{exponent_limit} << exponent_shift;
constexpr std::uint64_t canonical_nan = sign_mask | magnitude_mask;
constexpr std::uint64_t narrow_mask = ~((std::uint64_t{1} << extension_bits) - 1);

constexpr std::uint32_t alu_flags = 0x43f;
constexpr std::uint32_t multiplier_flags = 0x3c0;
constexpr std::uint32_t compare_history = 0xff000000;
constexpr std::uint32_t newest_comparison = 0x80000000;

struct Operand {
  llvm::Value *raw, *magnitude, *exponent, *sign, *zero, *infinite, *nan;
};

struct Operands {
  Operand left, right;
};

struct Shift {
  llvm::Value *value, *distance;
};

struct ScaledMagnitude {
  llvm::Value *magnitude, *exponent_at_top, *sign;
};

struct Number {
  llvm::Value *raw, *overflow, *underflow, *invalid;
};

class Floating {
public:
  Floating(llvm::IRBuilder<> &builder, FloatingMode mode) : ir_(builder), mode_(mode) {}

  FloatingEffects emit(FloatingOp operation, const FloatingInputs &inputs);

private:
  [[nodiscard]] llvm::FixedVectorType *type(unsigned bits) const;
  [[nodiscard]] llvm::Constant *constant(unsigned bits, std::uint64_t value) const;

  [[nodiscard]] llvm::Constant *word(std::uint64_t value) const {
    return constant(wide_bits, value);
  }

  [[nodiscard]] llvm::Constant *no() const { return constant(1, 0); }

  llvm::Value *flag(llvm::Value *predicate, std::uint32_t mask);
  Operand unpack(llvm::Value *raw);
  Operand operand(llvm::Value *raw);
  llvm::Value *fixed_operand(llvm::Value *raw);
  Number round_single(llvm::Value *raw);
  llvm::Value *round_shift(Shift input);
  llvm::Value *significand(const Operand &value);
  llvm::Value *shift_right_jam(Shift input);
  llvm::Value *ordered_key(const Operand &value);
  Number pack(ScaledMagnitude scaled, bool narrow);
  Number add(FloatingOp operation, const Operands &operands);
  Number multiply(const Operands &operands);
  Number from_fixed(FloatingOp operation, const FloatingInputs &inputs);
  Number scale(const Operand &value, llvm::Value *raw_scale);
  Number select(FloatingOp operation, const Operands &operands);
  FloatingEffects compare(const Operands &operands, const FloatingInputs &inputs);
  Number unary(FloatingOp operation, const Operand &value);
  FloatingEffects effects(FloatingOp operation, const Number &number, const FloatingInputs &inputs);

  llvm::IRBuilder<> &ir_;
  FloatingMode mode_;
};

llvm::FixedVectorType *Floating::type(unsigned bits) const {
  return llvm::FixedVectorType::get(ir_.getIntNTy(bits), 2);
}

llvm::Constant *Floating::constant(unsigned bits, std::uint64_t value) const {
  return llvm::ConstantVector::getSplat(type(bits)->getElementCount(), ir_.getIntN(bits, value));
}

llvm::Value *Floating::flag(llvm::Value *predicate, std::uint32_t mask) {
  return ir_.CreateSelect(predicate, constant(32, mask), constant(32, 0));
}

Operand Floating::unpack(llvm::Value *raw) {
  auto *magnitude = ir_.CreateAnd(raw, word(magnitude_mask));
  auto *exponent = ir_.CreateLShr(magnitude, word(exponent_shift));
  auto *sign = ir_.CreateAnd(raw, word(sign_mask));
  auto *zero = ir_.CreateICmpEQ(exponent, word(0));
  auto *infinite = ir_.CreateICmpEQ(magnitude, word(infinity));
  auto *nan = ir_.CreateICmpUGT(magnitude, word(infinity));
  // Inputs in the denormal range are consumed as signed zero.
  return {.raw = ir_.CreateSelect(zero, sign, raw),
          .magnitude = ir_.CreateSelect(zero, word(0), magnitude),
          .exponent = exponent,
          .sign = sign,
          .zero = zero,
          .infinite = infinite,
          .nan = nan};
}

Operand Floating::operand(llvm::Value *raw) {
  if (mode_.fast_fp32) { return unpack(round_single(raw).raw); }
  return unpack(mode_.rnd32 ? ir_.CreateAnd(raw, word(narrow_mask)) : raw);
}

llvm::Value *Floating::fixed_operand(llvm::Value *raw) {
  auto *fixed = ir_.CreateTrunc(ir_.CreateLShr(raw, word(extension_bits)), type(32));
  return ir_.CreateSExt(fixed, type(wide_bits));
}

llvm::Value *Floating::round_shift(Shift input) {
  const auto [value, shift] = input;
  auto *retained = ir_.CreateLShr(value, shift);
  if (mode_.truncate) { return retained; }
  auto *half = ir_.CreateShl(word(1), ir_.CreateSub(shift, word(1)));
  auto *discarded = ir_.CreateAnd(value, ir_.CreateSub(ir_.CreateShl(half, word(1)), word(1)));
  auto *greater = ir_.CreateICmpUGT(discarded, half);
  auto *tie = ir_.CreateICmpEQ(discarded, half);
  auto *odd = ir_.CreateICmpNE(ir_.CreateAnd(retained, word(1)), word(0));
  auto *increment = ir_.CreateOr(greater, ir_.CreateAnd(tie, odd));
  return ir_.CreateAdd(retained, ir_.CreateZExt(increment, type(wide_bits)));
}

Number Floating::round_single(llvm::Value *raw) {
  const auto original = unpack(raw);
  auto *rounded =
      ir_.CreateShl(round_shift({.value = original.magnitude, .distance = word(extension_bits)}),
                    word(extension_bits));
  auto *finite = ir_.CreateICmpULT(original.magnitude, word(infinity));
  auto *overflow = ir_.CreateAnd(finite, ir_.CreateICmpEQ(rounded, word(infinity)));
  auto *value = ir_.CreateOr(rounded, original.sign);
  // Keep the NaN class and sign when reducing FastFP32 operands.
  return {.raw = ir_.CreateSelect(original.nan, raw, value),
          .overflow = overflow,
          .underflow = no(),
          .invalid = original.nan};
}

llvm::Value *Floating::significand(const Operand &value) {
  auto *implicit =
      ir_.CreateOr(ir_.CreateAnd(value.magnitude, word(fraction_mask)), word(implicit_bit));
  return ir_.CreateSelect(value.zero, word(0), implicit);
}

llvm::Value *Floating::shift_right_jam(Shift input) {
  const auto [value, distance] = input;
  auto *shift = ir_.CreateBinaryIntrinsic(llvm::Intrinsic::umin, distance, word(wide_bits - 1));
  auto *shifted = ir_.CreateLShr(value, shift);
  auto *lost = ir_.CreateICmpNE(ir_.CreateShl(shifted, shift), value);
  return ir_.CreateOr(shifted, ir_.CreateZExt(lost, type(wide_bits)));
}

Number Floating::pack(ScaledMagnitude scaled, bool narrow) {
  const auto [magnitude, exponent_at_top, sign] = scaled;
  const auto precision = narrow ? single_precision : extended_precision;
  auto *leading =
      ir_.CreateIntrinsic(llvm::Intrinsic::ctlz, {type(wide_bits)}, {magnitude, ir_.getFalse()});
  auto *normalized = ir_.CreateShl(magnitude, ir_.CreateAnd(leading, word(wide_bits - 1)));
  auto *exponent = ir_.CreateSub(exponent_at_top, leading);
  auto *edge = ir_.CreateICmpEQ(exponent, word(0));
  // At the minimum normal boundary, quantize on the subnormal grid before flushing.
  auto *shift = ir_.CreateAdd(word(wide_bits - precision), ir_.CreateZExt(edge, type(wide_bits)));
  auto *rounded = round_shift({.value = normalized, .distance = shift});
  auto *carry = ir_.CreateICmpEQ(rounded, word(std::uint64_t{1} << precision));
  auto *promoted = ir_.CreateICmpUGE(rounded, word(std::uint64_t{1} << (precision - 1)));
  auto *adjusted = ir_.CreateAdd(
      exponent,
      ir_.CreateZExt(ir_.CreateOr(carry, ir_.CreateAnd(edge, promoted)), type(wide_bits)));
  auto *nonzero = ir_.CreateICmpNE(magnitude, word(0));
  auto *subnormal = ir_.CreateICmpSLT(adjusted, word(1));
  auto *tiny = subnormal;
  if (!mode_.truncate) {
    constexpr auto all_bits = ~std::uint64_t{0};
    const auto halfway = all_bits << (wide_bits - precision);
    tiny = ir_.CreateOr(tiny, ir_.CreateAnd(edge, ir_.CreateICmpEQ(normalized, word(halfway))));
  }
  auto *overflow = ir_.CreateAnd(nonzero, ir_.CreateICmpSGE(adjusted, word(exponent_limit)));
  auto *fraction = ir_.CreateAnd(rounded, word((std::uint64_t{1} << (precision - 1)) - 1));
  auto *raw = ir_.CreateOr(ir_.CreateShl(adjusted, word(exponent_shift)),
                           ir_.CreateShl(fraction, word(narrow ? extension_bits : 0)));
  constexpr auto maximum = infinity - 1;
  const auto finite_limit = narrow ? maximum & narrow_mask : maximum;
  auto *overflow_value = word(mode_.truncate ? finite_limit : infinity);
  raw = ir_.CreateSelect(overflow, overflow_value, raw);
  raw = ir_.CreateSelect(ir_.CreateAnd(nonzero, ir_.CreateNot(subnormal)), raw, word(0));
  return {.raw = ir_.CreateOr(raw, sign),
          .overflow = overflow,
          .underflow = ir_.CreateAnd(nonzero, tiny),
          .invalid = no()};
}

Number Floating::add(FloatingOp operation, const Operands &operands) {
  const auto &[left, right] = operands;
  const bool subtract =
      operation == FloatingOp::subtract || operation == FloatingOp::absolute_difference;
  const bool absolute =
      operation == FloatingOp::absolute_sum || operation == FloatingOp::absolute_difference;
  auto *right_sign = subtract ? ir_.CreateXor(right.sign, word(sign_mask)) : right.sign;
  auto *opposite = ir_.CreateICmpNE(left.sign, right_sign);
  auto *left_large = ir_.CreateICmpUGE(left.magnitude, right.magnitude);
  auto *larger = ir_.CreateSelect(left_large, left.exponent, right.exponent);
  auto *smaller = ir_.CreateSelect(left_large, right.exponent, left.exponent);
  auto *x = ir_.CreateShl(significand(left), word(exponent_shift));
  auto *y = ir_.CreateShl(significand(right), word(exponent_shift));
  auto *hi = ir_.CreateSelect(left_large, x, y);
  auto *lo = shift_right_jam(
      {.value = ir_.CreateSelect(left_large, y, x), .distance = ir_.CreateSub(larger, smaller)});
  auto *magnitude = ir_.CreateSelect(opposite, ir_.CreateSub(hi, lo), ir_.CreateAdd(hi, lo));
  auto *sign = ir_.CreateSelect(left_large, left.sign, right_sign);
  sign = ir_.CreateSelect(
      ir_.CreateICmpNE(magnitude, word(0)), sign, ir_.CreateAnd(left.sign, right_sign));
  if (absolute) { sign = word(0); }
  auto *at_top = operation == FloatingOp::average ? larger : ir_.CreateAdd(larger, word(1));
  auto result = pack({.magnitude = magnitude, .exponent_at_top = at_top, .sign = sign},
                     mode_.rnd32 || mode_.fast_fp32);
  // The ALU flushes an exact halfway underflow. The multiplier retains the rounded value.
  result.raw = ir_.CreateSelect(result.underflow, sign, result.raw);
  auto *infinite = ir_.CreateOr(left.infinite, right.infinite);
  auto *unlike_infinities = ir_.CreateAnd(ir_.CreateAnd(left.infinite, right.infinite), opposite);
  auto *invalid = ir_.CreateOr(unlike_infinities, ir_.CreateOr(left.nan, right.nan));
  auto *infinite_sign = absolute ? word(0) : ir_.CreateSelect(left.infinite, left.sign, right_sign);
  result.raw = ir_.CreateSelect(infinite, ir_.CreateOr(word(infinity), infinite_sign), result.raw);
  result.overflow = ir_.CreateAnd(ir_.CreateNot(ir_.CreateOr(infinite, invalid)), result.overflow);
  result.invalid = invalid;
  return result;
}

Number Floating::multiply(const Operands &operands) {
  const auto &[left, right] = operands;
  auto *sign = ir_.CreateXor(left.sign, right.sign);
  auto *magnitude = ir_.CreateMul(significand(left), significand(right));
  constexpr unsigned product_exponent_bias = 126;
  auto *at_top =
      ir_.CreateSub(ir_.CreateAdd(left.exponent, right.exponent), word(product_exponent_bias));
  auto result = pack({.magnitude = magnitude, .exponent_at_top = at_top, .sign = sign},
                     mode_.rnd32 || mode_.fast_fp32);
  auto *infinite = ir_.CreateOr(left.infinite, right.infinite);
  auto *zero = ir_.CreateOr(left.zero, right.zero);
  result.invalid = ir_.CreateOr(ir_.CreateAnd(infinite, zero), ir_.CreateOr(left.nan, right.nan));
  auto *finite = ir_.CreateNot(ir_.CreateOr(infinite, result.invalid));
  result.raw = ir_.CreateSelect(infinite, ir_.CreateOr(sign, word(infinity)), result.raw);
  result.raw =
      ir_.CreateSelect(left.nan, left.raw, ir_.CreateSelect(right.nan, right.raw, result.raw));
  result.overflow = ir_.CreateAnd(finite, result.overflow);
  result.underflow = ir_.CreateAnd(finite, result.underflow);
  return result;
}

Number Floating::from_fixed(FloatingOp operation, const FloatingInputs &inputs) {
  auto *fixed = fixed_operand(inputs.left);
  auto *negative = ir_.CreateICmpSLT(fixed, word(0));
  auto *magnitude = ir_.CreateSelect(negative, ir_.CreateNeg(fixed), fixed);
  auto *sign = ir_.CreateSelect(negative, word(sign_mask), word(0));
  constexpr unsigned exponent_bias = 127;
  llvm::Value *at_top = word(exponent_bias + wide_bits - 1);
  if (operation == FloatingOp::float_scaled) {
    at_top = ir_.CreateAdd(at_top, fixed_operand(inputs.right));
  }
  // Strict FLOAT returns an extended result at either RND32 setting.
  auto result =
      pack({.magnitude = magnitude, .exponent_at_top = at_top, .sign = sign}, mode_.fast_fp32);
  result.raw = ir_.CreateSelect(result.underflow, sign, result.raw);
  return result;
}

Number Floating::scale(const Operand &value, llvm::Value *raw_scale) {
  auto *exponent = ir_.CreateAdd(value.exponent, fixed_operand(raw_scale));
  auto *at_top = ir_.CreateAdd(exponent, word(wide_bits - 1 - exponent_shift));
  auto result =
      pack({.magnitude = significand(value), .exponent_at_top = at_top, .sign = value.sign},
           mode_.rnd32 || mode_.fast_fp32);
  auto *finite = ir_.CreateNot(ir_.CreateOr(value.infinite, value.nan));
  result.raw = ir_.CreateSelect(value.infinite, value.raw, result.raw);
  result.overflow = ir_.CreateAnd(finite, result.overflow);
  result.underflow = ir_.CreateAnd(finite, result.underflow);
  result.invalid = value.nan;
  return result;
}

llvm::Value *Floating::ordered_key(const Operand &value) {
  auto *negative = ir_.CreateICmpNE(value.sign, word(0));
  return ir_.CreateSelect(negative, ir_.CreateNot(value.magnitude), value.magnitude);
}

Number Floating::select(FloatingOp operation, const Operands &operands) {
  const auto &[left, right] = operands;
  llvm::Value *raw{};
  if (operation == FloatingOp::copy_sign) {
    raw = ir_.CreateOr(left.magnitude, right.sign);
  } else if (operation == FloatingOp::clip) {
    auto *magnitude =
        ir_.CreateBinaryIntrinsic(llvm::Intrinsic::umin, left.magnitude, right.magnitude);
    raw = ir_.CreateOr(magnitude, left.sign);
  } else {
    auto *a = ordered_key(left);
    auto *b = ordered_key(right);
    auto *choose_left =
        operation == FloatingOp::minimum ? ir_.CreateICmpSLT(a, b) : ir_.CreateICmpSGT(a, b);
    raw = ir_.CreateSelect(choose_left, left.raw, right.raw);
  }
  return {.raw = raw,
          .overflow = no(),
          .underflow = no(),
          .invalid = ir_.CreateOr(left.nan, right.nan)};
}

FloatingEffects Floating::compare(const Operands &operands, const FloatingInputs &inputs) {
  const auto &[left, right] = operands;
  auto *a = ir_.CreateSelect(left.zero, word(0), ordered_key(left));
  auto *b = ir_.CreateSelect(right.zero, word(0), ordered_key(right));
  auto *invalid = ir_.CreateOr(left.nan, right.nan);
  auto *ordered = ir_.CreateNot(invalid);
  auto *equal = ir_.CreateAnd(ordered, ir_.CreateICmpEQ(a, b));
  auto *less = ir_.CreateAnd(ordered, ir_.CreateICmpSLT(a, b));
  auto *newest = ir_.CreateNot(ir_.CreateOr(equal, less));
  auto *history =
      ir_.CreateAnd(ir_.CreateLShr(inputs.status, constant(32, 1)), constant(32, compare_history));
  auto *flags = ir_.CreateOr(constant(32, 0x400), ir_.CreateOr(flag(equal, 1), flag(less, 4)));
  flags = ir_.CreateOr(flags, ir_.CreateOr(flag(invalid, 32), flag(newest, newest_comparison)));
  return {.raw = nullptr,
          .status = ir_.CreateOr(
              ir_.CreateAnd(inputs.status, constant(32, ~(alu_flags | compare_history))),
              ir_.CreateOr(flags, history)),
          .sticky = ir_.CreateOr(inputs.sticky, flag(invalid, 32))};
}

Number Floating::unary(FloatingOp operation, const Operand &value) {
  auto *raw = value.raw;
  if (operation == FloatingOp::absolute) { raw = value.magnitude; }
  if (operation == FloatingOp::negate) { raw = ir_.CreateXor(raw, word(sign_mask)); }
  return {.raw = raw, .overflow = no(), .underflow = no(), .invalid = value.nan};
}

FloatingEffects
Floating::effects(FloatingOp operation, const Number &number, const FloatingInputs &inputs) {
  const bool narrow = mode_.rnd32 || mode_.fast_fp32 || operation == FloatingOp::round;
  const auto nan = narrow ? canonical_nan & narrow_mask : canonical_nan;
  auto *sign = ir_.CreateAnd(number.raw, word(sign_mask));
  llvm::Value *invalid = word(nan);
  if (operation == FloatingOp::multiply) {
    invalid = ir_.CreateOr(word(nan & magnitude_mask), sign);
  }
  auto *raw = ir_.CreateSelect(number.invalid, invalid, number.raw);
  auto *magnitude = ir_.CreateAnd(raw, word(magnitude_mask));
  auto *zero = ir_.CreateICmpEQ(magnitude, word(0));
  auto *negative = ir_.CreateAnd(ir_.CreateICmpNE(sign, word(0)), ir_.CreateNot(number.invalid));
  if (operation == FloatingOp::multiply) {
    auto *flags = ir_.CreateOr(flag(negative, 0x40), flag(number.overflow, 0x80));
    flags = ir_.CreateOr(flags,
                         ir_.CreateOr(flag(number.underflow, 0x100), flag(number.invalid, 0x200)));
    auto *sticky = ir_.CreateOr(flag(number.overflow, 0x80), flag(number.underflow, 0x100));
    sticky = ir_.CreateOr(sticky, flag(number.invalid, 0x200));
    return {.raw = raw,
            .status =
                ir_.CreateOr(ir_.CreateAnd(inputs.status, constant(32, ~multiplier_flags)), flags),
            .sticky = ir_.CreateOr(inputs.sticky, sticky)};
  }
  const bool average = operation == FloatingOp::average;
  const bool absolute = operation == FloatingOp::absolute ||
                        operation == FloatingOp::absolute_sum ||
                        operation == FloatingOp::absolute_difference;
  auto *flags = ir_.CreateOr(constant(32, 0x400), flag(zero, 1));
  if (!absolute) { flags = ir_.CreateOr(flags, flag(negative, 4)); }
  if (!average) { flags = ir_.CreateOr(flags, flag(number.overflow, 2)); }
  if (operation == FloatingOp::absolute) {
    auto *input_sign = ir_.CreateICmpNE(ir_.CreateAnd(inputs.left, word(sign_mask)), word(0));
    auto *input_negative = ir_.CreateAnd(input_sign, ir_.CreateNot(number.invalid));
    flags = ir_.CreateOr(flags, flag(input_negative, 16));
  }
  flags = ir_.CreateOr(flags, flag(number.invalid, 32));
  auto *sticky = ir_.CreateOr(inputs.sticky, flag(number.invalid, 32));
  if (!average) { sticky = ir_.CreateOr(sticky, flag(number.overflow, 2)); }
  if (floating_arithmetic(operation) || operation == FloatingOp::float_scaled ||
      operation == FloatingOp::scale) {
    sticky = ir_.CreateOr(sticky, flag(number.underflow, 1));
  }
  return {.raw = raw,
          .status = ir_.CreateOr(ir_.CreateAnd(inputs.status, constant(32, ~alu_flags)), flags),
          .sticky = sticky};
}

FloatingEffects Floating::emit(FloatingOp operation, const FloatingInputs &inputs) {
  if (operation == FloatingOp::float_fixed || operation == FloatingOp::float_scaled) {
    return effects(operation, from_fixed(operation, inputs), inputs);
  }
  if (operation == FloatingOp::round) {
    const auto number =
        round_single(mode_.rnd32 || mode_.fast_fp32 ? operand(inputs.left).raw : inputs.left);
    return effects(operation, number, inputs);
  }
  const auto left = operand(inputs.left);
  if (operation == FloatingOp::scale) {
    return effects(operation, scale(left, inputs.right), inputs);
  }
  if (operation == FloatingOp::pass || operation == FloatingOp::negate ||
      operation == FloatingOp::absolute) {
    return effects(operation, unary(operation, left), inputs);
  }
  const Operands operands{.left = left, .right = operand(inputs.right)};
  if (operation == FloatingOp::compare) { return compare(operands, inputs); }
  if (operation == FloatingOp::multiply) { return effects(operation, multiply(operands), inputs); }
  if (floating_arithmetic(operation)) {
    return effects(operation, add(operation, operands), inputs);
  }
  return effects(operation, select(operation, operands), inputs);
}
} // namespace

bool floating_arithmetic(FloatingOp operation) {
  return operation == FloatingOp::add || operation == FloatingOp::subtract ||
         operation == FloatingOp::average || operation == FloatingOp::absolute_sum ||
         operation == FloatingOp::absolute_difference;
}

bool floating_alu(std::uint8_t opcode) {
  switch (static_cast<FloatingOp>(opcode)) {
    case FloatingOp::add:
    case FloatingOp::subtract:
    case FloatingOp::average:
    case FloatingOp::compare:
    case FloatingOp::absolute_sum:
    case FloatingOp::absolute_difference:
    case FloatingOp::pass:
    case FloatingOp::negate:
    case FloatingOp::round:
    case FloatingOp::absolute:
    case FloatingOp::scale:
    case FloatingOp::float_fixed:
    case FloatingOp::float_scaled:
    case FloatingOp::copy_sign:
    case FloatingOp::minimum:
    case FloatingOp::maximum:
    case FloatingOp::clip: return true;
    case FloatingOp::multiply: return false;
  }
  return false;
}

FloatingEffects emit_floating(llvm::IRBuilder<> &builder,
                              FloatingOp operation,
                              FloatingMode mode,
                              FloatingInputs inputs) {
  return Floating(builder, mode).emit(operation, inputs);
}
} // namespace sharc::detail
