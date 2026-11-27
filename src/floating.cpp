// SPDX-License-Identifier: GPL-3.0-only
#include "floating.hpp"

#include <array>
#include <cstdint>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>

namespace sharc::detail {
namespace {
constexpr unsigned exponent_shift = 31;
constexpr unsigned sign_shift = 39;
constexpr unsigned extension_bits = 8;
constexpr unsigned wide_bits = 64;
constexpr unsigned extended_precision = 32;
constexpr unsigned single_precision = 24;
constexpr unsigned exponent_limit = 255;
constexpr unsigned exponent_bias = 127;
constexpr unsigned fixed_exponent = exponent_bias + 31;
constexpr std::uint64_t implicit_bit = std::uint64_t{1} << exponent_shift;
constexpr std::uint64_t sign_mask = std::uint64_t{1} << sign_shift;
constexpr std::uint64_t fraction_mask = implicit_bit - 1;
constexpr std::uint64_t magnitude_mask = sign_mask - 1;
constexpr std::uint64_t infinity = std::uint64_t{exponent_limit} << exponent_shift;
constexpr std::uint64_t canonical_nan = sign_mask | magnitude_mask;
constexpr std::uint64_t narrow_mask = ~((std::uint64_t{1} << extension_bits) - 1);
constexpr auto fixed_max = implicit_bit - 1;
constexpr auto fixed_mask = (implicit_bit << 1) - 1;
constexpr unsigned ieee_fraction_bits = single_precision - 1;
constexpr auto ieee_hidden_bit = std::uint64_t{1} << ieee_fraction_bits;
constexpr unsigned short_fraction_bits = 11;
constexpr unsigned short_source_bias = 120;
constexpr unsigned short_minimum_source_exponent = 110;
constexpr auto short_fraction_mask = (std::uint64_t{1} << short_fraction_bits) - 1;
constexpr auto short_sign_mask = std::uint64_t{1} << 15;
constexpr auto short_max = short_sign_mask - 1;

constexpr std::uint32_t alu_flags = 0x43f;
constexpr std::uint32_t multiplier_flags = 0x3c0;
constexpr std::uint32_t shifter_flags = 0x3800;
constexpr std::uint32_t shifter_overflow = 0x800;
constexpr std::uint32_t compare_history = 0xff000000;
constexpr std::uint32_t newest_comparison = 0x80000000;

// Fraction bytes observed with the CCES ADSP-21489 simulator, indexed as in 11-41/43.
constexpr std::array<std::uint8_t, 128> reciprocal_fractions{
    0xff, 0xfc, 0xf8, 0xf4, 0xf0, 0xec, 0xe8, 0xe4, 0xe0, 0xde, 0xda, 0xd6, 0xd4, 0xd0, 0xcc, 0xca,
    0xc6, 0xc2, 0xc0, 0xbc, 0xba, 0xb6, 0xb4, 0xb2, 0xae, 0xac, 0xa8, 0xa6, 0xa4, 0xa0, 0x9e, 0x9c,
    0x98, 0x96, 0x94, 0x92, 0x8e, 0x8c, 0x8a, 0x88, 0x86, 0x82, 0x80, 0x7e, 0x7c, 0x7a, 0x78, 0x76,
    0x74, 0x72, 0x70, 0x6e, 0x6c, 0x6a, 0x68, 0x66, 0x64, 0x62, 0x60, 0x5e, 0x5c, 0x5a, 0x58, 0x56,
    0x54, 0x52, 0x50, 0x50, 0x4e, 0x4c, 0x4a, 0x48, 0x46, 0x46, 0x44, 0x42, 0x40, 0x3e, 0x3e, 0x3c,
    0x3a, 0x38, 0x38, 0x36, 0x34, 0x32, 0x32, 0x30, 0x2e, 0x2e, 0x2c, 0x2a, 0x28, 0x28, 0x26, 0x24,
    0x24, 0x22, 0x20, 0x20, 0x1e, 0x1e, 0x1c, 0x1a, 0x1a, 0x18, 0x18, 0x16, 0x14, 0x14, 0x12, 0x12,
    0x10, 0x0e, 0x0e, 0x0c, 0x0c, 0x0a, 0x0a, 0x08, 0x08, 0x06, 0x06, 0x04, 0x04, 0x02, 0x02, 0x00};
constexpr std::array<std::uint8_t, 128> inverse_sqrt_fractions{
    0x6a, 0x66, 0x64, 0x60, 0x5e, 0x5c, 0x5a, 0x56, 0x54, 0x52, 0x50, 0x4e, 0x4c, 0x4a, 0x46, 0x44,
    0x42, 0x40, 0x3e, 0x3c, 0x3c, 0x3a, 0x38, 0x36, 0x34, 0x32, 0x30, 0x2e, 0x2c, 0x2c, 0x2a, 0x28,
    0x26, 0x26, 0x24, 0x22, 0x20, 0x20, 0x1e, 0x1c, 0x1c, 0x1a, 0x18, 0x16, 0x16, 0x14, 0x14, 0x12,
    0x10, 0x10, 0x0e, 0x0e, 0x0c, 0x0a, 0x0a, 0x08, 0x08, 0x06, 0x06, 0x04, 0x04, 0x02, 0x02, 0x00,
    0xff, 0xfc, 0xf8, 0xf4, 0xf0, 0xec, 0xe8, 0xe6, 0xe2, 0xde, 0xdc, 0xd8, 0xd4, 0xd2, 0xce, 0xcc,
    0xc8, 0xc6, 0xc4, 0xc0, 0xbe, 0xbc, 0xb8, 0xb6, 0xb4, 0xb2, 0xae, 0xac, 0xaa, 0xa8, 0xa6, 0xa4,
    0xa2, 0x9e, 0x9c, 0x9a, 0x98, 0x96, 0x94, 0x92, 0x90, 0x8e, 0x8c, 0x8a, 0x8a, 0x88, 0x86, 0x84,
    0x82, 0x80, 0x7e, 0x7c, 0x7c, 0x7a, 0x78, 0x76, 0x74, 0x74, 0x72, 0x70, 0x6e, 0x6e, 0x6c, 0x6a};

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
  Number to_fixed(FloatingOp operation, const Operand &value, llvm::Value *raw_scale);
  llvm::Value *fixed_magnitude(FloatingOp operation, Shift input, llvm::Value *negative);
  Number scale(const Operand &value, llvm::Value *raw_scale);
  Number parts(FloatingOp operation, const Operand &value);
  Number seed(FloatingOp operation, const Operand &value);
  llvm::Value *seed_fraction(bool inverse_sqrt, llvm::Value *index);
  FloatingEffects pack_short(const FloatingInputs &inputs);
  FloatingEffects unpack_short(const FloatingInputs &inputs);
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

llvm::Value *Floating::fixed_magnitude(FloatingOp operation, Shift input, llvm::Value *negative) {
  const auto [significand, distance] = input;
  auto *shift = ir_.CreateBinaryIntrinsic(llvm::Intrinsic::smax, distance, word(1));
  shift = ir_.CreateBinaryIntrinsic(llvm::Intrinsic::umin, shift, word(wide_bits - 1));
  const bool fix = operation == FloatingOp::fix || operation == FloatingOp::fix_scaled;
  llvm::Value *magnitude{};
  if (fix && !mode_.truncate) {
    magnitude = round_shift({.value = significand, .distance = shift});
  } else {
    magnitude = ir_.CreateLShr(significand, shift);
    if (fix) {
      auto *discarded = ir_.CreateICmpNE(ir_.CreateShl(magnitude, shift), significand);
      auto *floor = ir_.CreateAnd(negative, discarded);
      magnitude = ir_.CreateAdd(magnitude, ir_.CreateZExt(floor, type(wide_bits)));
    }
  }
  return ir_.CreateSelect(ir_.CreateICmpSGT(distance, word(0)), magnitude, significand);
}

Number Floating::to_fixed(FloatingOp operation, const Operand &value, llvm::Value *raw_scale) {
  auto *exponent = value.exponent;
  if (operation == FloatingOp::fix_scaled || operation == FloatingOp::truncate_scaled) {
    exponent = ir_.CreateAdd(exponent, fixed_operand(raw_scale));
  }
  auto *negative = ir_.CreateICmpNE(value.sign, word(0));
  auto *distance = ir_.CreateSub(word(fixed_exponent), exponent);
  auto *magnitude =
      fixed_magnitude(operation, {.value = significand(value), .distance = distance}, negative);
  auto *limit = ir_.CreateSelect(negative, word(implicit_bit), word(fixed_max));
  auto *outside = ir_.CreateOr(ir_.CreateICmpSGT(exponent, word(fixed_exponent)),
                               ir_.CreateICmpUGT(magnitude, limit));
  auto *normal = ir_.CreateNot(ir_.CreateOr(value.zero, ir_.CreateOr(value.infinite, value.nan)));
  auto *overflow = ir_.CreateOr(value.infinite, ir_.CreateAnd(normal, outside));
  auto *underflow = ir_.CreateAnd(normal, ir_.CreateICmpSLT(exponent, word(exponent_bias)));
  magnitude = ir_.CreateSelect(overflow, limit, magnitude);
  auto *fixed = ir_.CreateSelect(negative, ir_.CreateNeg(magnitude), magnitude);
  auto *raw = ir_.CreateShl(ir_.CreateAnd(fixed, word(fixed_mask)), word(extension_bits));
  return {.raw = raw,
          .overflow = overflow,
          .underflow = underflow,
          .invalid = mode_.saturate ? value.nan : ir_.CreateOr(value.nan, overflow)};
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

Number Floating::parts(FloatingOp operation, const Operand &value) {
  if (operation == FloatingOp::mantissa) {
    return {.raw = ir_.CreateShl(significand(value), word(extension_bits)),
            .overflow = value.infinite,
            .underflow = no(),
            .invalid = value.nan};
  }
  auto *unbiased = ir_.CreateSub(value.exponent, word(exponent_bias));
  auto *encoded = ir_.CreateShl(ir_.CreateAnd(unbiased, word(fixed_mask)), word(extension_bits));
  auto *overflow = ir_.CreateOr(value.zero, value.infinite);
  auto *low = word(mode_.saturate ? sign_mask : sign_mask | infinity);
  auto *high = word(mode_.saturate ? fixed_max << extension_bits : infinity);
  auto *limit = ir_.CreateSelect(value.zero, low, high);
  return {.raw = ir_.CreateSelect(overflow, limit, encoded),
          .overflow = overflow,
          .underflow = no(),
          .invalid = value.nan};
}

llvm::Value *Floating::seed_fraction(bool inverse_sqrt, llvm::Value *index) {
  auto *module = ir_.GetInsertBlock()->getModule();
  const auto *name = inverse_sqrt ? "sharc.rsqrts.seed" : "sharc.recips.seed";
  auto *table = module->getNamedGlobal(name);
  if (table == nullptr) {
    auto *initializer = llvm::ConstantDataArray::get(
        ir_.getContext(), inverse_sqrt ? inverse_sqrt_fractions : reciprocal_fractions);
    table = new llvm::GlobalVariable(*module,
                                     initializer->getType(),
                                     true,
                                     llvm::GlobalValue::PrivateLinkage,
                                     initializer,
                                     name);
    table->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  }
  llvm::Value *values = llvm::PoisonValue::get(type(8));
  for (unsigned lane = 0; lane < 2; ++lane) {
    auto *offset = ir_.CreateExtractElement(index, lane);
    auto *entry = ir_.CreateInBoundsGEP(table->getValueType(), table, {ir_.getInt32(0), offset});
    values = ir_.CreateInsertElement(values, ir_.CreateLoad(ir_.getInt8Ty(), entry), lane);
  }
  return ir_.CreateShl(ir_.CreateZExt(values, type(wide_bits)), word(exponent_shift - 8));
}

Number Floating::seed(FloatingOp operation, const Operand &value) {
  const bool inverse_sqrt = operation == FloatingOp::inverse_sqrt_seed;
  auto *unbiased = ir_.CreateSub(value.exponent, word(exponent_bias));
  auto *scaled = inverse_sqrt ? ir_.CreateAShr(unbiased, word(1)) : unbiased;
  auto *exponent = ir_.CreateSub(word(exponent_bias - 1), scaled);
  auto *fraction = ir_.CreateAnd(value.magnitude, word(fraction_mask));
  auto *index = ir_.CreateLShr(fraction, word(exponent_shift - (inverse_sqrt ? 6 : 7)));
  llvm::Value *underflow = no();
  llvm::Value *invalid = value.nan;
  if (inverse_sqrt) {
    auto *parity = ir_.CreateAnd(value.exponent, word(1));
    index = ir_.CreateOr(index, ir_.CreateShl(parity, word(6)));
    auto *negative = ir_.CreateICmpNE(value.sign, word(0));
    invalid = ir_.CreateOr(invalid, ir_.CreateAnd(negative, ir_.CreateNot(value.zero)));
  } else {
    constexpr unsigned last_reciprocal_normal_exponent = 252;
    auto *too_large = ir_.CreateICmpUGT(value.exponent, word(last_reciprocal_normal_exponent));
    underflow = ir_.CreateAnd(too_large, ir_.CreateNot(ir_.CreateOr(value.infinite, value.nan)));
  }
  auto *raw = ir_.CreateOr(ir_.CreateShl(exponent, word(exponent_shift)),
                           seed_fraction(inverse_sqrt, index));
  raw = ir_.CreateSelect(ir_.CreateOr(underflow, value.infinite), word(0), raw);
  raw = ir_.CreateSelect(value.zero, word(infinity), raw);
  return {.raw = ir_.CreateOr(raw, value.sign),
          .overflow = value.zero,
          .underflow = underflow,
          .invalid = invalid};
}

FloatingEffects Floating::pack_short(const FloatingInputs &inputs) {
  auto *raw = mode_.fast_fp32 ? round_single(inputs.left).raw : inputs.left;
  raw = ir_.CreateLShr(raw, word(extension_bits));
  auto *exponent =
      ir_.CreateAnd(ir_.CreateLShr(raw, word(ieee_fraction_bits)), word(exponent_limit));
  auto *normal = ir_.CreateICmpUGT(exponent, word(short_source_bias));
  auto *lowest = ir_.CreateBinaryIntrinsic(
      llvm::Intrinsic::umax, exponent, word(short_minimum_source_exponent));
  auto *shift = ir_.CreateSelect(
      normal,
      word(ieee_fraction_bits - short_fraction_bits),
      ir_.CreateSub(word(short_minimum_source_exponent + ieee_fraction_bits), lowest));
  auto *mantissa =
      ir_.CreateOr(ir_.CreateAnd(raw, word(ieee_hidden_bit - 1)), word(ieee_hidden_bit));
  auto *half = ir_.CreateShl(word(1), ir_.CreateSub(shift, word(1)));
  auto *fraction = ir_.CreateLShr(ir_.CreateAdd(mantissa, half), shift);
  auto *offset = ir_.CreateShl(ir_.CreateSub(exponent, word(short_source_bias + 1)),
                               word(short_fraction_bits));
  auto *encoded = ir_.CreateAdd(fraction, ir_.CreateSelect(normal, offset, word(0)));
  auto *overflow = ir_.CreateICmpUGT(encoded, word(short_max));
  auto *tiny = ir_.CreateICmpULT(exponent, word(short_minimum_source_exponent));
  auto *magnitude =
      ir_.CreateSelect(tiny, word(0), ir_.CreateSelect(overflow, word(short_max), encoded));
  auto *sign = ir_.CreateAnd(ir_.CreateLShr(raw, word(16)), word(short_sign_mask));
  return {.raw = ir_.CreateShl(ir_.CreateOr(sign, magnitude), word(extension_bits)),
          .status = ir_.CreateOr(ir_.CreateAnd(inputs.status, constant(32, ~shifter_flags)),
                                 flag(overflow, shifter_overflow)),
          .sticky = inputs.sticky};
}

FloatingEffects Floating::unpack_short(const FloatingInputs &inputs) {
  auto *short_word = ir_.CreateLShr(inputs.left, word(extension_bits));
  auto *fraction = ir_.CreateAnd(short_word, word(short_fraction_mask));
  auto *exponent = ir_.CreateAnd(ir_.CreateLShr(short_word, word(short_fraction_bits)), word(15));
  auto *denormal = ir_.CreateICmpEQ(exponent, word(0));
  auto *leading =
      ir_.CreateIntrinsic(llvm::Intrinsic::ctlz, {type(wide_bits)}, {fraction, ir_.getFalse()});
  auto *normalized = ir_.CreateShl(fraction, ir_.CreateAnd(leading, word(wide_bits - 1)));
  auto *denormal_fraction = ir_.CreateAnd(
      ir_.CreateLShr(normalized, word(wide_bits - 1 - exponent_shift)), word(fraction_mask));
  auto *denormal_exponent =
      ir_.CreateSub(word(short_minimum_source_exponent + wide_bits - 1), leading);
  auto *biased = ir_.CreateSelect(
      denormal, denormal_exponent, ir_.CreateAdd(exponent, word(short_source_bias)));
  auto *bits =
      ir_.CreateSelect(denormal,
                       denormal_fraction,
                       ir_.CreateShl(fraction, word(exponent_shift - short_fraction_bits)));
  auto *magnitude = ir_.CreateOr(ir_.CreateShl(biased, word(exponent_shift)), bits);
  auto *zero = ir_.CreateICmpEQ(ir_.CreateAnd(short_word, word(short_max)), word(0));
  auto *sign =
      ir_.CreateShl(ir_.CreateAnd(short_word, word(short_sign_mask)), word(sign_shift - 15));
  return {.raw = ir_.CreateOr(ir_.CreateSelect(zero, word(0), magnitude), sign),
          .status = ir_.CreateAnd(inputs.status, constant(32, ~shifter_flags)),
          .sticky = inputs.sticky};
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
  const bool fixed_output = operation == FloatingOp::mantissa ||
                            operation == FloatingOp::exponent || fixed_conversion(operation);
  auto *magnitude = fixed_output ? raw : ir_.CreateAnd(raw, word(magnitude_mask));
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
  if (!absolute && operation != FloatingOp::mantissa) {
    flags = ir_.CreateOr(flags, flag(negative, 4));
  }
  if (!average) { flags = ir_.CreateOr(flags, flag(number.overflow, 2)); }
  if (operation == FloatingOp::absolute || operation == FloatingOp::mantissa) {
    auto *input_sign = ir_.CreateICmpNE(ir_.CreateAnd(inputs.left, word(sign_mask)), word(0));
    auto *input_negative = ir_.CreateAnd(input_sign, ir_.CreateNot(number.invalid));
    flags = ir_.CreateOr(flags, flag(input_negative, 16));
  }
  flags = ir_.CreateOr(flags, flag(number.invalid, 32));
  auto *sticky = ir_.CreateOr(inputs.sticky, flag(number.invalid, 32));
  if (!average) { sticky = ir_.CreateOr(sticky, flag(number.overflow, 2)); }
  if (floating_arithmetic(operation) || operation == FloatingOp::float_scaled ||
      operation == FloatingOp::scale || operation == FloatingOp::reciprocal_seed ||
      fixed_conversion(operation)) {
    sticky = ir_.CreateOr(sticky, flag(number.underflow, 1));
  }
  return {.raw = raw,
          .status = ir_.CreateOr(ir_.CreateAnd(inputs.status, constant(32, ~alu_flags)), flags),
          .sticky = sticky};
}

FloatingEffects Floating::emit(FloatingOp operation, const FloatingInputs &inputs) {
  if (operation == FloatingOp::pack_short) { return pack_short(inputs); }
  if (operation == FloatingOp::unpack_short) { return unpack_short(inputs); }
  if (operation == FloatingOp::float_fixed || operation == FloatingOp::float_scaled) {
    return effects(operation, from_fixed(operation, inputs), inputs);
  }
  if (operation == FloatingOp::round) {
    const auto number =
        round_single(mode_.rnd32 || mode_.fast_fp32 ? operand(inputs.left).raw : inputs.left);
    return effects(operation, number, inputs);
  }
  const auto left = operand(inputs.left);
  if (fixed_conversion(operation)) {
    return effects(operation, to_fixed(operation, left, inputs.right), inputs);
  }
  if (operation == FloatingOp::mantissa || operation == FloatingOp::exponent) {
    return effects(operation, parts(operation, left), inputs);
  }
  if (operation == FloatingOp::reciprocal_seed || operation == FloatingOp::inverse_sqrt_seed) {
    return effects(operation, seed(operation, left), inputs);
  }
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

bool fixed_conversion(FloatingOp operation) {
  return operation == FloatingOp::fix || operation == FloatingOp::fix_scaled ||
         operation == FloatingOp::truncate_fixed || operation == FloatingOp::truncate_scaled;
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
    case FloatingOp::mantissa:
    case FloatingOp::absolute:
    case FloatingOp::scale:
    case FloatingOp::exponent:
    case FloatingOp::reciprocal_seed:
    case FloatingOp::inverse_sqrt_seed:
    case FloatingOp::fix:
    case FloatingOp::fix_scaled:
    case FloatingOp::truncate_fixed:
    case FloatingOp::truncate_scaled:
    case FloatingOp::float_fixed:
    case FloatingOp::float_scaled:
    case FloatingOp::copy_sign:
    case FloatingOp::minimum:
    case FloatingOp::maximum:
    case FloatingOp::clip: return true;
    case FloatingOp::multiply:
    case FloatingOp::pack_short:
    case FloatingOp::unpack_short: return false;
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
