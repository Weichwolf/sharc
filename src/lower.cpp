// SPDX-License-Identifier: GPL-3.0-only
#include "decode.hpp"
#include "native.hpp"
#include "sharc/core.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TypeSize.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sharc::detail {
namespace {
constexpr std::uint32_t alu_flags = 0x43f;
constexpr std::uint32_t compare_history = 0xff000000;
constexpr unsigned common_weight = 2000;

namespace alu {
constexpr unsigned add = 0x01, subtract = 0x02, add_carry = 0x05, subtract_carry = 0x06;
constexpr unsigned average = 0x09, compare = 0x0a, compare_unsigned = 0x0b;
constexpr unsigned pass = 0x21, negate = 0x22, add_carry_only = 0x25, subtract_borrow = 0x26;
constexpr unsigned increment = 0x29, decrement = 0x2a, absolute = 0x30;
constexpr unsigned bit_and = 0x40, bit_or = 0x41, bit_xor = 0x42, bit_not = 0x43;
constexpr unsigned minimum = 0x61, maximum = 0x62, clip = 0x63, dual_add_subtract = 0x70;
} // namespace alu

constexpr unsigned immediate_move_type = 17;
constexpr unsigned nop_type = 21;
constexpr unsigned direct_branch_type = 8;
constexpr unsigned return_branch_type = 11;
constexpr std::uint64_t relative_branch = std::uint64_t{1} << 40;
constexpr std::uint64_t call_branch = std::uint64_t{1} << 39;
constexpr std::uint64_t abort_branch = std::uint64_t{1} << 38;
constexpr std::uint64_t delayed_branch = std::uint64_t{1} << 26;
constexpr std::uint64_t clear_interrupt_branch = std::uint64_t{1} << 24;
// Programming Reference 2.4, 4-19: three pipeline bubbles on a taken jump.
constexpr unsigned branch_penalty = 3;
constexpr std::uint32_t true_condition = 31;
constexpr unsigned condition_shift = 33;
constexpr unsigned immediate_register_shift = 32;
constexpr unsigned raw_extension_bits = 8;
constexpr std::int64_t fixed_minimum = std::numeric_limits<std::int32_t>::min();
constexpr std::int64_t fixed_maximum = std::numeric_limits<std::int32_t>::max();
constexpr auto unsigned_limit = std::uint64_t{1} << 32;
constexpr std::array fixed_operations{
    alu::add,       alu::subtract,       alu::add_carry,        alu::subtract_carry,
    alu::average,   alu::compare,        alu::compare_unsigned, alu::pass,
    alu::negate,    alu::add_carry_only, alu::subtract_borrow,  alu::increment,
    alu::decrement, alu::absolute,       alu::bit_and,          alu::bit_or,
    alu::bit_xor,   alu::bit_not,        alu::minimum,          alu::maximum,
    alu::clip};

[[noreturn]] void reject(const Instruction &instruction,
                         std::string_view identity,
                         FaultReason reason,
                         const std::string &message) {
  char suffix = 'c';
  if (instruction.bits == full_instruction_bits) { suffix = 'a'; }
  if (instruction.bits == wide_instruction_bits) { suffix = 'b'; }
  refuse(reason,
         identity,
         instruction.variant,
         instruction.address,
         "type " + std::to_string(instruction.type) + suffix + ": " + message,
         instruction.word,
         instruction.bits);
}

bool supported_compute(const Compute &compute) {
  return compute.kind == ComputeKind::none ||
         (compute.kind == ComputeKind::alu &&
          std::ranges::find(fixed_operations, compute.opcode) != fixed_operations.end());
}

void validate_condition(const Instruction &instruction, std::string_view identity) {
  const auto cond = (instruction.word >> condition_shift) & true_condition;
  const auto base = cond & 15;
  if (cond != true_condition && (base == 15 || (base >= 9 && base <= 12))) {
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "external or loop condition is not lowered");
  }
}

void validate_branch(const Instruction &instruction, std::string_view identity) {
  constexpr auto modifiers = call_branch | abort_branch | delayed_branch | clear_interrupt_branch;
  constexpr auto allowed = (std::uint64_t{7} << 40) |
                           (std::uint64_t{true_condition} << condition_shift) | modifiers |
                           maximum_pc;
  if ((instruction.word & ~allowed) != 0) {
    reject(instruction, identity, FaultReason::invalid_encoding, "reserved direct-branch bits");
  }
  if ((instruction.word & (modifiers & ~call_branch)) != 0) {
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "delayed, loop-abort or interrupt-clear branch is not lowered");
  }
  validate_condition(instruction, identity);
}

std::uint32_t branch_target(const Instruction &instruction) {
  const auto base = (instruction.word & relative_branch) != 0 ? instruction.address : 0;
  return (base + static_cast<std::uint32_t>(instruction.word)) & maximum_pc;
}

void validate_return(const Instruction &instruction, std::string_view identity) {
  constexpr unsigned opcode_shift = 40;
  constexpr auto prefix = std::uint64_t{0x0b} << opcode_shift;
  constexpr auto modifiers = std::uint64_t{7} << 24;
  constexpr auto allowed =
      prefix | (std::uint64_t{true_condition} << condition_shift) | modifiers | 0x7fffff;
  if ((instruction.word & ~allowed) != 0) {
    reject(instruction, identity, FaultReason::invalid_encoding, "reserved return bits");
  }
  if ((instruction.word >> opcode_shift) != 0x0a || (instruction.word & modifiers) != 0 ||
      instruction.compute.kind != ComputeKind::none) {
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "RTI, modified return or parallel return compute is not lowered");
  }
  validate_condition(instruction, identity);
}

