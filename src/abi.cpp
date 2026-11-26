// SPDX-License-Identifier: GPL-3.0-only
#include "abi.hpp"

#include "decode.hpp"
#include "native.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Value.h>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sharc::detail {
namespace {
constexpr unsigned lanes = 2, lane_slots = 32, bank_slots = 16;
#ifdef SHARC_POISON_NATIVE_STATE
constexpr std::uint64_t poison_raw = 0xa5a5a5a5a5;
constexpr std::uint32_t poison_status = 0xa5a5a5a5;
#endif

llvm::Value *pointer(llvm::IRBuilder<> &builder, llvm::Value *pointers, llvm::Value *slot) {
  return builder.CreateLoad(builder.getPtrTy(),
                            builder.CreateGEP(builder.getPtrTy(), pointers, slot));
}

llvm::Value *pointer(llvm::IRBuilder<> &builder, llvm::Value *pointers, std::size_t slot) {
  return pointer(builder, pointers, builder.getInt64(slot));
}

struct ActiveBank {
  llvm::Value *pointers{}, *mode{};
};

llvm::Value *
data_pointer(llvm::IRBuilder<> &builder, const ActiveBank &bank, unsigned lane, unsigned reg) {
  auto *offset = builder.CreateSelect(
      builder.CreateICmpNE(builder.CreateAnd(bank.mode, builder.getInt32(low_bank_mode)),
                           builder.getInt32(0)),
      builder.getInt64(bank_slots),
      builder.getInt64(0));
  return pointer(builder,
                 bank.pointers,
                 builder.CreateAdd(offset, builder.getInt64((lane * lane_slots) + reg)));
}

std::vector<llvm::Value *> arguments(llvm::Function &function) {
  std::vector<llvm::Value *> result;
  for (auto &arg : function.args()) { result.push_back(&arg); }
  return result;
}

void tail(llvm::IRBuilder<> &builder,
          llvm::Function *destination,
          std::span<llvm::Value *const> values) {
  auto *call = builder.CreateCall(destination, llvm::ArrayRef(values.data(), values.size()));
  call->setCallingConv(llvm::CallingConv::PreserveNone);
  call->setTailCallKind(llvm::CallInst::TCK_MustTail);
  builder.CreateRetVoid();
}

std::string dispatcher(std::uint32_t variant) {
  return "sharc.native.dispatch." + std::to_string(variant);
}
} // namespace

NativeAbi::NativeAbi(llvm::Module &module,
                     std::span<const NativeUnit> units,
                     std::span<const Continuation> continuations)
    : module_(module), units_(units), continuations_(continuations) {
  llvm::IRBuilder<> builder(module.getContext());
  std::vector<llvm::Type *> types{builder.getPtrTy(),
                                  builder.getPtrTy(),
                                  builder.getInt64Ty(),
                                  builder.getInt64Ty(),
                                  builder.getInt64Ty(),
                                  builder.getInt32Ty(),
                                  builder.getInt32Ty(),
                                  builder.getInt32Ty(),
                                  builder.getInt32Ty(),
                                  builder.getInt64Ty()};
  types.insert(
      types.end(), pinned_registers, llvm::FixedVectorType::get(builder.getInt64Ty(), lanes));
  types.insert(types.end(), 2, llvm::FixedVectorType::get(builder.getInt32Ty(), lanes));
  type_ = llvm::FunctionType::get(builder.getVoidTy(), types, false);
  spill_ = llvm::Function::Create(
      type_, llvm::GlobalValue::InternalLinkage, "sharc.native.spill", module);
  spill_->setCallingConv(llvm::CallingConv::PreserveNone);
  reload_ = llvm::Function::Create(
      llvm::FunctionType::get(
          builder.getVoidTy(),
          {builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy(), builder.getInt32Ty()},
          false),
      llvm::GlobalValue::InternalLinkage,
      "sharc.native.reload",
      module);
  std::set<std::uint32_t> variants;
  for (const auto &unit : units_) {
    auto *function = llvm::Function::Create(
        type_, llvm::GlobalValue::InternalLinkage, unit.symbol + ".unit", module);
    function->setCallingConv(llvm::CallingConv::PreserveNone);
    variants.insert(unit.variant);
    auto *entry = llvm::Function::Create(
        llvm::FunctionType::get(
            builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy()}, false),
        llvm::GlobalValue::ExternalLinkage,
        unit.symbol,
        module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(module.getContext(), "entry", entry));
    builder.CreateCall(
        reload_, {entry->getArg(0), entry->getArg(1), function, builder.getInt32(unit.entry)});
    builder.CreateRetVoid();
  }
  emit_reload();
  emit_spill();
  for (const auto variant : variants) { emit_dispatch(variant); }
}

llvm::Function *NativeAbi::body(const NativeUnit &unit) const {
  return module_.getFunction(unit.symbol + ".unit");
}

