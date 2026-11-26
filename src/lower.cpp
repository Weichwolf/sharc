// SPDX-License-Identifier: GPL-3.0-only
#include "abi.hpp"
#include "decode.hpp"
#include "native.hpp"
#include "sharc/core.hpp"
#include "sharc/memory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <llvm/ADT/SmallVector.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/TypeSize.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <memory>
#include <optional>
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

constexpr unsigned shadow_register_first = 0x50;
constexpr unsigned system_register_first = 0x60;
constexpr unsigned ureg_mask = 127, byte_mask = 255;
constexpr unsigned history_older_mask = 0xff00;
constexpr unsigned rotate_opcode = 0x08;
constexpr unsigned conditional_compute_type = 2;
constexpr unsigned immediate_shift_type = 6;
constexpr unsigned register_move_type = 5;
constexpr unsigned immediate_move_type = 17;
constexpr unsigned nop_type = 21;
constexpr unsigned direct_branch_type = 8;
constexpr unsigned computed_branch_type = 9;
constexpr unsigned return_branch_type = 11;
constexpr unsigned memory_move_type = 15;
constexpr unsigned index_modify_type = 19;
constexpr std::uint64_t forced_long = std::uint64_t{1} << 39;
constexpr std::uint64_t relative_branch = std::uint64_t{1} << 40;
constexpr std::uint64_t call_branch = std::uint64_t{1} << 39;
constexpr std::uint64_t abort_branch = std::uint64_t{1} << 38;
constexpr std::uint64_t delayed_branch = std::uint64_t{1} << 26;
constexpr std::uint64_t else_compute = std::uint64_t{1} << 25;
constexpr std::uint64_t clear_interrupt_branch = std::uint64_t{1} << 24;
// Programming Reference 2.4, 4-19: three pipeline bubbles on a taken jump.
constexpr unsigned branch_penalty = 3;
constexpr unsigned delay_slot_count = 2;
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
          std::ranges::find(fixed_operations, compute.opcode) != fixed_operations.end()) ||
         ((compute.kind == ComputeKind::shifter || compute.kind == ComputeKind::shift_immediate) &&
          compute.opcode == rotate_opcode);
}

struct IndexModify {
  unsigned source{}, destination{};
  bool reverse{};
};

IndexModify index_modify(const Instruction &instruction) {
  constexpr unsigned dag_shift = 38;
  constexpr unsigned destination_shift = 35;
  constexpr unsigned source_shift = 32;
  constexpr auto reverse = std::uint64_t{1} << 39;
  const auto dag = static_cast<unsigned>((instruction.word >> dag_shift) & 1) * 8;
  const auto source = static_cast<unsigned>((instruction.word >> source_shift) & 7);
  return {.source = source + dag,
          .destination =
              (source ^ static_cast<unsigned>((instruction.word >> destination_shift) & 7)) + dag,
          .reverse = (instruction.word & reverse) != 0};
}

struct PipelineEffects {
  llvm::Value *stall{}, *tag{};
};

struct RegisterMove {
  unsigned source{}, destination{};
  bool swap{};
};

RegisterMove register_move(const Instruction &op) {
  constexpr unsigned source_high_shift = 38;
  constexpr unsigned source_low_shift = 31;
  constexpr unsigned destination_shift = 23;
  constexpr auto swap_flag = std::uint64_t{1} << 43;
  if ((op.word & swap_flag) != 0) {
    return {.source =
                shadow_register_first + static_cast<unsigned>((op.word >> source_high_shift) & 15),
            .destination = static_cast<unsigned>((op.word >> destination_shift) & 15),
            .swap = true};
  }
  return {.source = static_cast<unsigned>(((op.word >> source_high_shift) & true_condition) << 2) |
                    static_cast<unsigned>((op.word >> source_low_shift) & 3),
          .destination = static_cast<unsigned>((op.word >> destination_shift) & byte_mask),
          .swap = (op.word & swap_flag) != 0};
}

bool data_register(unsigned reg) {
  return reg < 16 || (reg >= shadow_register_first && reg < system_register_first);
}

bool dag_register(unsigned reg) {
  return reg >= 0x10 && reg < shadow_register_first;
}

unsigned dag_number(unsigned reg) {
  return reg - 0x10;
}

unsigned loaded_register(const Instruction &op) {
  if (op.type == register_move_type) { return register_move(op).destination; }
  if (op.type == immediate_move_type) {
    return static_cast<unsigned>((op.word >> immediate_register_shift) & ureg_mask);
  }
  return 0;
}

bool branch(const Instruction &op) {
  return op.type == direct_branch_type || op.type == computed_branch_type ||
         op.type == return_branch_type;
}

bool call_branch_instruction(const Instruction &op) {
  return (op.type == direct_branch_type || op.type == computed_branch_type) &&
         (op.word & call_branch) != 0;
}

bool indirect_branch(const Instruction &op) {
  return op.type == computed_branch_type && (op.word & relative_branch) == 0;
}

unsigned branch_condition(const Instruction &op) {
  return static_cast<unsigned>((op.word >> condition_shift) & true_condition);
}

bool delayed(const Instruction &op) {
  return branch(op) && (op.word & delayed_branch) != 0;
}

std::uint32_t next_pc(const Instruction &op, unsigned words = 1) {
  return (op.address + words) & maximum_pc;
}

std::uint32_t call_resume(const Instruction &op) {
  return next_pc(op, delayed(op) ? delay_slot_count + 1 : 1);
}

unsigned branch_index(const Instruction &op) {
  constexpr unsigned shift = 30;
  return 8U + static_cast<unsigned>((op.word >> shift) & 7);
}

unsigned branch_modifier(const Instruction &op) {
  constexpr unsigned shift = 27;
  return 8U + static_cast<unsigned>((op.word >> shift) & 7);
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
  if ((instruction.word & (abort_branch | clear_interrupt_branch)) != 0) {
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "loop-abort or interrupt-clear branch is not lowered");
  }
  validate_condition(instruction, identity);
}

std::uint32_t branch_target(const Instruction &instruction) {
  if (instruction.type == computed_branch_type) {
    constexpr unsigned displacement_shift = 27;
    constexpr unsigned displacement_mask = 63;
    constexpr unsigned sign_bit = 32;
    const auto field =
        static_cast<std::uint32_t>(instruction.word >> displacement_shift) & displacement_mask;
    return (instruction.address + (field ^ sign_bit) - sign_bit) & maximum_pc;
  }
  const auto base = (instruction.word & relative_branch) != 0 ? instruction.address : 0;
  return (base + static_cast<std::uint32_t>(instruction.word)) & maximum_pc;
}

void validate_computed_branch(const Instruction &instruction, std::string_view identity) {
  constexpr auto reserved = std::uint64_t{1} << 23;
  if ((instruction.word & reserved) != 0) {
    reject(instruction, identity, FaultReason::invalid_encoding, "reserved computed-branch bit");
  }
  if ((instruction.word & (abort_branch | clear_interrupt_branch)) != 0 ||
      !supported_compute(instruction.compute)) {
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "modified branch or parallel compute is not lowered");
  }
  validate_condition(instruction, identity);
}

void validate_register_move(const Instruction &instruction, std::string_view identity) {
  const auto move = register_move(instruction);
  constexpr auto swap_allowed = std::uint64_t{0x78} << 40 | (std::uint64_t{15} << 38) |
                                (std::uint64_t{31} << condition_shift) | (std::uint64_t{15} << 23) |
                                0x7fffff;
  if (move.swap && (instruction.word & ~swap_allowed) != 0) {
    reject(instruction, identity, FaultReason::invalid_encoding, "reserved register-swap bits");
  }
  if ((!data_register(move.source) && !dag_register(move.source)) ||
      (!data_register(move.destination) && !dag_register(move.destination)) ||
      !supported_compute(instruction.compute)) {
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "system register or parallel compute is not lowered");
  }
  validate_condition(instruction, identity);
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
  if ((instruction.word >> opcode_shift) != 0x0a ||
      (instruction.word & (modifiers & ~delayed_branch)) != 0 ||
      instruction.compute.kind != ComputeKind::none) {
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "RTI, modified return or parallel return compute is not lowered");
  }
  validate_condition(instruction, identity);
}

void validate_memory_move(const Instruction &instruction, std::string_view identity) {
  constexpr unsigned register_shift = 32;
  constexpr unsigned y_first = 0x50;
  constexpr unsigned y_limit = 0x60;
  const auto ureg = (instruction.word >> register_shift) & 0x7f;
  if ((instruction.word & forced_long) == 0 && (ureg < 16 || (ureg >= y_first && ureg < y_limit))) {
    return;
  }
  reject(instruction,
         identity,
         FaultReason::unsupported_instruction,
         "forced long-word or non-data-register memory transfer is not lowered");
}

void validate_immediate_move(const Instruction &instruction, std::string_view identity) {
  constexpr std::uint64_t reserved_immediate_bit = std::uint64_t{1} << 39;
  if ((instruction.word & reserved_immediate_bit) != 0) {
    reject(instruction, identity, FaultReason::invalid_encoding, "reserved ISA immediate-move bit");
  }
  const auto reg = (instruction.word >> immediate_register_shift) & 0x7f;
  if (data_register(static_cast<unsigned>(reg)) || dag_register(static_cast<unsigned>(reg))) {
    return;
  }
  reject(instruction,
         identity,
         FaultReason::unsupported_instruction,
         "this universal-register immediate move is not lowered");
}

void validate_immediate_shift(const Instruction &instruction, std::string_view identity) {
  constexpr unsigned prefix_shift = 40;
  if ((instruction.word >> prefix_shift) != 2) {
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "parallel immediate shift and memory transfer is not lowered");
  }
  constexpr auto allowed = (std::uint64_t{2} << prefix_shift) |
                           (std::uint64_t{true_condition} << condition_shift) | 0x3fffff;
  if ((instruction.word & ~allowed) != 0) {
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "standalone immediate shift requires canonical unused fields");
  }
  validate_condition(instruction, identity);
  if (!supported_compute(instruction.compute)) {
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "immediate shifter operation has no LLVM lowering");
  }
}