void validate(const Instruction &instruction, std::string_view identity) {
  if (instruction.instruction_set != InstructionSet::isa) {
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "VISA native pipeline lowering is not implemented");
  }
  if (instruction.type == nop_type && instruction.word == 0) { return; }
  if (instruction.type == direct_branch_type) {
    validate_branch(instruction, identity);
    return;
  }
  if (instruction.type == return_branch_type) {
    validate_return(instruction, identity);
    return;
  }
  if (instruction.type == immediate_move_type) {
    constexpr std::uint64_t reserved_immediate_bit = std::uint64_t{1} << 39;
    if ((instruction.word & reserved_immediate_bit) != 0) {
      reject(
          instruction, identity, FaultReason::invalid_encoding, "reserved ISA immediate-move bit");
    }
    const auto reg = (instruction.word >> immediate_register_shift) & 0x7f;
    if (reg < 16) { return; }
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "this universal-register immediate move is not lowered");
  }
  if (instruction.type == 2) {
    constexpr auto allowed =
        (std::uint64_t{1} << 40) | (std::uint64_t{true_condition} << condition_shift) | 0x7fffff;
    if ((instruction.word & ~allowed) != 0) {
      reject(instruction,
             identity,
             FaultReason::invalid_encoding,
             "reserved conditional-compute bits");
    }
    validate_condition(instruction, identity);
    const auto &compute = instruction.compute;
    if (supported_compute(compute)) { return; }
    if (compute.kind == ComputeKind::dual_alu && compute.opcode == alu::dual_add_subtract) {
      if (compute.outputs[0] == compute.outputs[1]) {
        reject(instruction,
               identity,
               FaultReason::unsupported_instruction,
               "duplicate dual-ALU output priority is unqualified");
      }
      return;
    }
  }
  reject(instruction,
         identity,
         FaultReason::unsupported_instruction,
         "instruction family or compute operation has no LLVM lowering");
}

constexpr std::size_t status_value = 16;
constexpr std::size_t sticky_value = status_value + 1;
constexpr std::size_t instruction_remaining_value = sticky_value + 1;
constexpr std::size_t cycle_remaining_value = instruction_remaining_value + 1;
constexpr std::size_t instructions_retired_value = cycle_remaining_value + 1;
constexpr std::size_t cycles_retired_value = instructions_retired_value + 1;
constexpr std::size_t credit_value = cycles_retired_value + 1;
constexpr std::size_t hazard_value = credit_value + 1;
constexpr std::size_t pc_stack_value = hazard_value + 1;
constexpr std::size_t pc_stack_depth_value = pc_stack_value + 1;
constexpr std::size_t interrupt_latch_value = pc_stack_depth_value + 1;
constexpr std::size_t state_value_count = interrupt_latch_value + 1;
using Values = std::array<llvm::Value *, state_value_count>;

struct Node {
  llvm::BasicBlock *block{};
  std::array<llvm::PHINode *, state_value_count> phi{};

  [[nodiscard]] Values values() const {
    Values result{};
    std::ranges::copy(phi, result.begin());
    return result;
  }

  void incoming(const Values &values, llvm::BasicBlock *predecessor) const {
    for (std::size_t i = 0; i < values.size(); ++i) { phi[i]->addIncoming(values[i], predecessor); }
  }
};

struct Arithmetic {
  llvm::Value *raw{}, *status{}, *sticky{};
};

struct FixedResult {
  llvm::Value *value{}, *carry{}, *overflow{}, *sign{};
};

struct BlockCost {
  std::uint64_t instructions{1}, cycles{1};
};

class Lowering {
public:
  Lowering(llvm::Module &module,
           const CodeVariant &variant,
           std::vector<Instruction> instructions,
           const NativeUnit &unit)
      : module_(module),
        variant_(variant),
        instructions_(std::move(instructions)),
        entry_address_(unit.entry),
        context_(unit.context),
        builder_(module.getContext()) {
    auto *pointer = builder_.getPtrTy();
    function_ = llvm::Function::Create(
        llvm::FunctionType::get(builder_.getVoidTy(), {pointer, pointer}, false),
        llvm::GlobalValue::ExternalLinkage,
        unit.symbol,
        module_);
    function_->addFnAttr(llvm::Attribute::NoUnwind);
    for (auto &argument : function_->args()) {
      argument.addAttr(llvm::Attribute::NoAlias);
      argument.addAttr(llvm::Attribute::NonNull);
      argument.addAttr(llvm::Attribute::NoCapture);
    }
    function_->getArg(0)->addAttr(llvm::Attribute::ReadOnly);
    function_->getArg(0)->setName("register_pointers");
    function_->getArg(1)->setName("control_words");
  }

  void emit();

private:
  llvm::Type *vector_type(unsigned bits) const {
    return llvm::FixedVectorType::get(llvm::IntegerType::get(module_.getContext(), bits), 2);
  }

  llvm::Type *stack_type() const {
    return llvm::FixedVectorType::get(llvm::Type::getInt32Ty(module_.getContext()),
                                      Registers::pc_stack_capacity);
  }

  llvm::Constant *vector(unsigned bits, std::uint64_t value) const {
    return llvm::ConstantVector::getSplat(
        llvm::ElementCount::getFixed(2),
        llvm::ConstantInt::get(llvm::IntegerType::get(module_.getContext(), bits), value));
  }

  llvm::Value *pointer(std::size_t slot) {
    auto *address =
        builder_.CreateGEP(builder_.getPtrTy(), function_->getArg(0), builder_.getInt64(slot));
    return builder_.CreateLoad(builder_.getPtrTy(), address);
  }

  llvm::Value *load(unsigned bits, std::size_t slot) {
    return builder_.CreateLoad(llvm::IntegerType::get(module_.getContext(), bits), pointer(slot));
  }

  void store(llvm::Value *value, std::size_t slot) { builder_.CreateStore(value, pointer(slot)); }

  std::size_t data_slot(unsigned lane, unsigned reg) const {
    const auto bank_bit = reg < 8 ? low_bank_mode : high_bank_mode;
    const auto bank = (context_ & bank_bit) != 0 ? 1U : 0U;
    return (lane * 32) + (bank * 16) + reg;
  }

  llvm::Value *pair(llvm::Value *x, llvm::Value *y, unsigned bits) {
    auto *partial = builder_.CreateInsertElement(
        llvm::PoisonValue::get(vector_type(bits)), x, builder_.getInt32(0));
    return builder_.CreateInsertElement(partial, y, builder_.getInt32(1));
  }

  llvm::Value *flag(llvm::Value *predicate, std::uint32_t mask) {
    return builder_.CreateSelect(predicate, vector(32, mask), vector(32, 0));
  }

  llvm::Value *bit(llvm::Value *status, std::uint32_t mask) {
    return builder_.CreateICmpNE(builder_.CreateAnd(status, vector(32, mask)), vector(32, 0));
  }