void NativeAbi::emit_reload() {
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(module_.getContext(), "reload", reload_));
  std::vector<llvm::Value *> args(unit_sticky + 1);
  args[unit_pointers] = reload_->getArg(0);
  args[unit_controls] = reload_->getArg(1);
  const auto load = [&](llvm::Type *type, std::size_t slot) {
    return builder.CreateLoad(type, pointer(builder, args[unit_pointers], slot));
  };
  args[unit_mode] = load(builder.getInt32Ty(), mode_slot);
  args[unit_pc] = load(builder.getInt32Ty(), pc_slot);
  args[unit_root] = reload_->getArg(3);
  args[unit_pipeline] = load(builder.getInt32Ty(), pipeline_slot);
  args[unit_reason] = builder.getInt64(0);
  args[unit_cycle_overrun] = builder.getInt64(0);
  for (const auto [arg, slot] :
       {std::pair{unit_instructions, instruction_budget}, std::pair{unit_cycles, cycle_budget}}) {
    args[arg] = builder.CreateLoad(
        builder.getInt64Ty(),
        builder.CreateGEP(builder.getInt64Ty(), args[unit_controls], builder.getInt64(slot)));
  }
  for (unsigned reg = 0; reg < pinned_registers + 2; ++reg) {
    const bool data = reg < pinned_registers;
    auto *type = data ? builder.getInt64Ty() : builder.getInt32Ty();
    llvm::Value *value = llvm::PoisonValue::get(llvm::FixedVectorType::get(type, lanes));
    for (unsigned lane = 0; lane < lanes; ++lane) {
      auto *address =
          data ? data_pointer(
                     builder, {.pointers = args[unit_pointers], .mode = args[unit_mode]}, lane, reg)
               : pointer(builder,
                         args[unit_pointers],
                         (reg == pinned_registers ? astat_slot : stky_slot) + lane);
      value = builder.CreateInsertElement(
          value, builder.CreateLoad(type, address), builder.getInt32(lane));
#ifdef SHARC_POISON_NATIVE_STATE
      builder.CreateStore(llvm::ConstantInt::get(type, data ? poison_raw : poison_status), address);
#endif
    }
    args[unit_data + reg] = value;
  }
  auto *call = builder.CreateCall(type_, reload_->getArg(2), args);
  call->setCallingConv(llvm::CallingConv::PreserveNone);
  builder.CreateRetVoid();
}

void NativeAbi::emit_spill() {
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(module_.getContext(), "spill", spill_));
  auto *pointers = spill_->getArg(unit_pointers);
  const auto write = [&](llvm::Value *value, std::size_t slot) {
    builder.CreateStore(value, pointer(builder, pointers, slot));
  };
  for (unsigned reg = 0; reg < pinned_registers + 2; ++reg) {
    const bool data = reg < pinned_registers;
    for (unsigned lane = 0; lane < lanes; ++lane) {
      auto *address =
          data ? data_pointer(
                     builder, {.pointers = pointers, .mode = spill_->getArg(unit_mode)}, lane, reg)
               : pointer(
                     builder, pointers, (reg == pinned_registers ? astat_slot : stky_slot) + lane);
      auto *value =
          builder.CreateExtractElement(spill_->getArg(unit_data + reg), builder.getInt32(lane));
#ifdef SHARC_POISON_NATIVE_STATE
      auto *valid = llvm::BasicBlock::Create(module_.getContext(), "poison.intact", spill_);
      auto *broken = llvm::BasicBlock::Create(module_.getContext(), "poison.changed", spill_);
      builder.CreateCondBr(
          builder.CreateICmpEQ(
              builder.CreateLoad(value->getType(), address),
              llvm::ConstantInt::get(value->getType(), data ? poison_raw : poison_status)),
          valid,
          broken);
      builder.SetInsertPoint(broken);
      builder.CreateCall(llvm::Intrinsic::getDeclaration(&module_, llvm::Intrinsic::trap));
      builder.CreateUnreachable();
      builder.SetInsertPoint(valid);
#endif
      builder.CreateStore(value, address);
    }
  }
  auto *pc = spill_->getArg(unit_pc);
  write(pc, pc_slot);
  const auto control = [&](ControlSlot slot) {
    return builder.CreateGEP(
        builder.getInt64Ty(), spill_->getArg(unit_controls), builder.getInt64(slot));
  };
  const auto consumed = [&](ControlSlot slot, UnitArgument remaining) {
    return builder.CreateSub(builder.CreateLoad(builder.getInt64Ty(), control(slot)),
                             spill_->getArg(remaining));
  };
  auto *instructions = consumed(instruction_budget, unit_instructions);
  auto *cycles =
      builder.CreateAdd(consumed(cycle_budget, unit_cycles), spill_->getArg(unit_cycle_overrun));
  auto *retired = builder.CreateICmpNE(instructions, builder.getInt64(0));
  for (const auto [slot, displacement] : {std::pair{faddr_slot, 4U}, std::pair{daddr_slot, 2U}}) {
    auto *next = builder.CreateAnd(builder.CreateAdd(pc, builder.getInt32(displacement)),
                                   builder.getInt32(maximum_pc));
    write(builder.CreateSelect(
              retired,
              next,
              builder.CreateLoad(builder.getInt32Ty(), pointer(builder, pointers, slot))),
          slot);
  }
  write(
      builder.CreateAdd(
          builder.CreateLoad(builder.getInt64Ty(), pointer(builder, pointers, clock_slot)), cycles),
      clock_slot);
  write(spill_->getArg(unit_pipeline), pipeline_slot);
  builder.CreateStore(instructions, control(retired_instructions));
  builder.CreateStore(cycles, control(retired_cycles));
  builder.CreateStore(spill_->getArg(unit_reason), control(exit_reason));
  builder.CreateStore(builder.CreateZExt(spill_->getArg(unit_root), builder.getInt64Ty()),
                      control(resume_entry));
  builder.CreateRetVoid();
}

