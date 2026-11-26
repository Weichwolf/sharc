// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "native.hpp"

#include <cstdint>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <span>

namespace sharc::detail {
// Six paired raw registers plus paired status fit the eight vector argument
// registers of the initial x86-64 ABI. All native units share this signature.
inline constexpr unsigned pinned_registers = 6;

enum UnitArgument : std::uint8_t {
  unit_pointers = 0,
  unit_controls = 1,
  unit_instructions = 2,
  unit_cycles = 3,
  unit_cycle_overrun = 4,
  unit_pc = 5,
  unit_root = 6,
  unit_mode = 7,
  unit_pipeline = 8,
  unit_reason = 9,
  unit_data = 10,
  unit_status = unit_data + pinned_registers,
  unit_sticky = unit_status + 1
};

struct Continuation {
  std::uint32_t variant{}, address{}, root{};
};

class NativeAbi {
public:
  NativeAbi(llvm::Module &module,
            std::span<const NativeUnit> units,
            std::span<const Continuation> continuations);
  [[nodiscard]] llvm::Function *body(const NativeUnit &unit) const;
  void leave(llvm::IRBuilder<> &builder,
             const NativeUnit &unit,
             std::span<llvm::Value *const> args) const;

private:
  void emit_reload();
  void emit_spill();
  void emit_dispatch(std::uint32_t variant);
  llvm::Module &module_;
  std::span<const NativeUnit> units_;
  std::span<const Continuation> continuations_;
  llvm::FunctionType *type_{};
  llvm::Function *reload_{}, *spill_{};
};
} // namespace sharc::detail