  Node node(const std::string &name) {
    Node result;
    result.block = llvm::BasicBlock::Create(module_.getContext(), name, function_);
    llvm::IRBuilder<> start(result.block);
    for (std::size_t i = 0; i < state_value_count; ++i) {
      llvm::Type *type = builder_.getInt64Ty();
      if (i < instruction_remaining_value) { type = vector_type(32); }
      if (i < status_value) { type = vector_type(64); }
      if (i == hazard_value) { type = builder_.getInt1Ty(); }
      if (i == pc_stack_value) { type = stack_type(); }
      if (i == pc_stack_depth_value || i == interrupt_latch_value) { type = builder_.getInt32Ty(); }
      result.phi[i] = start.CreatePHI(type, 2);
    }
    return result;
  }

  void jump(const Node &target, const Values &values) {
    target.incoming(values, builder_.GetInsertBlock());
    builder_.CreateBr(target.block);
  }

  void exit(const Values &values, llvm::Value *pc, NativeExit reason) {
    auto *predecessor = builder_.GetInsertBlock();
    exit_.incoming(values, predecessor);
    exit_pc_->addIncoming(pc, predecessor);
    exit_reason_->addIncoming(builder_.getInt64(static_cast<std::uint64_t>(reason)), predecessor);
    builder_.CreateBr(exit_.block);
  }

  llvm::Value *condition(const Values &values, unsigned code);
  Arithmetic arithmetic(const Values &values, const Compute &compute, unsigned opcode);
  Arithmetic status_result(const Values &values,
                           const FixedResult &result,
                           llvm::Value *x,
                           llvm::Value *y,
                           unsigned opcode);
  void instruction(Values &values, const Instruction &op);
  void finish(Values values, std::uint32_t next);
  void credit(Values &values, llvm::Value *count, llvm::Value *cycles, bool partial);
  void complete(Values values, const Instruction &op, bool partial);
  void stack_guard(const Values &values, const Instruction &op, llvm::Value *allowed, bool partial);
  void stack_status(Values &values);
  void call(Values &values, const Instruction &op, bool partial);
  llvm::Value *pop(Values &values, const Instruction &op, bool partial);
  void return_to_caller(Values values, llvm::Value *target);
  std::vector<BlockCost> block_costs() const;
  void emit_exit();

  llvm::Module &module_;
  const CodeVariant &variant_;
  std::vector<Instruction> instructions_;
  std::uint32_t entry_address_;
  std::uint32_t context_;
  llvm::IRBuilder<> builder_;
  llvm::Function *function_{};
  llvm::Value *initial_clock_{};
  llvm::Value *initial_fetch_{};
  llvm::Value *initial_decode_{};
  std::map<std::uint32_t, std::size_t> addresses_;
  std::vector<Node> admission_, fast_, partial_;
  std::set<unsigned> written_;
  bool flags_written_{};
  bool stack_written_{};
  Node exit_;
  llvm::PHINode *exit_pc_{}, *exit_reason_{};
};

llvm::Value *Lowering::condition(const Values &values, unsigned code) {
  llvm::Value *result = vector(1, 1);
  auto *status = values[status_value];
  if (code != true_condition) {
    const auto base = code & 15U;
    auto *zero = bit(status, 1);
    auto *negative = bit(status, 4);
    auto *floating = bit(status, 1U << 10);
    auto *fixed_negative = negative;
    if ((context_ & saturation_mode) == 0) {
      fixed_negative = builder_.CreateXor(negative, bit(status, 2));
    }
    if (base == 0) {
      result = zero;
    } else if (base <= 2) {
      auto *floating_negative =
          base == 1 ? builder_.CreateAnd(negative, builder_.CreateNot(zero)) : negative;
      result = builder_.CreateSelect(floating, floating_negative, fixed_negative);
      if (base == 2) { result = builder_.CreateOr(result, zero); }
    } else {
      constexpr std::array<std::uint32_t, 16> masks{
          0, 0, 0, 8, 2, 128, 64, 2048, 4096, 0, 0, 0, 0, 1U << 18, 1U << 14, 0};
      result = bit(status, masks[base]);
    }
    if ((code & 16U) != 0) { result = builder_.CreateNot(result); }
  }
  if ((context_ & simd_mode) == 0) {
    result = builder_.CreateInsertElement(result, builder_.getFalse(), builder_.getInt32(1));
  }
  return result;
}