void NativeAbi::emit_dispatch(std::uint32_t variant) {
  auto *function = llvm::Function::Create(
      type_, llvm::GlobalValue::InternalLinkage, dispatcher(variant), module_);
  function->setCallingConv(llvm::CallingConv::PreserveNone);
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(module_.getContext(), "dispatch", function));
  auto args = arguments(*function);
  auto *done = llvm::BasicBlock::Create(module_.getContext(), "cold.spill", function);
  auto *impossible = llvm::BasicBlock::Create(module_.getContext(), "unreachable.mode", function);
  llvm::IRBuilder<>(impossible).CreateUnreachable();
  auto *returned = builder.CreateICmpEQ(
      args[unit_reason], builder.getInt64(static_cast<unsigned>(NativeExit::returned)));
  std::map<std::uint32_t, std::uint32_t> targets;
  std::set<std::uint32_t> roots;
  for (const auto &unit : units_) {
    if (unit.variant == variant) {
      targets.emplace(unit.entry, unit.entry);
      roots.insert(unit.entry);
    }
  }
  for (const auto &point : continuations_) {
    if (point.variant == variant) { targets.try_emplace(point.address, point.root); }
  }
  auto *dispatch = builder.CreateSwitch(args[unit_pc], done);
  for (const auto [address, root] : targets) {
    auto *target = llvm::BasicBlock::Create(module_.getContext(), "target.mode", function);
    dispatch->addCase(builder.getInt32(address), target);
    builder.SetInsertPoint(target);
    if (!roots.contains(address)) {
      auto *valid = llvm::BasicBlock::Create(module_.getContext(), "return.mode", function);
      builder.CreateCondBr(returned, valid, done);
      builder.SetInsertPoint(valid);
    }
    const auto unit = std::ranges::find_if(units_, [&](const auto &candidate) {
      return candidate.variant == variant && candidate.entry == root;
    });
    auto *mode = builder.CreateAnd(args[unit_mode], builder.getInt32(unit->mask));
    // Preparation enumerates every subset of this mask for the selected root.
    auto *modes = builder.CreateSwitch(mode, impossible);
    for (const auto &candidate : units_) {
      if (candidate.variant != variant || candidate.entry != root) { continue; }
      auto *hit = llvm::BasicBlock::Create(module_.getContext(), "unit", function);
      modes->addCase(builder.getInt32(candidate.context), hit);
      builder.SetInsertPoint(hit);
      args[unit_root] = builder.getInt32(root);
      tail(builder, body(candidate), args);
    }
  }
  args[unit_root] = function->getArg(unit_root);
  builder.SetInsertPoint(done);
  tail(builder, spill_, args);
}

void NativeAbi::leave(llvm::IRBuilder<> &builder,
                      const NativeUnit &unit,
                      std::span<llvm::Value *const> args) const {
  auto *dispatch = llvm::BasicBlock::Create(
      module_.getContext(), "cold.transfer", builder.GetInsertBlock()->getParent());
  auto *done = llvm::BasicBlock::Create(
      module_.getContext(), "cold.spill", builder.GetInsertBlock()->getParent());
  auto *indirect = builder.CreateICmpEQ(
      args[unit_reason], builder.getInt64(static_cast<unsigned>(NativeExit::indirect)));
  auto *returned = builder.CreateICmpEQ(
      args[unit_reason], builder.getInt64(static_cast<unsigned>(NativeExit::returned)));
  // The selected unit admits or stops at its own entry, including at a bound.
  builder.CreateCondBr(builder.CreateOr(indirect, returned), dispatch, done);
  builder.SetInsertPoint(dispatch);
  tail(builder, module_.getFunction(dispatcher(unit.variant)), args);
  builder.SetInsertPoint(done);
  tail(builder, spill_, args);
}
} // namespace sharc::detail