void validate(const Instruction &instruction, std::string_view identity) {
  if (instruction.instruction_set != InstructionSet::isa) {
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "VISA native pipeline lowering is not implemented");
  }
  if (instruction.type == nop_type && instruction.word == 0) { return; }
  if (instruction.type == index_modify_type) { return; }
  if (instruction.type == register_move_type) {
    validate_register_move(instruction, identity);
    return;
  }
  if (instruction.type == direct_branch_type) {
    validate_branch(instruction, identity);
    return;
  }
  if (instruction.type == computed_branch_type) {
    validate_computed_branch(instruction, identity);
    return;
  }
  if (instruction.type == return_branch_type) {
    validate_return(instruction, identity);
    return;
  }
  if (instruction.type == memory_move_type) {
    validate_memory_move(instruction, identity);
    return;
  }
  if (instruction.type == immediate_move_type) {
    validate_immediate_move(instruction, identity);
    return;
  }
  if (instruction.type == immediate_shift_type) {
    validate_immediate_shift(instruction, identity);
    return;
  }
  if (instruction.type == conditional_compute_type) {
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
constexpr std::size_t cycle_overrun_value = cycle_remaining_value + 1;
constexpr std::size_t credit_value = cycle_overrun_value + 1;
constexpr std::size_t hazard_value = credit_value + 1;
constexpr std::size_t extra_cycles_value = hazard_value + 1;
constexpr std::size_t pc_stack_depth_value = extra_cycles_value + 1;
constexpr std::size_t interrupt_latch_value = pc_stack_depth_value + 1;
constexpr std::size_t loop_grant_value = interrupt_latch_value + 1;
constexpr std::size_t loop_remaining_value = loop_grant_value + 1;
constexpr std::size_t tail_value = loop_remaining_value + 1;
constexpr std::size_t dag_history_value = tail_value + 1;
constexpr std::size_t index_value = dag_history_value + 1;
constexpr std::size_t modifier_value = index_value + 16;
constexpr std::size_t length_value = modifier_value + 16;
constexpr std::size_t base_value = length_value + 16;
constexpr std::size_t state_value_count = base_value + 16;
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
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (phi[i] != nullptr) { phi[i]->addIncoming(values[i], predecessor); }
    }
  }
};

struct Arithmetic {
  llvm::Value *raw{}, *status{}, *sticky{};
};

struct FixedResult {
  llvm::Value *value{}, *carry{}, *overflow{}, *sign{};
};

struct MemoryPlan {
  std::array<llvm::Value *, 2> pointers{};
  std::uint64_t stride{};
};

struct BlockCost {
  std::uint64_t instructions{1}, cycles{1};
};

void debug_scope(llvm::DIBuilder &debug, llvm::Function &function, const std::string &filename) {
  auto *file = debug.createFile(filename, "");
  function.setSubprogram(
      debug.createFunction(file,
                           function.getName(),
                           function.getName(),
                           file,
                           0,
                           debug.createSubroutineType(debug.getOrCreateTypeArray({})),
                           0,
                           llvm::DINode::FlagZero,
                           llvm::DISubprogram::toSPFlags(false, true, true)));
}

void finalize_debug(llvm::Module &module, llvm::DIBuilder &debug) {
  // Retain inlined guest locations in adapters too; line zero has no guest attribution.
  for (auto &function : module) {
    if (function.isDeclaration()) { continue; }
    if (function.getSubprogram() == nullptr) { debug_scope(debug, function, "sharc-generated"); }
    auto *const unknown =
        llvm::DILocation::get(module.getContext(), 0, 0, function.getSubprogram());
    for (auto &block : function) {
      for (auto &instruction : block) {
        if (!instruction.getDebugLoc()) { instruction.setDebugLoc(unknown); }
      }
    }
  }
  debug.finalize();
}

class Lowering {
public:
  Lowering(llvm::Module &module,
           llvm::DIBuilder &debug,
           CompileOptions::MemoryMode memory_mode,
           const NativeAbi &abi,
           const CodeVariant &variant,
           std::vector<Instruction> instructions,
           const NativeUnit &unit)
      : module_(module),
        memory_mode_(memory_mode),
        abi_(abi),
        unit_(unit),
        variant_(variant),
        instructions_(std::move(instructions)),
        entry_address_(unit.entry),
        context_(unit.context),
        builder_(module.getContext()) {
    function_ = abi_.body(unit);
    function_->addFnAttr(llvm::Attribute::NoUnwind);
    debug_scope(debug, *function_, "sharc-variant-" + std::to_string(variant.id));
  }

  void emit();

private:
  llvm::Value *memory_binding(std::size_t region, bool writing) {
    return pointer(register_pointer_count + region +
                   (writing ? variant_.layout.regions.size() : 0));
  }

  bool host_protected(const RegionLayout &layout) const {
    return memory_mode_ == CompileOptions::MemoryMode::direct && layout.initialized &&
           layout.view == MemoryView::normal_word && layout.count == (std::uint64_t{1} << 32);
  }