Arithmetic Lowering::arithmetic(const Values &values, const Compute &compute, unsigned opcode) {
  auto *x = builder_.CreateTrunc(
      builder_.CreateLShr(values[compute.inputs[0]], vector(64, raw_extension_bits)),
      vector_type(32));
  auto *y = builder_.CreateTrunc(
      builder_.CreateLShr(values[compute.inputs[1]], vector(64, raw_extension_bits)),
      vector_type(32));
  auto *sx = builder_.CreateSExt(x, vector_type(64));
  auto *sy = builder_.CreateSExt(y, vector_type(64));
  auto *ux = builder_.CreateZExt(x, vector_type(64));
  auto *uy = builder_.CreateZExt(y, vector_type(64));
  auto *old_status = values[status_value];
  auto *carry_in = builder_.CreateZExt(bit(old_status, 8), vector_type(64));
  llvm::Value *value = sx;
  llvm::Value *carry = vector(1, 0);
  llvm::Value *overflow = vector(1, 0);
  llvm::Value *sign = vector(1, 0);
  bool range_check{};
  switch (opcode) {
    case alu::add:
    case alu::add_carry: {
      auto *extra = opcode == alu::add_carry ? carry_in : vector(64, 0);
      value = builder_.CreateAdd(builder_.CreateAdd(sx, sy), extra);
      carry = builder_.CreateICmpUGE(builder_.CreateAdd(builder_.CreateAdd(ux, uy), extra),
                                     vector(64, unsigned_limit));
      range_check = true;
      break;
    }
    case alu::subtract:
    case alu::subtract_carry: {
      auto *borrow = opcode == alu::subtract_carry ? builder_.CreateSub(vector(64, 1), carry_in)
                                                   : vector(64, 0);
      value = builder_.CreateSub(builder_.CreateSub(sx, sy), borrow);
      carry = builder_.CreateICmpSGE(builder_.CreateSub(builder_.CreateSub(ux, uy), borrow),
                                     vector(64, 0));
      range_check = true;
      break;
    }
    case alu::add_carry_only:
    case alu::subtract_borrow:
    case alu::increment:
    case alu::decrement: {
      llvm::Value *extra = carry_in;
      if (opcode == alu::subtract_borrow) { extra = builder_.CreateSub(carry_in, vector(64, 1)); }
      if (opcode == alu::increment) { extra = vector(64, 1); }
      if (opcode == alu::decrement) {
        extra = vector(64, std::numeric_limits<std::uint64_t>::max());
      }
      value = builder_.CreateAdd(sx, extra);
      auto *unsigned_value = builder_.CreateAdd(ux, extra);
      carry = opcode == alu::subtract_borrow || opcode == alu::decrement
                  ? builder_.CreateICmpSGE(unsigned_value, vector(64, 0))
                  : builder_.CreateICmpUGE(unsigned_value, vector(64, unsigned_limit));
      range_check = true;
      break;
    }
    case alu::negate:
      value = builder_.CreateNeg(sx);
      carry = builder_.CreateICmpEQ(x, vector(32, 0));
      range_check = true;
      break;
    case alu::absolute:
      sign = builder_.CreateICmpSLT(x, vector(32, 0));
      value = builder_.CreateSelect(sign, builder_.CreateNeg(sx), sx);
      range_check = true;
      break;
    case alu::average: {
      auto *sum = builder_.CreateAdd(sx, sy);
      value = builder_.CreateAShr(sum, vector(64, 1));
      auto *odd = builder_.CreateICmpNE(builder_.CreateAnd(sum, vector(64, 1)), vector(64, 0));
      auto *increment =
          (context_ & truncation_mode) != 0
              ? builder_.CreateICmpSLT(sum, vector(64, 0))
              : builder_.CreateICmpNE(builder_.CreateAnd(value, vector(64, 1)), vector(64, 0));
      value = builder_.CreateAdd(
          value, builder_.CreateZExt(builder_.CreateAnd(odd, increment), vector_type(64)));
      carry = builder_.CreateICmpUGE(builder_.CreateAdd(ux, uy), vector(64, unsigned_limit));
      break;
    }
    case alu::bit_and: value = builder_.CreateAnd(sx, sy); break;
    case alu::bit_or: value = builder_.CreateOr(sx, sy); break;
    case alu::bit_xor: value = builder_.CreateXor(sx, sy); break;
    case alu::bit_not: value = builder_.CreateNot(sx); break;
    case alu::minimum:
    case alu::maximum:
      value = builder_.CreateSelect(opcode == alu::minimum ? builder_.CreateICmpSLT(sx, sy)
                                                           : builder_.CreateICmpSGT(sx, sy),
                                    sx,
                                    sy);
      break;
    case alu::clip: {
      auto *negative = builder_.CreateICmpSLT(sx, vector(64, 0));
      auto *a = builder_.CreateSelect(negative, builder_.CreateNeg(sx), sx);
      auto *b = builder_.CreateSelect(
          builder_.CreateICmpSLT(sy, vector(64, 0)), builder_.CreateNeg(sy), sy);
      auto *magnitude = builder_.CreateSelect(builder_.CreateICmpULT(a, b), a, b);
      value = builder_.CreateSelect(negative, builder_.CreateNeg(magnitude), magnitude);
      break;
    }
    default: break;
  }
  if (range_check) {
    overflow = builder_.CreateOr(
        builder_.CreateICmpSLT(value, vector(64, static_cast<std::uint64_t>(fixed_minimum))),
        builder_.CreateICmpSGT(value, vector(64, fixed_maximum)));
    if ((context_ & saturation_mode) != 0) {
      auto *limit = builder_.CreateSelect(builder_.CreateICmpSLT(value, vector(64, 0)),
                                          vector(64, static_cast<std::uint64_t>(fixed_minimum)),
                                          vector(64, fixed_maximum));
      value = builder_.CreateSelect(overflow, limit, value);
    }
  }
  return status_result(
      values, {.value = value, .carry = carry, .overflow = overflow, .sign = sign}, x, y, opcode);
}

Arithmetic Lowering::status_result(const Values &values,
                                   const FixedResult &result,
                                   llvm::Value *x,
                                   llvm::Value *y,
                                   unsigned opcode) {
  const auto &[value, carry, overflow, sign] = result;
  auto *old_status = values[status_value];
  auto *word = builder_.CreateTrunc(value, vector_type(32));
  auto *zero = builder_.CreateICmpEQ(word, vector(32, 0));
  auto *negative = builder_.CreateICmpSLT(word, vector(32, 0));
  auto *status = builder_.CreateAnd(old_status, vector(32, ~alu_flags));
  llvm::Value *raw = builder_.CreateShl(builder_.CreateZExt(word, vector_type(64)),
                                        vector(64, raw_extension_bits));
  if (opcode == alu::compare || opcode == alu::compare_unsigned) {
    zero = builder_.CreateICmpEQ(x, y);
    negative = opcode == alu::compare_unsigned ? builder_.CreateICmpULT(x, y)
                                               : builder_.CreateICmpSLT(x, y);
    auto *greater = opcode == alu::compare_unsigned ? builder_.CreateICmpUGT(x, y)
                                                    : builder_.CreateICmpSGT(x, y);
    auto *history = builder_.CreateAnd(builder_.CreateLShr(old_status, vector(32, 1)),
                                       vector(32, compare_history));
    status = builder_.CreateOr(
        builder_.CreateAnd(status, vector(32, ~compare_history)),
        builder_.CreateOr(
            history,
            flag(greater, std::uint32_t{1} << (std::numeric_limits<std::uint32_t>::digits - 1))));
    raw = nullptr;
  }
  status = builder_.CreateOr(status, builder_.CreateOr(flag(zero, 1), flag(negative, 4)));
  status = builder_.CreateOr(status, builder_.CreateOr(flag(carry, 8), flag(overflow, 2)));
  status = builder_.CreateOr(status, flag(sign, 16));
  auto *sticky = opcode == alu::absolute
                     ? values[sticky_value]
                     : builder_.CreateOr(values[sticky_value], flag(overflow, 4));
  return {.raw = raw, .status = status, .sticky = sticky};
}

void Lowering::instruction(Values &values, const Instruction &op) {
  if (op.type != direct_branch_type && op.type != return_branch_type) {
    values[hazard_value] = builder_.getInt1(op.compute.kind != ComputeKind::none);
  }
  if (op.type == immediate_move_type) {
    const auto reg = static_cast<unsigned>((op.word >> immediate_register_shift) & 15);
    auto *raw = vector(
        64, static_cast<std::uint64_t>(static_cast<std::uint32_t>(op.word)) << raw_extension_bits);
    values[reg] = builder_.CreateSelect(condition(values, true_condition), raw, values[reg]);
    written_.insert(reg);
    return;
  }
  const auto &compute = op.compute;
  if (compute.kind == ComputeKind::none) { return; }
  auto *selected =
      condition(values, static_cast<unsigned>((op.word >> condition_shift) & true_condition));
  Arithmetic result;
  if (compute.kind == ComputeKind::dual_alu) {
    result = arithmetic(values, compute, alu::add);
    const auto difference = arithmetic(values, compute, alu::subtract);
    result.status = builder_.CreateOr(result.status, difference.status);
    result.sticky = builder_.CreateOr(result.sticky, difference.sticky);
    const auto reg = compute.outputs[1];
    values[reg] = builder_.CreateSelect(selected, difference.raw, values[reg]);
    written_.insert(reg);
  } else {
    result = arithmetic(values, compute, compute.opcode);
  }
  if (result.raw != nullptr) {
    const auto reg = compute.outputs[0];
    values[reg] = builder_.CreateSelect(selected, result.raw, values[reg]);
    written_.insert(reg);
  }
  values[status_value] = builder_.CreateSelect(selected, result.status, values[status_value]);
  values[sticky_value] = builder_.CreateSelect(selected, result.sticky, values[sticky_value]);
  flags_written_ = true;
}

void Lowering::stack_guard(const Values &values,
                           const Instruction &op,
                           llvm::Value *allowed,
                           bool partial) {
  auto *admitted = llvm::BasicBlock::Create(module_.getContext(), "stack.admitted", function_);
  auto *refused = llvm::BasicBlock::Create(module_.getContext(), "cold.stack", function_);
  builder_.CreateCondBr(
      allowed,
      admitted,
      refused,
      llvm::MDBuilder(module_.getContext()).createBranchWeights(common_weight, 1));
  builder_.SetInsertPoint(refused);
  auto prefix = values;
  if (!partial) {
    auto *completed = builder_.CreateSub(values[credit_value], builder_.getInt64(1));
    credit(prefix, completed, completed, false);
  }
  exit(prefix, builder_.getInt32(op.address), NativeExit::unsupported_stack);
  builder_.SetInsertPoint(admitted);
}

void Lowering::stack_status(Values &values) {
  constexpr std::uint32_t full = 1U << 21;
  constexpr std::uint32_t empty = 1U << 22;
  auto *depth = values[pc_stack_depth_value];
  auto *status = builder_.CreateExtractElement(values[sticky_value], builder_.getInt32(0));
  status = builder_.CreateAnd(status, builder_.getInt32(~(full | empty)));
  status =
      builder_.CreateOr(status,
                        builder_.CreateSelect(builder_.CreateICmpEQ(depth, builder_.getInt32(0)),
                                              builder_.getInt32(empty),
                                              builder_.getInt32(0)));
  status = builder_.CreateOr(
      status,
      builder_.CreateSelect(
          builder_.CreateICmpEQ(depth, builder_.getInt32(Registers::pc_stack_capacity)),
          builder_.getInt32(full),
          builder_.getInt32(0)));
  values[sticky_value] =
      builder_.CreateInsertElement(values[sticky_value], status, builder_.getInt32(0));
  flags_written_ = true;
}

void Lowering::call(Values &values, const Instruction &op, bool partial) {
  constexpr std::uint32_t call_tag = 1U << 24;
  constexpr unsigned almost_full = Registers::pc_stack_capacity - 1;
  constexpr std::uint32_t stack_overflow_interrupt = 1U << 3;
  auto *depth = values[pc_stack_depth_value];
  stack_guard(values,
              op,
              builder_.CreateICmpULT(depth, builder_.getInt32(Registers::pc_stack_capacity)),
              partial);
  values[pc_stack_value] = builder_.CreateInsertElement(
      values[pc_stack_value], builder_.getInt32(((op.address + 1) & maximum_pc) | call_tag), depth);
  depth = builder_.CreateAdd(depth, builder_.getInt32(1));
  values[pc_stack_depth_value] = depth;
  values[interrupt_latch_value] = builder_.CreateOr(
      values[interrupt_latch_value],
      builder_.CreateSelect(builder_.CreateICmpEQ(depth, builder_.getInt32(almost_full)),
                            builder_.getInt32(stack_overflow_interrupt),
                            builder_.getInt32(0)));
  stack_status(values);
}

llvm::Value *Lowering::pop(Values &values, const Instruction &op, bool partial) {
  // PCSTK bit 25 records loop termination or interrupt continuation (A-10).
  constexpr std::uint32_t sequencer_tag = 1U << 25;
  auto *depth = values[pc_stack_depth_value];
  stack_guard(values,
              op,
              builder_.CreateAnd(
                  builder_.CreateICmpNE(depth, builder_.getInt32(0)),
                  builder_.CreateICmpULE(depth, builder_.getInt32(Registers::pc_stack_capacity))),
              partial);
  auto *popped_depth = builder_.CreateSub(depth, builder_.getInt32(1));
  auto *top = builder_.CreateExtractElement(values[pc_stack_value], popped_depth);
  stack_guard(values,
              op,
              builder_.CreateICmpEQ(builder_.CreateAnd(top, builder_.getInt32(sequencer_tag)),
                                    builder_.getInt32(0)),
              partial);
  values[pc_stack_depth_value] = popped_depth;
  stack_status(values);
  return builder_.CreateAnd(top, builder_.getInt32(maximum_pc));
}