  llvm::Type *vector_type(unsigned bits) const {
    return llvm::FixedVectorType::get(llvm::IntegerType::get(module_.getContext(), bits), 2);
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

  llvm::Value *stack_cell(llvm::Value *index) {
    return builder_.CreateInBoundsGEP(builder_.getInt32Ty(), pointer(stack_slot), index);
  }

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
      if (i >= index_value && dag_[i - index_value] == nullptr) { continue; }
      llvm::Type *type = builder_.getInt64Ty();
      if (i < instruction_remaining_value) { type = vector_type(32); }
      if (i < status_value) { type = vector_type(64); }
      if (i == hazard_value || i == tail_value) { type = builder_.getInt1Ty(); }
      if (i >= dag_history_value) { type = builder_.getInt32Ty(); }
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
  FixedResult word_step(llvm::Value *input, bool decrement);
  Arithmetic rotation(const Values &values, const Compute &compute);
  Arithmetic status_result(const Values &values,
                           const FixedResult &result,
                           llvm::Value *x,
                           llvm::Value *y,
                           unsigned opcode);
  void instruction(Values &values, const Instruction &op, bool partial);
  void memory_instruction(Values &values, const Instruction &op, bool partial);
  void modify_instruction(Values &values, const Instruction &op);
  llvm::Value *planned_pointer(const Instruction &op, llvm::Value *address, unsigned transfer);
  bool plan_changing_memory(const Instruction &op);
  llvm::Value *
  memory_pointer(const Values &values, const Instruction &op, llvm::Value *address, bool partial);
  void memory_failure(const Values &values,
                      const Instruction &op,
                      llvm::Value *address,
                      NativeExit reason,
                      bool partial);
  void finish(Values values, std::uint32_t next, bool partial);
  void credit(Values &values, llvm::Value *count, llvm::Value *cycles, bool partial);
  void complete(Values values,
                const Instruction &op,
                bool partial,
                llvm::Value *lanes,
                PipelineEffects pipeline);
  void delayed_slot(const Values &values, const Instruction &op, bool partial);
  void redirect(Values values, const Instruction &op, llvm::Value *target, bool partial);
  [[nodiscard]] bool delay_boundary(std::uint32_t pc) const;
  llvm::Value *branch_lanes(const Values &values, const Instruction &op, bool partial);
  llvm::Value *branch_taken(llvm::Value *lanes);
  void stack_guard(const Values &values, const Instruction &op, llvm::Value *allowed, bool partial);
  void stack_status(Values &values);
  void call(Values &values, const Instruction &op, bool partial);
  llvm::Value *pop(Values &values, const Instruction &op, bool partial);
  void return_to_caller(Values values, llvm::Value *target, bool partial);
  std::vector<BlockCost> block_costs() const;
  void emit_exit();
  void load_indices();
  void load_dag(unsigned reg);
  llvm::Value *read_register(const Values &values, unsigned reg);
  void write_register(Values &values, unsigned reg, llvm::Value *raw, llvm::Value *selected);
  void register_instruction(Values &values, const Instruction &op, const Values &before);
  llvm::Value *pipeline_stall(const Values &values, const Instruction &op);
  llvm::Value *loaded_tag(const Values &values, const Instruction &op);
  void retire_pipeline(Values &values, PipelineEffects effects);
  void plan_memory();
  std::pair<llvm::Value *, llvm::Value *> plan_pointer(const MemoryMove &move,
                                                       llvm::Value *address);
  void emit_instruction(std::size_t i, const std::optional<BlockCost> &grant, bool dead_status);
  [[nodiscard]] std::vector<bool> dead_alu_status() const;
  void plan_loops();
  void complete_loop(Values values, const Instruction &op);
  std::vector<std::optional<BlockCost>> unit_costs() const;

  llvm::Module &module_;
  CompileOptions::MemoryMode memory_mode_;
  const NativeAbi &abi_;
  const NativeUnit &unit_;
  const CodeVariant &variant_;
  std::vector<Instruction> instructions_;
  std::uint32_t entry_address_;
  std::uint32_t context_;
  llvm::IRBuilder<> builder_;
  llvm::Function *function_{};
  std::map<std::uint32_t, std::size_t> addresses_;
  std::set<std::uint32_t> delay_slots_;
  std::map<std::uint32_t, const Instruction *> delay_ends_;
  std::vector<Node> admission_, fast_entry_, fast_, partial_;
  std::map<std::uint32_t, MemoryPlan> memory_plan_;
  llvm::Value *memory_admitted_{};
  std::set<unsigned> written_;
  std::set<unsigned> y_written_;
  std::array<llvm::Value *, 64> dag_{};
  std::set<unsigned> dag_written_;
  bool dag_status_written_{};
  std::vector<BlockCost> costs_;
  std::map<std::size_t, std::size_t> loop_headers_;
  std::map<std::size_t, BlockCost> loop_ends_;
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
    case alu::subtract_borrow: {
      const bool subtract = opcode == alu::subtract_borrow;
      auto *extra = subtract ? builder_.CreateSub(carry_in, vector(64, 1)) : carry_in;
      value = builder_.CreateAdd(sx, extra);
      auto *unsigned_value = builder_.CreateAdd(ux, extra);
      carry = subtract ? builder_.CreateICmpSGE(unsigned_value, vector(64, 0))
                       : builder_.CreateICmpUGE(unsigned_value, vector(64, unsigned_limit));
      range_check = true;
      break;
    }
    case alu::increment:
    case alu::decrement:
      return status_result(values, word_step(x, opcode == alu::decrement), x, y, opcode);
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

FixedResult Lowering::word_step(llvm::Value *input, bool decrement) {
  auto *limit = vector(32, decrement ? static_cast<std::uint32_t>(fixed_minimum) : fixed_maximum);
  auto *one = vector(32, 1);
  auto *word = decrement ? builder_.CreateSub(input, one) : builder_.CreateAdd(input, one);
  auto *overflow = builder_.CreateICmpEQ(input, limit);
  auto *carry =
      decrement
          ? builder_.CreateICmpNE(input, vector(32, 0))
          : builder_.CreateICmpEQ(input, vector(32, std::numeric_limits<std::uint32_t>::max()));
  if ((context_ & saturation_mode) != 0) { word = builder_.CreateSelect(overflow, limit, word); }
  return {.value = builder_.CreateZExt(word, vector_type(64)),
          .carry = carry,
          .overflow = overflow,
          .sign = vector(1, 0)};
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

llvm::Value *Lowering::read_register(const Values &values, unsigned reg) {
  if (dag_register(reg)) {
    auto *word = builder_.CreateShl(
        builder_.CreateZExt(values[index_value + dag_number(reg)], builder_.getInt64Ty()),
        builder_.getInt64(raw_extension_bits));
    return pair(word, word, 64);
  }
  auto *raw = values[reg & 15];
  return reg < 16 ? raw : builder_.CreateShuffleVector(raw, {1, 0});
}

void Lowering::write_register(Values &values,
                              unsigned reg,
                              llvm::Value *raw,
                              llvm::Value *selected) {
  if (dag_register(reg)) {
    const auto slot = index_value + dag_number(reg);
    auto *word = builder_.CreateTrunc(
        builder_.CreateLShr(builder_.CreateExtractElement(raw, builder_.getInt32(0)),
                            builder_.getInt64(raw_extension_bits)),
        builder_.getInt32Ty());
    values[slot] = builder_.CreateSelect(
        builder_.CreateExtractElement(selected, builder_.getInt32(0)), word, values[slot]);
    return;
  }
  if (reg >= 16) {
    raw = builder_.CreateShuffleVector(raw, {1, 0});
    selected = builder_.CreateShuffleVector(selected, {1, 0});
    y_written_.insert(reg & 15);
  }
  values[reg & 15] = builder_.CreateSelect(selected, raw, values[reg & 15]);
  written_.insert(reg & 15);
}

void Lowering::register_instruction(Values &values, const Instruction &op, const Values &before) {
  const auto move = register_move(op);
  auto *selected =
      condition(before, static_cast<unsigned>((op.word >> condition_shift) & true_condition));
  if (!move.swap) {
    write_register(values, move.destination, read_register(before, move.source), selected);
    return;
  }
  auto *x = builder_.CreateExtractElement(selected, builder_.getInt32(0));
  auto *y = (context_ & simd_mode) != 0
                ? builder_.CreateExtractElement(selected, builder_.getInt32(1))
                : x;
  write_register(values,
                 move.destination,
                 read_register(before, move.source),
                 pair(x, builder_.getFalse(), 1));
  write_register(values,
                 move.source,
                 read_register(before, move.destination),
                 pair(y, builder_.getFalse(), 1));
}

llvm::Value *Lowering::loaded_tag(const Values &values, const Instruction &op) {
  const auto reg = loaded_register(op);
  if (!dag_register(reg)) { return builder_.getInt32(0); }
  const auto index = reg & 15;
  const auto bank = (context_ & dag_bank_mode(index)) != 0 ? 1U : 0U;
  const auto tag = 1 + index + (bank * 16) + (reg >= 0x20 && reg < 0x30 ? 32U : 0U);
  auto *selected =
      condition(values,
                op.type == immediate_move_type
                    ? true_condition
                    : static_cast<unsigned>((op.word >> condition_shift) & true_condition));
  return builder_.CreateSelect(builder_.CreateExtractElement(selected, builder_.getInt32(0)),
                               builder_.getInt32(tag),
                               builder_.getInt32(0));
}

llvm::Value *Lowering::pipeline_stall(const Values &values, const Instruction &op) {
  llvm::Value *stall = builder_.getInt64(0);
  std::vector<unsigned> dependencies;
  if (op.type == memory_move_type) { dependencies.push_back(memory_move(op).index); }
  if (op.type == index_modify_type) { dependencies.push_back(index_modify(op).source); }
  if (indirect_branch(op)) {
    dependencies.push_back(branch_index(op));
    dependencies.push_back(16 + branch_modifier(op));
  }
  auto *previous = builder_.CreateAnd(values[dag_history_value], builder_.getInt32(byte_mask));
  auto *earlier = builder_.CreateLShr(values[dag_history_value], builder_.getInt32(8));
  llvm::Value *recent_match = builder_.getFalse();
  llvm::Value *earlier_match = builder_.getFalse();
  for (const auto dependency : dependencies) {
    const auto index = dependency & 15;
    const auto bank = (context_ & dag_bank_mode(index)) != 0 ? 1U : 0U;
    auto *tag = builder_.getInt32(1 + index + (bank * 16) + (dependency >= 16 ? 32U : 0U));
    recent_match = builder_.CreateOr(recent_match, builder_.CreateICmpEQ(previous, tag));
    earlier_match = builder_.CreateOr(earlier_match, builder_.CreateICmpEQ(earlier, tag));
  }
  stall = builder_.CreateSelect(recent_match,
                                builder_.getInt64(2),
                                builder_.CreateSelect(earlier_match, builder_.getInt64(1), stall));
  if (branch(op) && ((op.word >> condition_shift) & true_condition) != true_condition) {
    // Concurrent Decode/Address hazards overlap, rather than adding sequential stalls.
    auto *compute = builder_.CreateZExt(values[hazard_value], builder_.getInt64Ty());
    stall = builder_.CreateSelect(builder_.CreateICmpUGT(compute, stall), compute, stall);
  }
  return stall;
}

void Lowering::retire_pipeline(Values &values, PipelineEffects effects) {
  const auto [stall, tag] = effects;
  auto *history =
      builder_.CreateAnd(builder_.CreateShl(values[dag_history_value], builder_.getInt32(8)),
                         builder_.getInt32(history_older_mask));
  history = builder_.CreateSelect(
      builder_.CreateICmpEQ(stall, builder_.getInt64(0)), history, builder_.getInt32(0));
  values[dag_history_value] = builder_.CreateOr(history, tag);
  values[extra_cycles_value] = builder_.CreateAdd(values[extra_cycles_value], stall);
}

Arithmetic Lowering::rotation(const Values &values, const Compute &compute) {
  constexpr std::uint32_t shifter_flags = 0x3800;
  constexpr std::uint32_t shifter_zero = 0x1000;
  const auto word = [&](unsigned reg) {
    return builder_.CreateTrunc(builder_.CreateLShr(values[reg], vector(64, raw_extension_bits)),
                                vector_type(32));
  };
  auto *source = word(compute.inputs[0]);
  // Funnel rotation reduces modulo 32, including signed byte counts.
  auto *count = compute.kind == ComputeKind::shift_immediate
                    ? vector(32, compute.immediate & byte_mask)
                    : word(compute.inputs[1]);
  auto *result =
      builder_.CreateIntrinsic(llvm::Intrinsic::fshl, {vector_type(32)}, {source, source, count});
  auto *zero = builder_.CreateICmpEQ(result, vector(32, 0));
  auto *status =
      builder_.CreateOr(builder_.CreateAnd(values[status_value], vector(32, ~shifter_flags)),
                        flag(zero, shifter_zero));
  auto *raw = builder_.CreateShl(builder_.CreateZExt(result, vector_type(64)),
                                 vector(64, raw_extension_bits));
  return {.raw = raw, .status = status, .sticky = values[sticky_value]};
}

void Lowering::instruction(Values &values, const Instruction &op, bool partial) {
  if (op.type == memory_move_type) {
    memory_instruction(values, op, partial);
    values[hazard_value] = builder_.getFalse();
    return;
  }
  if (!branch(op)) {
    values[hazard_value] = builder_.getInt1(op.compute.kind != ComputeKind::none);
  }
  if (op.type == immediate_move_type) {
    const auto reg = loaded_register(op);
    auto *raw = vector(
        64, static_cast<std::uint64_t>(static_cast<std::uint32_t>(op.word)) << raw_extension_bits);
    write_register(values, reg, raw, condition(values, true_condition));
    return;
  }
  if (op.type == index_modify_type) {
    modify_instruction(values, op);
    return;
  }
  const auto before = values;
  const auto &compute = op.compute;
  if (compute.kind == ComputeKind::none) {
    if (op.type == register_move_type) { register_instruction(values, op, before); }
    return;
  }
  auto *selected =
      condition(values, static_cast<unsigned>((op.word >> condition_shift) & true_condition));
  if (op.type == computed_branch_type) {
    if ((op.word & else_compute) != 0) {
      selected =
          builder_.CreateAnd(builder_.CreateNot(selected), condition(values, true_condition));
    } else if ((context_ & simd_mode) == 0) {
      selected = condition(values, true_condition);
    }
  }
  Arithmetic result;
  if (compute.kind == ComputeKind::dual_alu) {
    result = arithmetic(values, compute, alu::add);
    const auto difference = arithmetic(values, compute, alu::subtract);
    result.status = builder_.CreateOr(result.status, difference.status);
    result.sticky = builder_.CreateOr(result.sticky, difference.sticky);
    const auto reg = compute.outputs[1];
    values[reg] = builder_.CreateSelect(selected, difference.raw, values[reg]);
    written_.insert(reg);
  } else if (compute.kind == ComputeKind::shifter || compute.kind == ComputeKind::shift_immediate) {
    result = rotation(values, compute);
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
  if (op.type == register_move_type) { register_instruction(values, op, before); }
}

void Lowering::memory_failure(const Values &values,
                              const Instruction &op,
                              llvm::Value *address,
                              NativeExit reason,
                              bool partial) {
  for (const auto [slot, value] :
       {std::pair{memory_address, builder_.CreateZExt(address, builder_.getInt64Ty())},
        std::pair{memory_space,
                  static_cast<llvm::Value *>(
                      builder_.getInt64(static_cast<unsigned>(memory_move(op).space)))},
        std::pair{fault_instruction, static_cast<llvm::Value *>(builder_.getInt64(op.word))}}) {
    builder_.CreateStore(
        value,
        builder_.CreateGEP(builder_.getInt64Ty(), function_->getArg(1), builder_.getInt64(slot)));
  }
  auto prefix = values;
  if (!partial) {
    auto *count = builder_.CreateSub(
        values[credit_value], builder_.getInt64(costs_[addresses_.at(op.address)].instructions));
    credit(prefix, count, builder_.CreateAdd(count, values[extra_cycles_value]), false);
  }
  exit(prefix, builder_.getInt32(op.address), reason);
}

void Lowering::modify_instruction(Values &values, const Instruction &op) {
  constexpr unsigned first_sticky_bit = 17;
  constexpr unsigned first_interrupt_bit = 20;
  const auto modify = index_modify(op);
  auto *length = values[length_value + modify.source];
  auto *base = values[base_value + modify.source];
  const auto immediate = static_cast<std::uint32_t>(op.word);
  const bool negative = (immediate >> 31) != 0;
  auto *sum = builder_.CreateAdd(values[index_value + modify.source], builder_.getInt32(immediate));
  auto *wrapped = negative ? builder_.CreateICmpULT(sum, base)
                           : builder_.CreateICmpUGE(sum, builder_.CreateAdd(base, length));
  auto *circular = builder_.CreateICmpNE(length, builder_.getInt32(0));
  auto *adjusted = negative ? builder_.CreateAdd(sum, length) : builder_.CreateSub(sum, length);
  auto *result = builder_.CreateSelect(builder_.CreateAnd(circular, wrapped), adjusted, sum);
  if (modify.reverse) {
    result = builder_.CreateUnaryIntrinsic(llvm::Intrinsic::bitreverse, result);
  }
  values[index_value + modify.destination] = result;
  if ((modify.source & 7) == 7) {
    const auto dag = modify.source / 8;
    auto *enabled = (context_ & circular_mode) != 0 ? builder_.getTrue() : circular;
    auto *overflow = builder_.CreateAnd(enabled, wrapped);
    auto *sticky = builder_.CreateExtractElement(values[sticky_value], builder_.getInt32(0));
    sticky = builder_.CreateOr(
        sticky,
        builder_.CreateSelect(
            overflow, builder_.getInt32(1U << (first_sticky_bit + dag)), builder_.getInt32(0)));
    values[sticky_value] =
        builder_.CreateInsertElement(values[sticky_value], sticky, builder_.getInt32(0));
    values[interrupt_latch_value] = builder_.CreateOr(
        values[interrupt_latch_value],
        builder_.CreateSelect(
            overflow, builder_.getInt32(1U << (first_interrupt_bit + dag)), builder_.getInt32(0)));
  }
}

llvm::Value *
Lowering::planned_pointer(const Instruction &op, llvm::Value *address, unsigned transfer) {
  const auto &plan = memory_plan_.at(op.address);
  if (plan.stride == 0) { return plan.pointers[transfer]; }
  auto *offset = builder_.CreateMul(builder_.CreateZExt(address, builder_.getInt64Ty()),
                                    builder_.getInt64(plan.stride));
  return builder_.CreateGEP(builder_.getInt8Ty(), plan.pointers[0], offset);
}

bool Lowering::plan_changing_memory(const Instruction &op) {
  const auto move = memory_move(op);
  auto &plan = memory_plan_[op.address];
  plan.pointers.fill(llvm::PoisonValue::get(builder_.getPtrTy()));
  for (std::size_t i = 0; i < variant_.layout.regions.size(); ++i) {
    const auto &layout = variant_.layout.regions[i];
    if (layout.space != move.space || layout.count != (std::uint64_t{1} << 32) ||
        layout.view != MemoryView::normal_word || !layout.initialized ||
        (!host_protected(layout) && (!(move.write ? layout.writable : layout.readable) ||
                                     (move.write && aliases_code(variant_, i))))) {
      continue;
    }
    plan.pointers.fill(memory_binding(i, move.write));
    plan.stride = layout.byte_stride;
    return true;
  }
  return false;
}

llvm::Value *Lowering::memory_pointer(const Values &values,
                                      const Instruction &op,
                                      llvm::Value *address,
                                      bool partial) {
  const auto move = memory_move(op);
  auto *done = llvm::BasicBlock::Create(module_.getContext(), "memory.address", function_);
  llvm::IRBuilder<> join(done);
  auto *result = join.CreatePHI(builder_.getPtrTy(), 2);
  const auto &regions = variant_.layout.regions;
  for (std::size_t i = 0; i < regions.size(); ++i) {
    const auto &layout = regions[i];
    if (layout.space != move.space || layout.view == MemoryView::instruction || layout.count == 0) {
      continue;
    }
    auto *hit = llvm::BasicBlock::Create(module_.getContext(), "memory.region", function_);
    auto *next = llvm::BasicBlock::Create(module_.getContext(), "memory.next", function_);
    auto *index = builder_.CreateSub(address, builder_.getInt32(layout.first));
    auto *inside = layout.count == (std::uint64_t{1} << 32)
                       ? static_cast<llvm::Value *>(builder_.getTrue())
                       : builder_.CreateICmpULT(
                             index, builder_.getInt32(static_cast<std::uint32_t>(layout.count)));
    builder_.CreateCondBr(inside, hit, next);
    builder_.SetInsertPoint(hit);
    if (!host_protected(layout) && !(move.write ? layout.writable : layout.readable)) {
      memory_failure(values, op, address, NativeExit::access_fault, partial);
    } else if (!layout.initialized || layout.view != MemoryView::normal_word) {
      memory_failure(values, op, address, NativeExit::unsupported_memory, partial);
    } else if (!host_protected(layout) && move.write && aliases_code(variant_, i)) {
      memory_failure(values, op, address, NativeExit::code_alias, partial);
    } else {
      auto *offset = builder_.CreateMul(builder_.CreateZExt(index, builder_.getInt64Ty()),
                                        builder_.getInt64(layout.byte_stride));
      auto *host = builder_.CreateGEP(builder_.getInt8Ty(), memory_binding(i, move.write), offset);
      result->addIncoming(host, builder_.GetInsertBlock());
      builder_.CreateBr(done);
    }
    builder_.SetInsertPoint(next);
  }
  memory_failure(values, op, address, NativeExit::unsupported_memory, partial);
  builder_.SetInsertPoint(done);
  if (result->getNumIncomingValues() == 0) {
    result->eraseFromParent();
    return llvm::PoisonValue::get(builder_.getPtrTy());
  }
  return result;
}

void Lowering::memory_instruction(Values &values, const Instruction &op, bool partial) {
  const auto move = memory_move(op);
  // Checked regions can share backing with protected Direct views.
  const bool observable =
      !variant_.layout.ordinary_regions ||
      std::ranges::any_of(variant_.layout.regions,
                          [&](const RegionLayout &layout) { return host_protected(layout); });
  const bool broadcast = !move.write && ((move.index == 1 && (context_ & broadcast1_mode) != 0) ||
                                         (move.index == 9 && (context_ & broadcast9_mode) != 0));
  const bool paired = broadcast || (context_ & simd_mode) != 0;
  auto *address = builder_.CreateAdd(values[index_value + move.index],
                                     builder_.getInt32(static_cast<std::uint32_t>(op.word)));
  auto *explicit_pointer =
      partial ? memory_pointer(values, op, address, true) : planned_pointer(op, address, 0);
  llvm::Value *implicit_pointer = explicit_pointer;
  if (paired && !broadcast) {
    implicit_pointer =
        partial
            ? memory_pointer(values, op, builder_.CreateAdd(address, builder_.getInt32(1)), true)
            : planned_pointer(op, builder_.CreateAdd(address, builder_.getInt32(1)), 1);
  }
  if (move.write) {
    for (unsigned transfer = 0; transfer < (paired ? 2U : 1U); ++transfer) {
      const auto lane = move.lane ^ transfer;
      auto *raw = builder_.CreateExtractElement(values[move.reg], builder_.getInt32(lane));
      auto *word = builder_.CreateTrunc(
          builder_.CreateLShr(raw, builder_.getInt64(raw_extension_bits)), builder_.getInt32Ty());
      auto *write = builder_.CreateStore(word, transfer == 0 ? explicit_pointer : implicit_pointer);
      write->setAlignment(llvm::Align(1));
      write->setVolatile(observable);
    }
    return;
  }
  auto *word = builder_.CreateLoad(builder_.getInt32Ty(), explicit_pointer);
  word->setAlignment(llvm::Align(1));
  word->setVolatile(observable);
  auto *raw = builder_.CreateShl(builder_.CreateZExt(word, builder_.getInt64Ty()),
                                 builder_.getInt64(raw_extension_bits));
  values[move.reg] =
      builder_.CreateInsertElement(values[move.reg], raw, builder_.getInt32(move.lane));
  if (paired) {
    llvm::Value *other = word;
    if (!broadcast) {
      auto *read = builder_.CreateLoad(builder_.getInt32Ty(), implicit_pointer);
      read->setAlignment(llvm::Align(1));
      read->setVolatile(observable);
      other = read;
    }
    auto *other_raw = builder_.CreateShl(builder_.CreateZExt(other, builder_.getInt64Ty()),
                                         builder_.getInt64(raw_extension_bits));
    values[move.reg] =
        builder_.CreateInsertElement(values[move.reg], other_raw, builder_.getInt32(move.lane ^ 1));
  }
  written_.insert(move.reg);
  if (move.lane != 0 || paired) { y_written_.insert(move.reg); }
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
    credit(prefix, completed, builder_.CreateAdd(completed, values[extra_cycles_value]), false);
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
}

void Lowering::call(Values &values, const Instruction &op, bool partial) {
  constexpr std::uint32_t call_tag = 1U << 24;
  constexpr unsigned almost_full = Registers::pc_stack_capacity - 1;
  constexpr std::uint32_t stack_overflow_interrupt = 1U << 3;
  auto *depth = values[pc_stack_depth_value];
  if (op.compute.kind == ComputeKind::none) {
    stack_guard(values,
                op,
                builder_.CreateICmpULT(depth, builder_.getInt32(Registers::pc_stack_capacity)),
                partial);
  }
  builder_.CreateStore(builder_.getInt32(call_resume(op) | call_tag), stack_cell(depth));
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
  auto *top = builder_.CreateLoad(builder_.getInt32Ty(), stack_cell(popped_depth));
  stack_guard(values,
              op,
              builder_.CreateICmpEQ(builder_.CreateAnd(top, builder_.getInt32(sequencer_tag)),
                                    builder_.getInt32(0)),
              partial);
  values[pc_stack_depth_value] = popped_depth;
  stack_status(values);
  return builder_.CreateAnd(top, builder_.getInt32(maximum_pc));
}

void Lowering::return_to_caller(Values values, llvm::Value *target, bool partial) {
  auto *unknown = llvm::BasicBlock::Create(module_.getContext(), "cold.return", function_);
  auto *dispatch = builder_.CreateSwitch(target, unknown);
  std::set<std::uint32_t> returns;
  for (const auto &op : instructions_) {
    if (call_branch_instruction(op)) { returns.insert(call_resume(op)); }
  }
  for (const auto address : returns) {
    const auto found = addresses_.find(address);
    if (found == addresses_.end()) { continue; }
    auto &destination = partial ? partial_[found->second] : admission_[found->second];
    destination.incoming(values, builder_.GetInsertBlock());
    dispatch->addCase(builder_.getInt32(address), destination.block);
  }
  builder_.SetInsertPoint(unknown);
  exit(values, target, NativeExit::returned);
}

void Lowering::credit(Values &values, llvm::Value *count, llvm::Value *cycles, bool partial) {
  values[instruction_remaining_value] =
      builder_.CreateSub(values[instruction_remaining_value], count);
  auto *available = values[cycle_remaining_value];
  auto *remaining = builder_.CreateSub(available, cycles);
  if (partial) {
    remaining = builder_.CreateBinaryIntrinsic(llvm::Intrinsic::usub_sat, available, cycles);
    values[cycle_overrun_value] =
        builder_.CreateBinaryIntrinsic(llvm::Intrinsic::usub_sat, cycles, available);
  }
  values[cycle_remaining_value] = remaining;
  values[extra_cycles_value] = builder_.getInt64(0);
}

llvm::Value *Lowering::branch_taken(llvm::Value *lanes) {
  auto *taken = builder_.CreateExtractElement(lanes, builder_.getInt32(0));
  if ((context_ & simd_mode) != 0) {
    taken = builder_.CreateAnd(taken, builder_.CreateExtractElement(lanes, builder_.getInt32(1)));
  }
  return taken;
}

llvm::Value *Lowering::branch_lanes(const Values &values, const Instruction &op, bool partial) {
  if (!branch(op)) { return nullptr; }
  auto *lanes =
      condition(values, static_cast<unsigned>((op.word >> condition_shift) & true_condition));
  if (call_branch_instruction(op) && op.compute.kind != ComputeKind::none) {
    auto *room = builder_.CreateICmpULT(values[pc_stack_depth_value],
                                        builder_.getInt32(Registers::pc_stack_capacity));
    stack_guard(
        values, op, builder_.CreateOr(builder_.CreateNot(branch_taken(lanes)), room), partial);
  }
  return lanes;
}

void Lowering::complete(Values values,
                        const Instruction &op,
                        bool partial,
                        llvm::Value *lanes,
                        PipelineEffects pipeline) {
  if (!partial && loop_ends_.contains(addresses_.at(op.address))) {
    complete_loop(values, op);
    return;
  }
  auto *count = partial ? builder_.getInt64(1) : values[credit_value];
  const auto next = next_pc(op);
  if (!branch(op)) {
    delayed_slot(values, op, partial);
    credit(values, count, builder_.CreateAdd(count, values[extra_cycles_value]), partial);
    finish(values, next, partial);
    return;
  }
  auto *cycles =
      builder_.CreateAdd(builder_.CreateAdd(count, values[extra_cycles_value]), pipeline.stall);
  auto *taken = branch_taken(lanes);
  auto *branch = llvm::BasicBlock::Create(module_.getContext(), "jump", function_);
  auto *fallthrough = llvm::BasicBlock::Create(module_.getContext(), "fallthrough", function_);
  builder_.CreateCondBr(taken, branch, fallthrough);
  builder_.SetInsertPoint(branch);
  auto branch_values = values;
  if (call_branch_instruction(op)) { call(branch_values, op, partial); }
  auto *target = op.type == return_branch_type ? pop(branch_values, op, partial) : nullptr;
  if (indirect_branch(op)) {
    auto *address = builder_.CreateAdd(values[index_value + branch_index(op)],
                                       values[modifier_value + branch_modifier(op)]);
    target = builder_.CreateAnd(address, builder_.getInt32(maximum_pc));
  }
  retire_pipeline(branch_values, pipeline);
  if (delayed(op)) {
    if (target == nullptr) { target = builder_.getInt32(branch_target(op)); }
    store(builder_.CreateOr(target, builder_.getInt32(before_first_delay_slot)),
          delayed_target_slot);
    branch_values[hazard_value] = builder_.getInt1(op.compute.kind != ComputeKind::none);
    credit(branch_values, count, cycles, partial);
    finish(branch_values, next, partial);
  } else {
    branch_values[hazard_value] = builder_.getFalse();
    branch_values[dag_history_value] = builder_.getInt32(0);
    credit(branch_values,
           count,
           builder_.CreateAdd(cycles, builder_.getInt64(branch_penalty)),
           partial);
    redirect(branch_values, op, target, partial);
  }
  builder_.SetInsertPoint(fallthrough);
  retire_pipeline(values, pipeline);
  values[hazard_value] = builder_.getInt1(op.compute.kind != ComputeKind::none);
  credit(values, count, cycles, partial);
  finish(values, next, partial);
}

void Lowering::redirect(Values values, const Instruction &op, llvm::Value *target, bool partial) {
  if (op.type == return_branch_type) {
    return_to_caller(values, target, partial);
  } else if (indirect_branch(op)) {
    exit(values, target, NativeExit::indirect);
  } else {
    finish(values, branch_target(op), partial);
  }
}

void Lowering::delayed_slot(const Values &values, const Instruction &op, bool partial) {
  if (!delay_slots_.contains(op.address)) { return; }
  auto *pending = load(32, delayed_target_slot);
  auto *slot = llvm::BasicBlock::Create(module_.getContext(), "delay.slot", function_);
  auto *ordinary = llvm::BasicBlock::Create(module_.getContext(), "delay.none", function_);
  builder_.CreateCondBr(builder_.CreateICmpNE(pending, builder_.getInt32(0)), slot, ordinary);
  builder_.SetInsertPoint(slot);
  auto retired = values;
  if (branch(op)) {
    // An adjacent complementary delayed JUMP is suppressed on the taken path.
    retired[hazard_value] = builder_.getFalse();
    retire_pipeline(retired, {.stall = builder_.getInt64(0), .tag = builder_.getInt32(0)});
  }
  auto *target = builder_.CreateAnd(pending, builder_.getInt32(maximum_pc));
  const auto origin = delay_ends_.find(op.address);
  llvm::BasicBlock *last = nullptr;
  if (origin != delay_ends_.end()) {
    last = llvm::BasicBlock::Create(module_.getContext(), "delay.last", function_);
    auto *first = llvm::BasicBlock::Create(module_.getContext(), "delay.first", function_);
    auto *phase = builder_.CreateAnd(pending, builder_.getInt32(before_second_delay_slot));
    builder_.CreateCondBr(builder_.CreateICmpNE(phase, builder_.getInt32(0)), last, first);
    builder_.SetInsertPoint(first);
  }
  auto first_values = retired;
  store(builder_.CreateOr(target, builder_.getInt32(before_second_delay_slot)),
        delayed_target_slot);
  credit(first_values,
         builder_.getInt64(1),
         builder_.CreateAdd(builder_.getInt64(1), retired[extra_cycles_value]),
         partial);
  finish(first_values, next_pc(op), partial);
  if (last != nullptr) {
    builder_.SetInsertPoint(last);
    store(builder_.getInt32(0), delayed_target_slot);
    retired[hazard_value] = builder_.getFalse();
    // One refill cycle follows the second slot. It also ages the DAG dependency.
    retire_pipeline(retired, {.stall = builder_.getInt64(0), .tag = builder_.getInt32(0)});
    credit(retired,
           builder_.getInt64(1),
           builder_.CreateAdd(builder_.getInt64(2), retired[extra_cycles_value]),
           partial);
    redirect(retired, *origin->second, target, partial);
  }
  builder_.SetInsertPoint(ordinary);
}

bool Lowering::delay_boundary(std::uint32_t pc) const {
  return delay_slots_.contains(pc) || delay_ends_.contains((pc + delay_slot_count) & maximum_pc);
}

void Lowering::finish(Values values, std::uint32_t next, bool partial) {
  const auto target = addresses_.find(next);
  if (target != addresses_.end()) {
    const auto i = target->second;
    if (delay_boundary(next)) {
      jump(partial_[i], values);
    } else if (partial && loop_headers_.contains(i)) {
      partial_[i].incoming(values, builder_.GetInsertBlock());
      admission_[i].incoming(values, builder_.GetInsertBlock());
      builder_.CreateCondBr(values[tail_value], partial_[i].block, admission_[i].block);
    } else {
      jump(partial ? partial_[i] : fast_entry_[i], values);
    }
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
    if (reg < pinned_registers) { continue; }
    store(builder_.CreateExtractElement(values[reg], builder_.getInt32(0)), data_slot(0, reg));
    if ((context_ & simd_mode) != 0 || y_written_.contains(reg)) {
      store(builder_.CreateExtractElement(values[reg], builder_.getInt32(1)), data_slot(1, reg));
    }
  }
  for (const auto index : dag_written_) {
    const auto bank = (context_ & dag_bank_mode(index & 15)) != 0 ? 1U : 0U;
    store(values[index_value + index], dag_slot(bank, index & 15, index / 16));
  }
  if (stack_written_) {
    store(builder_.CreateTrunc(values[pc_stack_depth_value], builder_.getInt8Ty()),
          stack_depth_slot);
  }
  if (stack_written_ || dag_status_written_) {
    store(values[interrupt_latch_value], interrupt_latch_slot);
  }
  std::vector<llvm::Value *> args{
      function_->getArg(unit_pointers),
      function_->getArg(unit_controls),
      values[instruction_remaining_value],
      values[cycle_remaining_value],
      values[cycle_overrun_value],
      exit_pc_,
      builder_.getInt32(entry_address_),
      function_->getArg(unit_mode),
      builder_.CreateOr(builder_.CreateZExt(values[hazard_value], builder_.getInt32Ty()),
                        builder_.CreateShl(values[dag_history_value], builder_.getInt32(1))),
      exit_reason_};
  for (unsigned reg = 0; reg < pinned_registers; ++reg) { args.push_back(values[reg]); }
  args.push_back(values[status_value]);
  args.push_back(values[sticky_value]);
  abi_.leave(builder_, unit_, args);
}

std::vector<BlockCost> Lowering::block_costs() const {
  std::set<std::uint32_t> leaders(variant_.entries.begin(), variant_.entries.end());
  leaders.insert(delay_slots_.begin(), delay_slots_.end());
  for (const auto &vector : variant_.interrupt_vectors) { leaders.insert(vector.vector); }
  std::vector<BlockCost> result(instructions_.size());
  for (const auto &op : instructions_) {
    if ((op.type == direct_branch_type || op.type == computed_branch_type) &&
        !indirect_branch(op)) {
      leaders.insert(branch_target(op));
    }
  }
  for (std::size_t i = instructions_.size(); i != 0; --i) {
    const auto index = i - 1;
    const auto &op = instructions_[index];
    if (delay_boundary(op.address)) {
      leaders.insert(op.address);
      continue;
    }
    if (branch(op)) {
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

void Lowering::load_dag(unsigned reg) {
  if (dag_[reg] != nullptr) { return; }
  const auto bank = (context_ & dag_bank_mode(reg & 15)) != 0 ? 1U : 0U;
  dag_[reg] = load(32, dag_slot(bank, reg & 15, reg / 16));
}

void Lowering::load_indices() {
  for (const auto &op : instructions_) {
    const auto destination = loaded_register(op);
    if (dag_register(destination)) { dag_written_.insert(dag_number(destination)); }
    if (op.type == register_move_type) {
      const auto move = register_move(op);
      for (const auto reg : {move.source, move.destination}) {
        if (dag_register(reg)) { load_dag(dag_number(reg)); }
      }
    }
    if (op.type == immediate_move_type && dag_register(destination)) {
      load_dag(dag_number(destination));
    }
    if (indirect_branch(op)) {
      load_dag(branch_index(op));
      load_dag(16 + branch_modifier(op));
    }
    if (op.type == memory_move_type) { load_dag(memory_move(op).index); }
    if (op.type == index_modify_type) {
      const auto modify = index_modify(op);
      dag_written_.insert(modify.destination);
      for (const auto reg :
           {modify.source, modify.destination, 32 + modify.source, 48 + modify.source}) {
        load_dag(reg);
      }
    }
  }
}

void Lowering::plan_loops() {
  for (std::size_t end = 0; end < instructions_.size(); ++end) {
    const auto &op = instructions_[end];
    if (op.type != direct_branch_type || (op.word & call_branch) != 0 ||
        delay_boundary(op.address)) {
      continue;
    }
    const auto header = addresses_.find(branch_target(op));
    if (header == addresses_.end() || header->second > end) { continue; }
    const auto start = header->second;
    const auto count = end - start + 1;
    const bool conditional = ((op.word >> condition_shift) & true_condition) != true_condition;
    if ((count == 1 && conditional) || costs_[start].instructions != count) { continue; }
    bool linear = true;
    for (std::size_t i = start; i < end; ++i) {
      linear = linear && instructions_[i].address + 1 == instructions_[i + 1].address &&
               !branch(instructions_[i]) && !dag_register(loaded_register(instructions_[i]));
    }
    if (!linear) { continue; }
    const bool stall = conditional && instructions_[end - 1].compute.kind != ComputeKind::none;
    loop_headers_.emplace(start, end);
    loop_ends_.emplace(
        end,
        BlockCost{.instructions = count, .cycles = count + branch_penalty + (stall ? 1U : 0U)});
  }
}

void Lowering::complete_loop(Values values, const Instruction &op) {
  values[extra_cycles_value] = builder_.getInt64(0);
  const auto end = addresses_.at(op.address);
  const auto start = addresses_.at(branch_target(op));
  const auto cost = loop_ends_.at(end);
  auto *lanes =
      condition(values, static_cast<unsigned>((op.word >> condition_shift) & true_condition));
  auto *taken = builder_.CreateExtractElement(lanes, builder_.getInt32(0));
  if ((context_ & simd_mode) != 0) {
    taken = builder_.CreateAnd(taken, builder_.CreateExtractElement(lanes, builder_.getInt32(1)));
  }
  values[hazard_value] = builder_.getFalse();
  auto *remaining = builder_.CreateSub(values[loop_remaining_value], builder_.getInt64(1));
  values[loop_remaining_value] = remaining;
  auto *account = llvm::BasicBlock::Create(module_.getContext(), "grant.account", function_);
  auto *again = builder_.CreateAnd(taken, builder_.CreateICmpNE(remaining, builder_.getInt64(0)));
  fast_[start].incoming(values, builder_.GetInsertBlock());
  builder_.CreateCondBr(again, fast_[start].block, account);
  builder_.SetInsertPoint(account);
  auto *turns = builder_.CreateSub(values[loop_grant_value], remaining);
  auto *count = builder_.CreateMul(turns, builder_.getInt64(cost.instructions));
  auto *cycles = builder_.CreateSub(
      builder_.CreateMul(turns, builder_.getInt64(cost.cycles)),
      builder_.CreateSelect(taken, builder_.getInt64(0), builder_.getInt64(branch_penalty)));
  credit(values, count, cycles, false);
  values[loop_grant_value] = builder_.getInt64(0);
  values[loop_remaining_value] = builder_.getInt64(0);
  auto *tail = llvm::BasicBlock::Create(module_.getContext(), "cold.loop.tail", function_);
  auto *finished = llvm::BasicBlock::Create(module_.getContext(), "loop.finished", function_);
  builder_.CreateCondBr(taken, tail, finished);
  builder_.SetInsertPoint(tail);
  auto prefix = values;
  prefix[tail_value] = builder_.getTrue();
  jump(partial_[start], prefix);
  builder_.SetInsertPoint(finished);
  const auto next = (op.address + 1) & maximum_pc;
  const auto successor = addresses_.find(next);
  if (successor == addresses_.end()) {
    finish(values, next, false);
  } else {
    jump(admission_[successor->second], values);
  }
}

std::pair<llvm::Value *, llvm::Value *> Lowering::plan_pointer(const MemoryMove &move,
                                                               llvm::Value *address) {
  llvm::Value *valid = builder_.getFalse();
  llvm::Value *host = llvm::PoisonValue::get(builder_.getPtrTy());
  for (std::size_t i = 0; i < variant_.layout.regions.size(); ++i) {
    const auto &layout = variant_.layout.regions[i];
    if (layout.space != move.space || layout.count == 0 || layout.view != MemoryView::normal_word ||
        !layout.initialized ||
        (!host_protected(layout) && (!(move.write ? layout.writable : layout.readable) ||
                                     (move.write && aliases_code(variant_, i))))) {
      continue;
    }
    auto *index = builder_.CreateSub(address, builder_.getInt32(layout.first));
    auto *inside = layout.count == (std::uint64_t{1} << 32)
                       ? static_cast<llvm::Value *>(builder_.getTrue())
                       : builder_.CreateICmpULT(
                             index, builder_.getInt32(static_cast<std::uint32_t>(layout.count)));
    auto *offset = builder_.CreateMul(builder_.CreateZExt(index, builder_.getInt64Ty()),
                                      builder_.getInt64(layout.byte_stride));
    auto *candidate =
        builder_.CreateGEP(builder_.getInt8Ty(), memory_binding(i, move.write), offset);
    host = builder_.CreateSelect(inside, candidate, host);
    valid = builder_.CreateOr(valid, inside);
  }
  return {host, valid};
}

void Lowering::plan_memory() {
  memory_admitted_ = builder_.getTrue();
  for (const auto &op : instructions_) {
    if (op.type != memory_move_type) { continue; }
    const auto move = memory_move(op);
    if (dag_written_.contains(move.index)) {
      if (!plan_changing_memory(op)) { memory_admitted_ = builder_.getFalse(); }
      continue;
    }
    const bool broadcast = !move.write && ((move.index == 1 && (context_ & broadcast1_mode) != 0) ||
                                           (move.index == 9 && (context_ & broadcast9_mode) != 0));
    const unsigned transfers = !broadcast && (context_ & simd_mode) != 0 ? 2U : 1U;
    auto &plan = memory_plan_[op.address];
    for (unsigned transfer = 0; transfer < transfers; ++transfer) {
      auto *address = builder_.CreateAdd(
          dag_[move.index], builder_.getInt32(static_cast<std::uint32_t>(op.word) + transfer));
      const auto [host, valid] = plan_pointer(move, address);
      plan.pointers[transfer] = host;
      memory_admitted_ = builder_.CreateAnd(memory_admitted_, valid);
    }
    if (transfers == 1) { plan.pointers[1] = plan.pointers[0]; }
  }
}

std::set<std::uint32_t> grant_successors(const Instruction &op) {
  const bool is_branch = branch(op);
  std::set<std::uint32_t> next;
  if ((op.type == direct_branch_type || op.type == computed_branch_type) && !indirect_branch(op)) {
    next.insert(branch_target(op));
  }
  // A taken return ends this grant. Its checked continuation admits a new one.
  if (!is_branch || ((op.word >> condition_shift) & true_condition) != true_condition) {
    next.insert((op.address + 1) & maximum_pc);
  }
  return next;
}

std::vector<std::optional<BlockCost>> Lowering::unit_costs() const {
  std::vector<std::optional<BlockCost>> result(instructions_.size());
  std::vector<unsigned> visited(instructions_.size());
  const std::function<std::optional<BlockCost>(std::size_t)> visit =
      [&](std::size_t i) -> std::optional<BlockCost> {
    if (visited[i] != 0) { return visited[i] == 2 ? result[i] : std::nullopt; }
    visited[i] = 1;
    const auto &op = instructions_[i];
    if (delay_boundary(op.address)) { return std::nullopt; }
    const bool is_branch = branch(op);
    const auto next = grant_successors(op);
    BlockCost suffix{.instructions = 0, .cycles = 0};
    for (const auto address : next) {
      const auto found = addresses_.find(address);
      if (found == addresses_.end() || delay_boundary(address)) { continue; }
      const auto cost = visit(found->second);
      if (!cost.has_value()) {
        visited[i] = 2;
        return std::nullopt;
      }
      suffix.instructions = std::max(suffix.instructions, cost->instructions);
      suffix.cycles = std::max(suffix.cycles, cost->cycles);
    }
    ++suffix.instructions;
    ++suffix.cycles;
    if (op.type == memory_move_type || op.type == index_modify_type || indirect_branch(op)) {
      suffix.cycles += 2;
    }
    if (is_branch) {
      suffix.cycles += branch_penalty;
      if (((op.word >> condition_shift) & true_condition) != true_condition) { ++suffix.cycles; }
    }
    visited[i] = 2;
    result[i] = suffix;
    return suffix;
  };
  for (std::size_t i = 0; i < instructions_.size(); ++i) { (void)visit(i); }
  return result;
}

std::vector<bool> Lowering::dead_alu_status() const {
  std::vector<bool> dead(instructions_.size());
  bool overwritten = false;
  for (auto end = instructions_.size(); end != 0; --end) {
    const auto i = end - 1;
    const auto &op = instructions_[i];
    if (costs_[i].instructions == 1) { overwritten = false; }
    if (op.type == conditional_compute_type && op.compute.kind == ComputeKind::alu &&
        ((op.word >> condition_shift) & true_condition) == true_condition) {
      const auto opcode = op.compute.opcode;
      // Compare history outlives the ordinary ALU flags.
      dead[i] = overwritten && opcode != alu::compare && opcode != alu::compare_unsigned;
      overwritten = opcode != alu::add_carry && opcode != alu::subtract_carry &&
                    opcode != alu::add_carry_only && opcode != alu::subtract_borrow;
    } else if (op.type != nop_type && op.type != immediate_move_type &&
               op.type != index_modify_type && op.type != memory_move_type) {
      overwritten = false;
    }
  }
  return dead;
}

void Lowering::emit_instruction(std::size_t i,
                                const std::optional<BlockCost> &grant,
                                bool dead_status) {
  const auto &op = instructions_[i];
  builder_.SetCurrentDebugLocation(
      llvm::DILocation::get(module_.getContext(), op.address + 1, 0, function_->getSubprogram()));
  builder_.SetInsertPoint(admission_[i].block);
  auto values = admission_[i].values();
  llvm::Value *admitted = builder_.getFalse();
  const auto loop = loop_headers_.find(i);
  if (loop != loop_headers_.end()) {
    const auto cost = loop_ends_.at(loop->second);
    auto *instructions = builder_.CreateUDiv(values[instruction_remaining_value],
                                             builder_.getInt64(cost.instructions));
    auto *cycles =
        builder_.CreateUDiv(values[cycle_remaining_value], builder_.getInt64(cost.cycles));
    auto *turns =
        builder_.CreateSelect(builder_.CreateICmpULT(instructions, cycles), instructions, cycles);
    values[loop_grant_value] = turns;
    values[loop_remaining_value] = turns;
    admitted = builder_.CreateAnd(
        memory_admitted_,
        builder_.CreateAnd(builder_.CreateICmpEQ(values[dag_history_value], builder_.getInt32(0)),
                           builder_.CreateICmpNE(turns, builder_.getInt64(0))));
  } else if (grant.has_value()) {
    admitted = builder_.CreateAnd(
        memory_admitted_,
        builder_.CreateAnd(builder_.CreateICmpUGE(values[instruction_remaining_value],
                                                  builder_.getInt64(grant->instructions)),
                           builder_.CreateICmpUGE(values[cycle_remaining_value],
                                                  builder_.getInt64(grant->cycles))));
  }
  auto hot = values;
  // A fixed-cost loop grant proves that no incoming DAG load is pending.
  if (loop != loop_headers_.end()) { hot[dag_history_value] = builder_.getInt32(0); }
  fast_entry_[i].incoming(hot, admission_[i].block);
  auto tail = values;
  tail[tail_value] = builder_.getInt1(loop != loop_headers_.end() || grant.has_value());
  partial_[i].incoming(tail, admission_[i].block);
  builder_.CreateCondBr(
      admitted,
      fast_entry_[i].block,
      partial_[i].block,
      llvm::MDBuilder(module_.getContext()).createBranchWeights(common_weight, 1));
  builder_.SetInsertPoint(fast_entry_[i].block);
  values = fast_entry_[i].values();
  values[credit_value] = builder_.getInt64(costs_[i].instructions);
  values[extra_cycles_value] = builder_.getInt64(0);
  jump(fast_[i], values);
  builder_.SetInsertPoint(fast_[i].block);
  values = fast_[i].values();
  auto *lanes = branch_lanes(values, op, false);
  auto pipeline =
      PipelineEffects{.stall = pipeline_stall(values, op), .tag = loaded_tag(values, op)};
  auto *old_status = values[status_value];
  if (branch(op)) { delayed_slot(values, op, false); }
  instruction(values, op, false);
  // Admitted memory accesses have no resumable exit inside this block.
  if (dead_status) { values[status_value] = old_status; }
  if (!branch(op)) { retire_pipeline(values, pipeline); }
  if (costs_[i].instructions > 1) {
    jump(fast_[i + 1], values);
  } else {
    complete(values, op, false, lanes, pipeline);
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
  if (branch(op)) { delayed_slot(values, op, true); }
  lanes = branch_lanes(values, op, true);
  pipeline = {.stall = pipeline_stall(values, op), .tag = loaded_tag(values, op)};
  instruction(values, op, true);
  if (!branch(op)) { retire_pipeline(values, pipeline); }
  if (costs_[i].instructions > 1) {
    credit(values,
           builder_.getInt64(1),
           builder_.CreateAdd(builder_.getInt64(1), values[extra_cycles_value]),
           true);
    jump(partial_[i + 1], values);
  } else {
    complete(values, op, true, lanes, pipeline);
  }
}

void Lowering::emit() {
  for (const auto &op : instructions_) {
    if (!delayed(op)) { continue; }
    delay_slots_.insert(next_pc(op));
    delay_slots_.insert(next_pc(op, delay_slot_count));
    delay_ends_.emplace(next_pc(op, delay_slot_count), &op);
  }
  stack_written_ = std::ranges::any_of(instructions_, [](const auto &op) {
    return op.type == return_branch_type || call_branch_instruction(op);
  });
  dag_status_written_ = std::ranges::any_of(instructions_, [](const auto &op) {
    return op.type == index_modify_type && (index_modify(op).source & 7) == 7;
  });
  auto *entry = llvm::BasicBlock::Create(module_.getContext(), "entry", function_);
  builder_.SetInsertPoint(entry);
  load_indices();
  exit_ = node("cold.exit");
  llvm::IRBuilder<> exit_builder(exit_.block);
  exit_pc_ = exit_builder.CreatePHI(builder_.getInt32Ty(), 2);
  exit_reason_ = exit_builder.CreatePHI(builder_.getInt64Ty(), 2);
  for (std::size_t i = 0; i < instructions_.size(); ++i) {
    addresses_.emplace(instructions_[i].address, i);
    admission_.push_back(node("admit." + std::to_string(i)));
    fast_entry_.push_back(node("grant.body." + std::to_string(i)));
    fast_.push_back(node("body." + std::to_string(i)));
    partial_.push_back(node("cold.prefix." + std::to_string(i)));
  }
  costs_ = block_costs();
  plan_loops();
  builder_.SetInsertPoint(entry);
  plan_memory();
  const auto grants = unit_costs();
  Values initial{};
  for (unsigned reg = 0; reg < 16; ++reg) {
    initial[reg] = reg < pinned_registers
                       ? function_->getArg(unit_data + reg)
                       : pair(load(64, data_slot(0, reg)), load(64, data_slot(1, reg)), 64);
  }
  initial[status_value] = function_->getArg(unit_status);
  initial[sticky_value] = function_->getArg(unit_sticky);
  initial[instruction_remaining_value] = function_->getArg(unit_instructions);
  initial[cycle_remaining_value] = function_->getArg(unit_cycles);
  initial[cycle_overrun_value] = function_->getArg(unit_cycle_overrun);
  initial[credit_value] = builder_.getInt64(0);
  initial[loop_grant_value] = builder_.getInt64(0);
  initial[loop_remaining_value] = builder_.getInt64(0);
  initial[tail_value] = builder_.getFalse();
  auto *pipeline = function_->getArg(unit_pipeline);
  initial[hazard_value] = builder_.CreateICmpNE(builder_.CreateAnd(pipeline, builder_.getInt32(1)),
                                                builder_.getInt32(0));
  initial[dag_history_value] = builder_.CreateLShr(pipeline, builder_.getInt32(1));
  initial[extra_cycles_value] = builder_.getInt64(0);
  initial[pc_stack_depth_value] = builder_.getInt32(0);
  initial[interrupt_latch_value] = builder_.getInt32(0);
  for (unsigned index = 0; index < dag_.size(); ++index) {
    initial[index_value + index] = dag_[index] != nullptr ? dag_[index] : builder_.getInt32(0);
  }
  if (stack_written_) {
    initial[pc_stack_depth_value] =
        builder_.CreateZExt(load(8, stack_depth_slot), builder_.getInt32Ty());
  }
  if (stack_written_ || dag_status_written_) {
    initial[interrupt_latch_value] = load(32, interrupt_latch_slot);
  }
  auto *pc = function_->getArg(unit_pc);
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
  const auto dead_status = dead_alu_status();
  for (std::size_t i = 0; i < instructions_.size(); ++i) {
    emit_instruction(i, grants[i], dead_status[i]);
  }
  builder_.SetCurrentDebugLocation({});
  emit_exit();
}

std::uint32_t register_context(const Instruction &instruction) {
  std::uint32_t mask = simd_mode;
  const auto source =
      instruction.type == register_move_type ? register_move(instruction).source : 0U;
  const auto destination = loaded_register(instruction);
  for (const auto reg : {source, destination}) {
    if (dag_register(reg)) { mask |= dag_bank_mode(reg & 15); }
    if (data_register(reg)) { mask |= ((reg & 15) < 8 ? low_bank_mode : high_bank_mode); }
  }
  return mask;
}

std::uint32_t instruction_context(const Instruction &instruction) {
  std::uint32_t mask{low_bank_mode};
  if (instruction.type == index_modify_type) {
    const auto modify = index_modify(instruction);
    mask |= dag_bank_mode(modify.source) | dag_bank_mode(modify.destination);
    if ((modify.source & 7) == 7) { mask |= circular_mode; }
  }
  if (instruction.type == memory_move_type) {
    const auto move = memory_move(instruction);
    mask |= dag_bank_mode(move.index) | simd_mode | (move.reg < 8 ? low_bank_mode : high_bank_mode);
    if (!move.write && move.index == 1) { mask |= broadcast1_mode; }
    if (!move.write && move.index == 9) { mask |= broadcast9_mode; }
  }
  if (instruction.type == register_move_type || instruction.type == immediate_move_type) {
    mask |= register_context(instruction);
  }
  if (branch(instruction)) { mask |= simd_mode | saturation_mode; }
  if (indirect_branch(instruction)) {
    mask |= dag_bank_mode(branch_index(instruction)) | dag_bank_mode(branch_modifier(instruction));
  }
  if (instruction.compute.kind == ComputeKind::none) { return mask; }
  mask |= simd_mode | saturation_mode;
  const auto high_register = [](std::uint8_t reg) { return reg >= 8; };
  if (std::ranges::any_of(instruction.compute.inputs, high_register) ||
      std::ranges::any_of(instruction.compute.outputs, high_register)) {
    mask |= high_bank_mode;
  }
  if (instruction.compute.opcode == alu::average) { mask |= truncation_mode; }
  return mask;
}

std::uint32_t context_mask(const std::vector<Instruction> &instructions) {
  std::uint32_t mask{};
  for (const auto &instruction : instructions) { mask |= instruction_context(instruction); }
  return mask;
}

bool complementary_jumps(const Instruction &first, const Instruction &second) {
  constexpr auto complement_bit = 16;
  return delayed(second) && !call_branch_instruction(first) && !call_branch_instruction(second) &&
         first.type != return_branch_type && second.type != return_branch_type &&
         first.compute.kind == ComputeKind::none && second.compute.kind == ComputeKind::none &&
         branch_condition(first) != true_condition && branch_condition(second) != true_condition &&
         (branch_condition(first) ^ branch_condition(second)) == complement_bit;
}

using InstructionMap = std::map<std::uint32_t, const Instruction *>;

void validate_delay_slots(const InstructionMap &code, std::string_view identity) {
  for (const auto &entry : code) {
    const auto &op = *entry.second;
    if (!delayed(op)) { continue; }
    for (unsigned i = 1; i <= delay_slot_count; ++i) {
      const auto slot = code.find(next_pc(op, i));
      if (slot == code.end()) {
        reject(
            op, identity, FaultReason::undeclared_entry, "delayed transfer has an undeclared slot");
      }
      const auto &instruction = *slot->second;
      if (branch(instruction) && (i != 1 || !complementary_jumps(op, instruction))) {
        reject(instruction, identity, FaultReason::invalid_encoding, "branch in a delay slot");
      }
    }
  }
}

std::vector<Instruction> connected_code(const InstructionMap &code, std::uint32_t entry) {
  std::set<std::uint32_t> visited;
  std::vector<std::uint32_t> pending{entry};
  std::set<std::uint32_t> slots;
  while (!pending.empty()) {
    const auto address = pending.back();
    pending.pop_back();
    const auto found = code.find(address);
    if (found == code.end() || !visited.insert(address).second) { continue; }
    const auto &op = *found->second;
    if (delayed(op)) {
      slots.insert(next_pc(op));
      slots.insert(next_pc(op, delay_slot_count));
      if (call_branch_instruction(op)) { pending.push_back(call_resume(op)); }
    }
    if (op.type == direct_branch_type || op.type == computed_branch_type) {
      if (!indirect_branch(op)) { pending.push_back(branch_target(op)); }
      if ((!call_branch_instruction(op) || delayed(op)) && branch_condition(op) == true_condition) {
        continue;
      }
    }
    if (op.type == return_branch_type && branch_condition(op) == true_condition) { continue; }
    pending.push_back(next_pc(op));
  }
  visited.merge(slots);
  std::vector<Instruction> result;
  result.reserve(visited.size());
  for (const auto address : visited) { result.push_back(*code.at(address)); }
  return result;
}

void collect_continuations(std::vector<Continuation> &continuations,
                           const std::vector<Instruction> &connected,
                           const CodeVariant &variant,
                           std::uint32_t entry) {
  for (const auto &op : connected) {
    if (!call_branch_instruction(op)) { continue; }
    const auto next = call_resume(op);
    if (std::ranges::find(connected, next, &Instruction::address) != connected.end()) {
      continuations.push_back({.variant = variant.id, .address = next, .root = entry});
    }
  }
}
} // namespace

std::shared_ptr<const PreparedImage> lower_image(const CodeImage &image,
                                                 const CompileOptions &options) {
  auto result = std::make_shared<PreparedImage>();
  result->identity = input_identity(image, options);
  validate_options(options, result->identity);
  const auto instructions = decode(image, result->identity);
  std::map<std::uint32_t, InstructionMap> variant_code;
  for (const auto &instruction : instructions) {
    validate(instruction, result->identity);
    variant_code[instruction.variant].emplace(instruction.address, &instruction);
  }
  result->code = image;
  result->options = options;
  llvm::LLVMContext context;
  llvm::Module module(result->identity, context);
  module.addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
  llvm::DIBuilder debug(module);
  debug.createCompileUnit(llvm::dwarf::DW_LANG_C_plus_plus,
                          debug.createFile("sharc", ""),
                          "sharc",
                          true,
                          "",
                          0,
                          "",
                          llvm::DICompileUnit::LineTablesOnly);
  std::vector<Continuation> continuations;
  for (const auto &variant : image.variants) {
    validate_memory_layout(variant, result->identity);
    const auto &code = variant_code[variant.id];
    validate_delay_slots(code, result->identity);
    std::set<std::uint32_t> entries(variant.entries.begin(), variant.entries.end());
    for (const auto &request : variant.interrupt_vectors) { entries.insert(request.vector); }
    for (const auto entry : entries) {
      const auto connected = connected_code(code, entry);
      collect_continuations(continuations, connected, variant, entry);
      const auto mask = context_mask(connected);
      std::vector<std::uint32_t> contexts{0};
      for (std::uint32_t bit = 1; bit != 0; bit <<= 1) {
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
        result->exports.push_back(symbol);
        result->units.push_back(std::move(unit));
      }
    }
  }
  const NativeAbi abi(module, result->units, continuations);
  for (const auto &unit : result->units) {
    const auto variant = std::ranges::find(image.variants, unit.variant, &CodeVariant::id);
    Lowering(module,
             debug,
             options.memory_mode,
             abi,
             *variant,
             connected_code(variant_code.at(unit.variant), unit.entry),
             unit)
        .emit();
  }
  finalize_debug(module, debug);
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