void Lowering::return_to_caller(Values values, llvm::Value *target) {
  auto *unknown = llvm::BasicBlock::Create(module_.getContext(), "cold.return", function_);
  auto *dispatch = builder_.CreateSwitch(target, unknown);
  std::set<std::uint32_t> returns;
  for (const auto &op : instructions_) {
    if (op.type == direct_branch_type && (op.word & call_branch) != 0) {
      returns.insert((op.address + 1) & maximum_pc);
    }
  }
  for (const auto address : returns) {
    const auto found = addresses_.find(address);
    if (found == addresses_.end()) { continue; }
    auto &destination = admission_[found->second];
    destination.incoming(values, builder_.GetInsertBlock());
    dispatch->addCase(builder_.getInt32(address), destination.block);
  }
  builder_.SetInsertPoint(unknown);
  exit(values, target, NativeExit::indirect);
}

void Lowering::credit(Values &values, llvm::Value *count, llvm::Value *cycles, bool partial) {
  values[instruction_remaining_value] =
      builder_.CreateSub(values[instruction_remaining_value], count);
  auto *available = values[cycle_remaining_value];
  auto *remaining = builder_.CreateSub(available, cycles);
  if (partial) {
    remaining = builder_.CreateSelect(
        builder_.CreateICmpUGE(available, cycles), remaining, builder_.getInt64(0));
  }
  values[cycle_remaining_value] = remaining;
  values[instructions_retired_value] =
      builder_.CreateAdd(values[instructions_retired_value], count);
  values[cycles_retired_value] = builder_.CreateAdd(values[cycles_retired_value], cycles);
}

void Lowering::complete(Values values, const Instruction &op, bool partial) {
  auto *count = partial ? builder_.getInt64(1) : values[credit_value];
  const auto next = (op.address + 1) & maximum_pc;
  if (op.type != direct_branch_type && op.type != return_branch_type) {
    credit(values, count, count, partial);
    finish(values, next);
    return;
  }
  auto *lanes =
      condition(values, static_cast<unsigned>((op.word >> condition_shift) & true_condition));
  // Conditional branches need the preceding compute's flags in Address, before
  // Execute writes them back. The interlock survives owner bounds (4-115).
  auto *stall = ((op.word >> condition_shift) & true_condition) == true_condition
                    ? builder_.getInt64(0)
                    : builder_.CreateZExt(values[hazard_value], builder_.getInt64Ty());
  auto *cycles = builder_.CreateAdd(count, stall);
  auto *taken = builder_.CreateExtractElement(lanes, builder_.getInt32(0));
  if ((context_ & simd_mode) != 0) {
    taken = builder_.CreateAnd(taken, builder_.CreateExtractElement(lanes, builder_.getInt32(1)));
  }
  auto *branch = llvm::BasicBlock::Create(module_.getContext(), "jump", function_);
  auto *fallthrough = llvm::BasicBlock::Create(module_.getContext(), "fallthrough", function_);
  builder_.CreateCondBr(taken, branch, fallthrough);
  builder_.SetInsertPoint(branch);
  auto branch_values = values;
  if (op.type == direct_branch_type && (op.word & call_branch) != 0) {
    call(branch_values, op, partial);
  }
  auto *target = op.type == return_branch_type ? pop(branch_values, op, partial) : nullptr;
  branch_values[hazard_value] = builder_.getFalse();
  credit(
      branch_values, count, builder_.CreateAdd(cycles, builder_.getInt64(branch_penalty)), partial);
  if (target != nullptr) {
    return_to_caller(branch_values, target);
  } else {
    finish(branch_values, branch_target(op));
  }
  builder_.SetInsertPoint(fallthrough);
  values[hazard_value] = builder_.getFalse();
  credit(values, count, cycles, partial);
  finish(values, next);
}

void Lowering::finish(Values values, std::uint32_t next) {
  const auto target = addresses_.find(next);
  if (target != addresses_.end()) {
    jump(admission_[target->second], values);
    return;
  }
  auto *bound = llvm::BasicBlock::Create(module_.getContext(), "cold.bound", function_);
  auto *missing = llvm::BasicBlock::Create(module_.getContext(), "cold.undeclared", function_);
  auto *done = builder_.CreateOr(
      builder_.CreateICmpEQ(values[instruction_remaining_value], builder_.getInt64(0)),
      builder_.CreateICmpEQ(values[cycle_remaining_value], builder_.getInt64(0)));
  builder_.CreateCondBr(done, bound, missing);
  builder_.SetInsertPoint(bound);
  exit(values, builder_.getInt32(next), NativeExit::bound);
  builder_.SetInsertPoint(missing);
  exit(values, builder_.getInt32(next), NativeExit::undeclared);
}

void Lowering::emit_exit() {
  builder_.SetInsertPoint(exit_.block);
  const auto values = exit_.values();
  for (const auto reg : written_) {
    store(builder_.CreateExtractElement(values[reg], builder_.getInt32(0)), data_slot(0, reg));
    if ((context_ & simd_mode) != 0) {
      store(builder_.CreateExtractElement(values[reg], builder_.getInt32(1)), data_slot(1, reg));
    }
  }
  if (flags_written_) {
    for (unsigned lane = 0; lane < ((context_ & simd_mode) != 0 ? 2U : 1U); ++lane) {
      store(builder_.CreateExtractElement(values[status_value], builder_.getInt32(lane)),
            astat_slot + lane);
      store(builder_.CreateExtractElement(values[sticky_value], builder_.getInt32(lane)),
            stky_slot + lane);
    }
  }
  if (stack_written_) {
    for (unsigned index = 0; index < Registers::pc_stack_capacity; ++index) {
      store(builder_.CreateExtractElement(values[pc_stack_value], builder_.getInt32(index)),
            stack_slot + index);
    }
    store(builder_.CreateTrunc(values[pc_stack_depth_value], builder_.getInt8Ty()),
          stack_depth_slot);
    store(values[interrupt_latch_value], interrupt_latch_slot);
  }
  store(exit_pc_, pc_slot);
  auto *retired = builder_.CreateICmpNE(values[instructions_retired_value], builder_.getInt64(0));
  auto *fetch = builder_.CreateAnd(builder_.CreateAdd(exit_pc_, builder_.getInt32(4)),
                                   builder_.getInt32(maximum_pc));
  auto *decode = builder_.CreateAnd(builder_.CreateAdd(exit_pc_, builder_.getInt32(2)),
                                    builder_.getInt32(maximum_pc));
  store(builder_.CreateSelect(retired, fetch, initial_fetch_), faddr_slot);
  store(builder_.CreateSelect(retired, decode, initial_decode_), daddr_slot);
  store(builder_.CreateAdd(initial_clock_, values[cycles_retired_value]), clock_slot);
  store(builder_.CreateZExt(values[hazard_value], builder_.getInt8Ty()), hazard_slot);
  for (const auto [slot, value] :
       {std::pair{retired_instructions, values[instructions_retired_value]},
        std::pair{retired_cycles, values[cycles_retired_value]},
        std::pair{exit_reason, static_cast<llvm::Value *>(exit_reason_)}}) {
    builder_.CreateStore(
        value,
        builder_.CreateGEP(builder_.getInt64Ty(), function_->getArg(1), builder_.getInt64(slot)));
  }
  builder_.CreateRetVoid();
}

std::vector<BlockCost> Lowering::block_costs() const {
  std::set<std::uint32_t> leaders(variant_.entries.begin(), variant_.entries.end());
  for (const auto &vector : variant_.interrupt_vectors) { leaders.insert(vector.vector); }
  std::vector<BlockCost> result(instructions_.size());
  for (const auto &op : instructions_) {
    if (op.type == direct_branch_type) { leaders.insert(branch_target(op)); }
  }
  for (std::size_t i = instructions_.size(); i != 0; --i) {
    const auto index = i - 1;
    const auto &op = instructions_[index];
    if (op.type == direct_branch_type || op.type == return_branch_type) {
      result[index].cycles += branch_penalty;
      if (((op.word >> condition_shift) & true_condition) != true_condition) {
        ++result[index].cycles;
      }
      continue;
    }
    if (i < instructions_.size() && op.address + 1 == instructions_[i].address &&
        !leaders.contains(instructions_[i].address)) {
      result[index].instructions += result[i].instructions;
      result[index].cycles += result[i].cycles;
    }
  }
  return result;
}

void Lowering::emit() {
  stack_written_ = std::ranges::any_of(instructions_, [](const auto &op) {
    return op.type == return_branch_type ||
           (op.type == direct_branch_type && (op.word & call_branch) != 0);
  });
  auto *entry = llvm::BasicBlock::Create(module_.getContext(), "entry", function_);
  exit_ = node("cold.exit");
  llvm::IRBuilder<> exit_builder(exit_.block);
  exit_pc_ = exit_builder.CreatePHI(builder_.getInt32Ty(), 2);
  exit_reason_ = exit_builder.CreatePHI(builder_.getInt64Ty(), 2);
  for (std::size_t i = 0; i < instructions_.size(); ++i) {
    addresses_.emplace(instructions_[i].address, i);
    admission_.push_back(node("admit." + std::to_string(i)));
    fast_.push_back(node("body." + std::to_string(i)));
    partial_.push_back(node("cold.prefix." + std::to_string(i)));
  }
  const auto remaining = block_costs();
  builder_.SetInsertPoint(entry);
  Values initial{};
  for (unsigned reg = 0; reg < 16; ++reg) {
    initial[reg] = pair(load(64, data_slot(0, reg)), load(64, data_slot(1, reg)), 64);
  }
  initial[status_value] = pair(load(32, astat_slot), load(32, astat_slot + 1), 32);
  initial[sticky_value] = pair(load(32, stky_slot), load(32, stky_slot + 1), 32);
  for (const auto [value, slot] : {std::pair{instruction_remaining_value, remaining_instructions},
                                   std::pair{cycle_remaining_value, remaining_cycles}}) {
    initial[value] = builder_.CreateLoad(
        builder_.getInt64Ty(),
        builder_.CreateGEP(builder_.getInt64Ty(), function_->getArg(1), builder_.getInt64(slot)));
  }
  for (const auto value : {instructions_retired_value, cycles_retired_value, credit_value}) {
    initial[value] = builder_.getInt64(0);
  }
  initial_clock_ = load(64, clock_slot);
  initial[hazard_value] = builder_.CreateICmpNE(load(8, hazard_slot), builder_.getInt8(0));
  initial[pc_stack_value] = llvm::PoisonValue::get(stack_type());
  initial[pc_stack_depth_value] = builder_.getInt32(0);
  initial[interrupt_latch_value] = builder_.getInt32(0);
  if (stack_written_) {
    for (unsigned index = 0; index < Registers::pc_stack_capacity; ++index) {
      initial[pc_stack_value] = builder_.CreateInsertElement(
          initial[pc_stack_value], load(32, stack_slot + index), builder_.getInt32(index));
    }
    initial[pc_stack_depth_value] =
        builder_.CreateZExt(load(8, stack_depth_slot), builder_.getInt32Ty());
    initial[interrupt_latch_value] = load(32, interrupt_latch_slot);
  }
  initial_fetch_ = load(32, faddr_slot);
  initial_decode_ = load(32, daddr_slot);
  auto *pc = load(32, pc_slot);
  auto *resume = llvm::BasicBlock::Create(module_.getContext(), "cold.resume", function_);
  auto *unknown = llvm::BasicBlock::Create(module_.getContext(), "cold.entry", function_);
  const auto root = addresses_.at(entry_address_);
  admission_[root].incoming(initial, entry);
  builder_.CreateCondBr(
      builder_.CreateICmpEQ(pc, builder_.getInt32(entry_address_)),
      admission_[root].block,
      resume,
      llvm::MDBuilder(module_.getContext()).createBranchWeights(common_weight, 1));
  builder_.SetInsertPoint(resume);
  auto *dispatch = builder_.CreateSwitch(pc, unknown);
  for (std::size_t i = 0; i < instructions_.size(); ++i) {
    if (i == root) { continue; }
    admission_[i].incoming(initial, resume);
    dispatch->addCase(builder_.getInt32(instructions_[i].address), admission_[i].block);
  }
  builder_.SetInsertPoint(unknown);
  exit(initial, pc, NativeExit::undeclared);
  for (std::size_t i = 0; i < instructions_.size(); ++i) {
    const auto &op = instructions_[i];
    builder_.SetInsertPoint(admission_[i].block);
    auto values = admission_[i].values();
    values[credit_value] = builder_.getInt64(remaining[i].instructions);
    auto *admitted = builder_.CreateAnd(
        builder_.CreateICmpUGE(values[instruction_remaining_value], values[credit_value]),
        builder_.CreateICmpUGE(values[cycle_remaining_value],
                               builder_.getInt64(remaining[i].cycles)));
    fast_[i].incoming(values, admission_[i].block);
    partial_[i].incoming(values, admission_[i].block);
    builder_.CreateCondBr(
        admitted,
        fast_[i].block,
        partial_[i].block,
        llvm::MDBuilder(module_.getContext()).createBranchWeights(common_weight, 1));
    builder_.SetInsertPoint(fast_[i].block);
    values = fast_[i].values();
    instruction(values, op);
    if (remaining[i].instructions > 1) {
      jump(fast_[i + 1], values);
    } else {
      complete(values, op, false);
    }
    builder_.SetInsertPoint(partial_[i].block);
    values = partial_[i].values();
    auto *execute = llvm::BasicBlock::Create(
        module_.getContext(), "cold.execute." + std::to_string(i), function_);
    auto *bound =
        llvm::BasicBlock::Create(module_.getContext(), "cold.stop." + std::to_string(i), function_);
    auto *available = builder_.CreateAnd(
        builder_.CreateICmpNE(values[instruction_remaining_value], builder_.getInt64(0)),
        builder_.CreateICmpNE(values[cycle_remaining_value], builder_.getInt64(0)));
    builder_.CreateCondBr(available, execute, bound);
    builder_.SetInsertPoint(bound);
    exit(values, builder_.getInt32(op.address), NativeExit::bound);
    builder_.SetInsertPoint(execute);
    instruction(values, op);
    if (remaining[i].instructions > 1) {
      credit(values, builder_.getInt64(1), builder_.getInt64(1), true);
      jump(partial_[i + 1], values);
    } else {
      complete(values, op, true);
    }
  }
  emit_exit();
}

std::uint32_t context_mask(const std::vector<Instruction> &instructions) {
  std::uint32_t mask{};
  for (const auto &instruction : instructions) {
    if (instruction.type == direct_branch_type || instruction.type == return_branch_type) {
      mask |= simd_mode | saturation_mode;
    }
    if (instruction.type == immediate_move_type) {
      mask |= low_bank_mode | high_bank_mode | simd_mode;
    }
    if (instruction.compute.kind == ComputeKind::none) { continue; }
    mask |= low_bank_mode | high_bank_mode | simd_mode | saturation_mode;
    if (instruction.compute.opcode == alu::average) { mask |= truncation_mode; }
  }
  return mask;
}

std::vector<Instruction> connected_code(const std::vector<Instruction> &code, std::uint32_t entry) {
  std::map<std::uint32_t, const Instruction *> addresses;
  for (const auto &op : code) { addresses.emplace(op.address, &op); }
  std::set<std::uint32_t> visited;
  std::vector<std::uint32_t> pending{entry};
  std::vector<Instruction> result;
  while (!pending.empty()) {
    const auto address = pending.back();
    pending.pop_back();
    const auto found = addresses.find(address);
    if (found == addresses.end() || !visited.insert(address).second) { continue; }
    const auto &op = *found->second;
    result.push_back(op);
    if (op.type == direct_branch_type) {
      pending.push_back(branch_target(op));
      if ((op.word & call_branch) == 0 &&
          ((op.word >> condition_shift) & true_condition) == true_condition) {
        continue;
      }
    }
    if (op.type == return_branch_type &&
        ((op.word >> condition_shift) & true_condition) == true_condition) {
      continue;
    }
    pending.push_back((op.address + 1) & maximum_pc);
  }
  std::ranges::sort(result, {}, &Instruction::address);
  return result;
}
} // namespace

std::shared_ptr<const PreparedImage> lower_image(const CodeImage &image,
                                                 const CompileOptions &options) {
  auto result = std::make_shared<PreparedImage>();
  result->identity = input_identity(image, options);
  if (options.mode != ExecutionMode::strict && options.mode != ExecutionMode::fast_fp32) {
    refuse(FaultReason::invalid_image, result->identity, 0, 0, "unknown numerical mode");
  }
  auto instructions = decode(image, result->identity);
  for (const auto &instruction : instructions) { validate(instruction, result->identity); }
  result->code = image;
  result->options = options;
  llvm::LLVMContext context;
  llvm::Module module(result->identity, context);
  for (const auto &variant : image.variants) {
    std::vector<Instruction> code;
    std::ranges::copy_if(instructions, std::back_inserter(code), [&](const auto &instruction) {
      return instruction.variant == variant.id;
    });
    std::set<std::uint32_t> entries(variant.entries.begin(), variant.entries.end());
    for (const auto &request : variant.interrupt_vectors) { entries.insert(request.vector); }
    for (const auto entry : entries) {
      const auto connected = connected_code(code, entry);
      const auto mask = context_mask(connected);
      std::vector<std::uint32_t> contexts{0};
      for (const auto bit :
           {low_bank_mode, high_bank_mode, saturation_mode, truncation_mode, simd_mode}) {
        if ((mask & bit) == 0) { continue; }
        const auto count = contexts.size();
        for (std::size_t i = 0; i < count; ++i) { contexts.push_back(contexts[i] | bit); }
      }
      for (const auto value : contexts) {
        const auto symbol = "sharc_" + result->identity + "_v" + std::to_string(variant.id) + "_e" +
                            std::to_string(entry) + "_m" + std::to_string(value);
        NativeUnit unit{.variant = variant.id,
                        .entry = entry,
                        .mask = mask,
                        .context = value,
                        .symbol = symbol};
        Lowering(module, variant, connected, unit).emit();
        result->exports.push_back(symbol);
        result->units.push_back(std::move(unit));
      }
    }
  }
  std::string error;
  llvm::raw_string_ostream diagnostic(error);
  if (llvm::verifyModule(module, &diagnostic)) {
    refuse(FaultReason::invalid_image,
           result->identity,
           0,
           0,
           "generated LLVM module is invalid: " + error);
  }
  llvm::SmallVector<char> bytes;
  llvm::raw_svector_ostream output(bytes);
  llvm::WriteBitcodeToFile(module, output);
  const auto data = std::as_bytes(std::span(bytes.data(), bytes.size()));
  result->bitcode.assign(data.begin(), data.end());
  return result;
}
} // namespace sharc::detail
