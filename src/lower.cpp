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
constexpr unsigned counted_loop_type = 12;
constexpr unsigned conditional_loop_type = 13;
constexpr unsigned memory_move_type = 15;
constexpr unsigned index_modify_type = 19;
constexpr std::uint64_t forced_long = std::uint64_t{1} << 39;
constexpr std::uint64_t relative_branch = std::uint64_t{1} << 40;
constexpr std::uint64_t call_branch = std::uint64_t{1} << 39;
constexpr std::uint64_t abort_branch = std::uint64_t{1} << 38;
constexpr std::uint64_t delayed_branch = std::uint64_t{1} << 26;
constexpr std::uint64_t else_compute = std::uint64_t{1} << 25;
constexpr std::uint64_t clear_interrupt_branch = std::uint64_t{1} << 24;
constexpr std::uint64_t loop_reentry = std::uint64_t{1} << 24;
// Programming Reference 2.4, 4-19: three pipeline bubbles on a taken jump.
constexpr unsigned branch_penalty = 3;
constexpr unsigned delay_slot_count = 2;
constexpr std::uint32_t true_condition = 31;
constexpr unsigned condition_shift = 33;
constexpr unsigned immediate_register_shift = 32;
constexpr unsigned raw_extension_bits = 8;
constexpr unsigned loop_opcode_shift = 40;
constexpr unsigned immediate_count_shift = 24;
constexpr unsigned loop_count_register_opcode = 0x0d;
constexpr unsigned loop_counter_expired = 15;
constexpr unsigned loop_type_shift = 29;
constexpr unsigned long_counted_loop_kind = 7;
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

bool loop_abort(const Instruction &op) {
  return (op.type == direct_branch_type || op.type == computed_branch_type) &&
         (op.word & abort_branch) != 0;
}

bool indirect_branch(const Instruction &op) {
  return op.type == computed_branch_type && (op.word & relative_branch) == 0;
}

unsigned branch_condition(const Instruction &op) {
  return static_cast<unsigned>((op.word >> condition_shift) & true_condition);
}

bool observes_not_lce(const Instruction &op) {
  return branch_condition(op) == loop_counter_expired &&
         (branch(op) || op.type == conditional_compute_type || op.type == immediate_shift_type ||
          op.type == register_move_type);
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

bool loop_setup(const Instruction &op) {
  return op.type == counted_loop_type || op.type == conditional_loop_type;
}

struct LoopShape {
  std::uint32_t start{}, end{};
  unsigned length{}, term{};
  bool has_reentry{};
  bool has_tail_abort{};

  [[nodiscard]] bool counted() const { return term == loop_counter_expired; }

  [[nodiscard]] bool arithmetic() const { return !counted() && term != true_condition; }

  [[nodiscard]] bool short_body() const {
    return length < loop_fetch_distance || (counted() && length == loop_fetch_distance);
  }

  [[nodiscard]] std::uint32_t condition_pc() const {
    if (length == 1 || length == 2) { return end; }
    return (end - loop_decode_distance) & maximum_pc;
  }

  [[nodiscard]] unsigned short_condition_tail() const { return length == 1 ? 3U : length; }

  [[nodiscard]] bool contains(std::uint32_t pc) const {
    return ((pc - start) & maximum_pc) < length;
  }

  [[nodiscard]] unsigned short_threshold() const {
    constexpr std::array thresholds{4U, 2U, 2U, 1U};
    return thresholds[length - 1];
  }

  [[nodiscard]] std::uint32_t address_word() const {
    const auto counted_type = length < loop_fetch_distance ? length * 2U : long_counted_loop_kind;
    const auto type = counted() ? counted_type : static_cast<unsigned>(length == 1);
    return end | (term << immediate_count_shift) | (type << loop_type_shift);
  }
};

using LoopMap = std::map<std::uint32_t, LoopShape>;

constexpr std::uint64_t return_loop_fetch = 1ULL << 50;
constexpr std::uint64_t return_loop_condition = 1ULL << 51;
constexpr unsigned counted_tail_index_shift = 52;
constexpr std::uint64_t counted_tail_index_mask = 7ULL << counted_tail_index_shift;
// A four-word loop can have at most 2^34-1 words left after a CALL.
constexpr unsigned return_short_left_shift = 24;
constexpr std::uint64_t return_short_left_mask = (unsigned_limit * loop_fetch_distance) - 1;
constexpr std::uint64_t return_short_loop = 1ULL << 58;
constexpr std::uint64_t return_short_compact = 1ULL << 59;
constexpr std::uint64_t return_short_lce = 1ULL << 60;
constexpr std::uint64_t return_loop_ready = 1ULL << 62;
// The counted tail shares its word with two bits of termination history.
constexpr unsigned lce_age_shift = 56;
constexpr unsigned lce_compute_age = 2;
constexpr unsigned lce_branch_age = 3;
constexpr auto lce_age_mask = std::uint64_t{lce_branch_age} << lce_age_shift;
constexpr auto lce_before_credit = 1ULL << 61;
constexpr auto lce_clock_mask = lce_age_mask | lce_before_credit;

std::uint64_t loop_return_payload(std::uint32_t call_pc, const LoopShape &shape) {
  if (shape.short_body()) { return return_short_loop | ((call_pc + 1) & maximum_pc); }
  const auto address = (std::uint64_t{shape.end} << loop_refill_end_shift) | shape.start;
  if (call_pc == ((shape.end - loop_fetch_distance) & maximum_pc)) {
    return address | return_loop_fetch;
  }
  if (shape.arithmetic() && call_pc == shape.condition_pc()) {
    return address | return_loop_condition;
  }
  return 0;
}

const LoopShape *loop_return_at(std::uint32_t pc, const LoopMap &calls) {
  const auto call_pc = (pc - 1) & maximum_pc;
  const auto site = calls.find(call_pc);
  if (site != calls.end() && loop_return_payload(call_pc, site->second) != 0) {
    return &site->second;
  }
  // A taken short endpoint CALL returns to the live buffer's header.
  for (unsigned distance = 1; distance < loop_fetch_distance; ++distance) {
    const auto end = (pc + distance) & maximum_pc;
    const auto endpoint = calls.find(end);
    if (endpoint != calls.end() && endpoint->second.short_body() && endpoint->second.start == pc &&
        endpoint->second.end == end) {
      return &endpoint->second;
    }
  }
  return nullptr;
}

std::set<std::uint32_t> call_continuations(std::span<const Instruction> code,
                                           const LoopMap &reentry_calls) {
  std::set<std::uint32_t> continuations;
  for (const auto &op : code) {
    if (!call_branch_instruction(op)) { continue; }
    continuations.insert(call_resume(op));
    const auto site = reentry_calls.find(op.address);
    if (site != reentry_calls.end() && site->second.end == op.address) {
      continuations.insert(site->second.start);
    }
  }
  return continuations;
}

struct LoopPlan {
  LoopMap shapes;
  LoopMap short_nested;
  LoopMap reentry_calls;
  LoopMap aborts;
  LoopMap not_lce;
  std::set<std::uint32_t> lce_endpoints;
  std::set<std::uint32_t> short_parent_tails;
  std::set<std::size_t> lce_slots;
  std::map<std::uint32_t, std::size_t> long_tail_indices;
  std::size_t long_tail_count{};
};

LoopShape loop_shape(const Instruction &op) {
  const auto length = static_cast<std::uint32_t>(op.word) & maximum_pc;
  return {.start = next_pc(op),
          .end = next_pc(op, length),
          .length = length,
          .term = op.type == counted_loop_type ? loop_counter_expired : branch_condition(op)};
}

bool register_count(const Instruction &op) {
  return (op.word >> loop_opcode_shift) == loop_count_register_opcode;
}

unsigned count_register(const Instruction &op) {
  return static_cast<unsigned>(op.word >> immediate_register_shift) & ureg_mask;
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
  if (base >= 9 && base <= 12) {
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "external condition is not lowered");
  }
}

unsigned source_register(const Instruction &op) {
  if (op.type == register_move_type) { return register_move(op).source; }
  if (register_count(op)) { return count_register(op); }
  return 0;
}

void validate_loop(const Instruction &op, std::string_view identity) {
  const auto shape = loop_shape(op);
  constexpr auto negative_offset = 1U << 23;
  if (shape.length == 0 || shape.length >= negative_offset) {
    reject(op, identity, FaultReason::invalid_encoding, "DO endpoint must follow the setup");
  }
  if (register_count(op)) {
    constexpr auto reserved = (1ULL << 39) | (std::uint64_t{byte_mask} << immediate_count_shift);
    if ((op.word & reserved) != 0) {
      reject(op, identity, FaultReason::invalid_encoding, "reserved register-count bits");
    }
    const auto reg = count_register(op);
    if (!data_register(reg) && !dag_register(reg)) {
      reject(
          op, identity, FaultReason::unsupported_instruction, "DO count register is unqualified");
    }
  }
  if (op.type == conditional_loop_type) {
    constexpr auto allowed = (0x0eULL << loop_opcode_shift) |
                             (std::uint64_t{true_condition} << condition_shift) | maximum_pc;
    if ((op.word & ~allowed) != 0) {
      reject(op, identity, FaultReason::invalid_encoding, "reserved termination bits");
    }
    if (!shape.counted()) { validate_condition(op, identity); }
  }
}

void validate_loop_abort_modifier(const Instruction &op, std::string_view identity) {
  if (loop_abort(op) && (op.word & (call_branch | clear_interrupt_branch)) != 0) {
    reject(op, identity, FaultReason::invalid_encoding, "LA requires a jump without CI");
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
  validate_loop_abort_modifier(instruction, identity);
  if ((instruction.word & clear_interrupt_branch) != 0) {
    reject(instruction,
           identity,
           FaultReason::unsupported_instruction,
           "interrupt-clear branch is not lowered");
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
  validate_loop_abort_modifier(instruction, identity);
  if ((instruction.word & clear_interrupt_branch) != 0 || !supported_compute(instruction.compute)) {
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
      (instruction.word & (modifiers & ~(delayed_branch | loop_reentry))) != 0 ||
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
  if (loop_setup(instruction)) {
    validate_loop(instruction, identity);
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

struct LoopFrame {
  llvm::Value *index{}, *metadata{}, *matches{};
};

struct OverlappingLoopFetch {
  std::uint32_t child_end;
  LoopShape parent;
};

bool overlapping_tail(const LoopShape &child, const LoopShape &parent) {
  const auto gap = (parent.end - child.end) & maximum_pc;
  return gap != 0 && gap <= loop_fetch_distance && parent.contains((child.start - 1) & maximum_pc);
}

bool overlapping_refill(const LoopShape &child, const LoopShape &parent) {
  return child.arithmetic() && !child.short_body() && overlapping_tail(child, parent);
}

bool overlapping_short_tail(const LoopShape &child, const LoopShape &parent) {
  return child.short_body() && overlapping_tail(child, parent);
}

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
           const NativeUnit &unit,
           const LoopPlan &loops)
      : module_(module),
        memory_mode_(memory_mode),
        abi_(abi),
        unit_(unit),
        variant_(variant),
        reentry_calls_(loops.reentry_calls),
        aborts_(loops.aborts),
        short_nested_(loops.short_nested),
        not_lce_(loops.not_lce),
        lce_endpoints_(loops.lce_endpoints),
        short_parent_tails_(loops.short_parent_tails),
        lce_slots_(loops.lce_slots),
        long_tail_indices_(loops.long_tail_indices),
        long_tail_count_(loops.long_tail_count),
        instructions_(std::move(instructions)),
        entry_address_(unit.entry),
        context_(unit.context),
        builder_(module.getContext()) {
    function_ = abi_.body(unit);
    function_->addFnAttr(llvm::Attribute::NoUnwind);
    debug_scope(debug, *function_, "sharc-variant-" + std::to_string(variant.id));
  }

  void emit(const LoopMap &shapes);

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

  llvm::Value *loop_cell(std::size_t slot, llvm::Value *index, unsigned bits = 32) {
    return builder_.CreateInBoundsGEP(
        llvm::IntegerType::get(module_.getContext(), bits), pointer(slot), index);
  }

  llvm::Value *short_cell(ShortLoopWord slot) {
    return builder_.CreateInBoundsGEP(
        builder_.getInt64Ty(), pointer(short_loop_slot), builder_.getInt64(slot));
  }

  llvm::Value *short_word(ShortLoopWord slot) {
    return builder_.CreateLoad(builder_.getInt64Ty(), short_cell(slot));
  }

  void write_short(ShortLoopWord slot, llvm::Value *word) {
    builder_.CreateStore(word, short_cell(slot));
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
  llvm::Value *condition(const Values &values, const Instruction &op);
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
  void readmit(const Values &values, std::uint32_t next);
  void credit(Values &values, llvm::Value *count, llvm::Value *cycles, bool partial);
  void advance_lce(llvm::Value *cycles, std::size_t slot);
  llvm::Value *lce_age(llvm::Value *tail);
  void complete(Values values,
                const Instruction &op,
                bool partial,
                llvm::Value *lanes,
                PipelineEffects pipeline,
                llvm::Value *old_status);
  void delayed_slot(const Values &values, const Instruction &op, bool partial);
  void redirect(Values values, const Instruction &op, llvm::Value *target, bool partial);
  [[nodiscard]] bool delay_boundary(std::uint32_t pc) const;
  [[nodiscard]] bool sequencer_boundary(std::uint32_t pc) const;
  void plan_hardware_loops(const LoopMap &shapes);
  void plan_short_body(const LoopShape &shape);
  void plan_overlapping_loop_fetches(const LoopMap &shapes);
  void plan_hardware_grants(const LoopMap &shapes);
  void admit_hardware_turns(const Values &values, const LoopShape &shape);
  void emit_hardware_turns(const Node &body,
                           const LoopFrame &frame,
                           llvm::Value *count,
                           const LoopShape &shape);
  void loop_push(Values &values, const Instruction &op, bool partial);
  void suspend_short_parent(Values &values,
                            const Instruction &op,
                            const LoopShape &parent,
                            unsigned words,
                            llvm::Value *active);
  void resume_short_parent(Values &values, const LoopShape &child, const LoopShape &parent);
  void loop_pop(Values &values, const LoopFrame &frame);
  void
  guard_loop_abort(const Values &values, const Instruction &op, llvm::Value *lanes, bool partial);
  void retire_loop_abort(Values &values, const LoopShape &shape);
  llvm::Value *jump_target(const Values &values, const Instruction &op);
  [[nodiscard]] std::size_t counted_tail_slot_for(const LoopShape &shape) const;
  [[nodiscard]] std::uint64_t counted_tail_word(const LoopShape &shape) const;
  llvm::Value *counted_tail_matches(const LoopShape &shape);
  void save_counted_tail(const LoopShape &shape, llvm::Value *age, bool returning);
  void clear_counted_tail(const LoopShape &shape, llvm::Value *enabled = nullptr);
  llvm::Value *
  abort_target_in_parent(llvm::Value *target, const Values &values, llvm::Value *already_exited);
  void loop_status(Values &values, llvm::Value *depth);
  [[nodiscard]] LoopFrame active_loop(const Values &values, const LoopShape &shape);
  llvm::Value *repeat_scheduled(const LoopFrame &frame);
  void clear_loop_repeat(const LoopFrame &frame);
  void loop_fetch(Values &values,
                  const LoopShape &shape,
                  llvm::Value *enabled,
                  bool returning = false,
                  llvm::Value *age = nullptr);
  void
  loop_test(Values &values, llvm::Value *old_status, const LoopShape &shape, llvm::Value *enabled);
  [[nodiscard]] llvm::Value *refill_at(llvm::Value *refill, std::uint32_t child_end);
  void overlapping_loop_fetches(Values &values,
                                std::uint32_t pc,
                                llvm::Value *refill,
                                bool whole_turn = false);
  void loop_refill(Values &values, const Instruction &op);
  void check_loop_end(const Values &values, const Instruction &op, bool partial);

  struct ShortLookahead {
    llvm::Value *fetch{}, *decode{};
  };

  struct ShortRetirement {
    llvm::Value *left{}, *old_status{};
  };

  struct ShortFetchWindow {
    llvm::Value *before{}, *after{}, *old_status{};
  };

  void write_short_addresses(ShortLookahead addresses, llvm::Value *live);
  ShortLookahead
  project_short_parent(ShortLookahead stages, const LoopShape &child, const LoopShape &parent);
  llvm::Value *short_flag(llvm::Value *descriptor, std::uint64_t mask);
  llvm::Value *short_active(const LoopShape &shape, llvm::Value *left);
  void setup_short_loop(const LoopShape &shape, llvm::Value *count);
  void short_loop_retirement(Values values,
                             const Instruction &op,
                             const LoopShape &shape,
                             llvm::Value *old_status,
                             bool partial);
  void short_loop_counter(Values &values,
                          std::uint32_t pc,
                          const LoopShape &shape,
                          llvm::Value *descriptor,
                          bool after_credit = false);
  llvm::Value *
  short_exit_descriptor(llvm::Value *descriptor, const Values &values, llvm::Value *previous_depth);
  llvm::Value *short_words_after(llvm::Value *remaining, const LoopShape &shape);
  llvm::Value *short_loop_test(Values &values, const LoopShape &shape, ShortRetirement retirement);
  llvm::Value *short_loop_refill(const LoopShape &shape, llvm::Value *done);
  llvm::Value *
  short_stage(const LoopShape &shape, std::uint32_t pc, llvm::Value *left, unsigned lag);
  ShortLookahead short_lookahead(std::uint32_t pc, const LoopShape &shape, llvm::Value *left);
  void short_loop_observation(Values &values,
                              const Instruction &op,
                              const LoopShape &shape,
                              ShortRetirement retirement);
  ShortLookahead overlapping_short_fetches(Values &values,
                                           const LoopShape &shape,
                                           ShortLookahead stages,
                                           llvm::Value *old_status);
  llvm::Value *short_fetch_crossed(ShortFetchWindow window, unsigned offset);
  ShortLookahead redirect_short_parent(ShortLookahead stages,
                                       const LoopShape &child,
                                       const LoopShape &parent,
                                       ShortFetchWindow window,
                                       llvm::Value *selected);
  void retire_short_observation(std::uint32_t pc);
  void loop_backedge(const Values &values, const LoopShape &shape, bool partial);
  void
  loop_retirement(Values &values, const Instruction &op, llvm::Value *old_status, bool partial);
  llvm::Value *branch_lanes(const Values &values, const Instruction &op, bool partial);
  llvm::Value *branch_taken(llvm::Value *lanes);
  void stack_guard(const Values &values,
                   const Instruction &op,
                   llvm::Value *allowed,
                   bool partial,
                   NativeExit reason = NativeExit::unsupported_stack);
  void stack_status(Values &values);
  void push_pc(Values &values, llvm::Value *address);
  void call(Values &values, const Instruction &op, bool partial);
  llvm::Value *loop_call_address(Values &values, const Instruction &op, const LoopShape &shape);
  llvm::Value *
  call_termination_tag(const Values &values, const Instruction &op, llvm::Value *terminated);
  llvm::Value *suspend_short_call(Values &values, const Instruction &op, const LoopShape &shape);
  void restore_short_call(Values &values,
                          const LoopShape &shape,
                          std::uint32_t pc,
                          llvm::Value *payload);
  llvm::Value *resume_short_count(Values &values,
                                  const LoopShape &shape,
                                  std::uint32_t pc,
                                  llvm::Value *payload);
  llvm::Value *pop(Values &values, const Instruction &op, bool partial);
  llvm::Value *check_loop_return(const Values &values,
                                 const Instruction &op,
                                 llvm::Value *popped_depth,
                                 bool partial);
  void restore_loop_tail(llvm::Value *tail);
  void resume_loop_tail(llvm::Value *target);
  void finish_loop_return(Values &values, const Instruction &op);
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
  const LoopMap &reentry_calls_;
  const LoopMap &aborts_;
  const LoopMap &short_nested_;
  const LoopMap &not_lce_;
  const std::set<std::uint32_t> &lce_endpoints_;
  const std::set<std::uint32_t> &short_parent_tails_;
  const std::set<std::size_t> &lce_slots_;
  const std::map<std::uint32_t, std::size_t> &long_tail_indices_;
  std::size_t long_tail_count_;
  std::vector<Instruction> instructions_;
  std::uint32_t entry_address_;
  std::uint32_t context_;
  llvm::IRBuilder<> builder_;
  llvm::Function *function_{};
  std::map<std::uint32_t, std::size_t> addresses_;
  std::set<std::uint32_t> delay_slots_;
  std::map<std::uint32_t, const Instruction *> delay_ends_;
  std::map<std::uint32_t, LoopShape> loop_fetches_, loop_tests_, hardware_ends_;
  std::map<std::uint32_t, std::vector<OverlappingLoopFetch>> overlapping_fetches_;
  std::map<std::uint32_t, std::vector<LoopShape>> short_parents_;
  std::set<std::uint32_t> shifted_fetches_;
  std::map<std::uint32_t, LoopShape> hardware_grants_, short_bodies_;
  std::set<std::uint32_t> short_successors_;
  std::set<std::uint32_t> loop_setups_;
  std::vector<Node> admission_, fast_entry_, fast_, partial_;
  std::map<std::uint32_t, MemoryPlan> memory_plan_;
  llvm::Value *memory_admitted_{};
  std::set<unsigned> written_;
  std::set<unsigned> y_written_;
  std::array<llvm::Value *, 64> dag_{};
  std::set<unsigned> dag_written_;
  bool dag_status_written_{};
  bool has_plain_loop_returns_{};
  bool has_loop_sample_returns_{};
  bool has_short_loop_returns_{};
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

llvm::Value *Lowering::condition(const Values &values, const Instruction &op) {
  const auto site = not_lce_.find(op.address);
  if (site == not_lce_.end()) { return condition(values, branch_condition(op)); }
  auto *age = lce_age(load(64, counted_tail_slot_for(site->second)));
  age = builder_.CreateAdd(age, pipeline_stall(values, op));
  const auto limit = branch(op) ? lce_branch_age : lce_compute_age;
  auto *expired = builder_.CreateAnd(counted_tail_matches(site->second),
                                     builder_.CreateICmpUGE(age, builder_.getInt64(limit)));
  auto *enabled = builder_.CreateNot(expired);
  return pair(enabled, (context_ & simd_mode) != 0 ? enabled : builder_.getFalse(), 1);
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
  auto *selected = condition(before, op);
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
      op.type == immediate_move_type ? condition(values, true_condition) : condition(values, op);
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
  if (branch(op) && branch_condition(op) != true_condition && !observes_not_lce(op)) {
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
  if (loop_setup(op)) {
    loop_push(values, op, partial);
    values[hazard_value] = builder_.getFalse();
    return;
  }
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
  auto *selected = condition(values, op);
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
                           bool partial,
                           NativeExit reason) {
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
  if (reason == NativeExit::unsupported_loop_stack ||
      reason == NativeExit::unsupported_loop_reentry || reason == NativeExit::invalid_loop_abort ||
      reason == NativeExit::invalid_short_loop_call) {
    auto *word = builder_.CreateGEP(builder_.getInt64Ty(),
                                    function_->getArg(unit_controls),
                                    builder_.getInt64(fault_instruction));
    builder_.CreateStore(builder_.getInt64(op.word), word);
  }
  exit(prefix, builder_.getInt32(op.address), reason);
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

void Lowering::push_pc(Values &values, llvm::Value *address) {
  constexpr unsigned almost_full = Registers::pc_stack_capacity - 1;
  constexpr std::uint32_t stack_overflow_interrupt = 1U << 3;
  auto *depth = values[pc_stack_depth_value];
  builder_.CreateStore(address, stack_cell(depth));
  depth = builder_.CreateAdd(depth, builder_.getInt32(1));
  values[pc_stack_depth_value] = depth;
  values[interrupt_latch_value] = builder_.CreateOr(
      values[interrupt_latch_value],
      builder_.CreateSelect(builder_.CreateICmpEQ(depth, builder_.getInt32(almost_full)),
                            builder_.getInt32(stack_overflow_interrupt),
                            builder_.getInt32(0)));
  stack_status(values);
}

void Lowering::call(Values &values, const Instruction &op, bool partial) {
  if (op.compute.kind == ComputeKind::none) {
    stack_guard(values,
                op,
                builder_.CreateICmpULT(values[pc_stack_depth_value],
                                       builder_.getInt32(Registers::pc_stack_capacity)),
                partial);
  }
  const auto site = reentry_calls_.find(op.address);
  auto *address = site == reentry_calls_.end() ? builder_.getInt32(call_resume(op) | pc_call_tag)
                                               : loop_call_address(values, op, site->second);
  push_pc(values, address);
}

llvm::Value *
Lowering::loop_call_address(Values &values, const Instruction &op, const LoopShape &shape) {
  if (shape.short_body()) { return suspend_short_call(values, op, shape); }
  const auto tail_slot = shape.counted() ? counted_tail_slot_for(shape) : loop_refill_slot;
  auto *pending = load(64, tail_slot);
  const auto signature = (std::uint64_t{shape.end} << loop_refill_end_shift) | shape.start |
                         (shape.counted() ? counted_loop_tail : loop_refill_live);
  auto *terminating = shape.counted()
                          ? counted_tail_matches(shape)
                          : builder_.CreateICmpEQ(pending, builder_.getInt64(signature));
  const auto frame = active_loop(values, shape);
  auto *in_loop = builder_.CreateOr(terminating, frame.matches);
  const bool endpoint = op.address == shape.end;
  // CALL at E replaces the remainder of the loop's pipeline refill.
  auto *saved = endpoint ? static_cast<llvm::Value *>(builder_.getInt64(0))
                         : builder_.CreateSelect(terminating, pending, builder_.getInt64(0));
  // The call redirects Fetch1 before the unsampled word arrives. The
  // return refill reaches that fetch after the three frozen fetch clocks.
  const auto sample_payload = loop_return_payload(op.address, shape);
  if (sample_payload != 0) {
    auto *unsampled = frame.matches;
    if ((sample_payload & return_loop_fetch) != 0) {
      unsampled = builder_.CreateAnd(unsampled, builder_.CreateNot(repeat_scheduled(frame)));
    }
    saved = builder_.CreateSelect(unsampled, builder_.getInt64(sample_payload), saved);
  }
  auto *call_state = builder_.CreateSelect(
      in_loop, builder_.CreateOr(saved, builder_.getInt64(loop_call_live)), builder_.getInt64(0));
  builder_.CreateStore(call_state, loop_cell(loop_call_slot, values[pc_stack_depth_value], 64));
  store(builder_.CreateSelect(terminating, builder_.getInt64(0), pending), tail_slot);
  llvm::Value *address = builder_.getInt32(call_resume(op) | pc_call_tag);
  if (endpoint) {
    auto *repeat = builder_.CreateAnd(frame.matches, repeat_scheduled(frame));
    auto *backedge = llvm::BasicBlock::Create(module_.getContext(), "call.loop.repeat", function_);
    auto *ready = llvm::BasicBlock::Create(module_.getContext(), "call.loop.resume", function_);
    builder_.CreateCondBr(repeat, backedge, ready);
    builder_.SetInsertPoint(backedge);
    clear_loop_repeat(frame);
    builder_.CreateBr(ready);
    builder_.SetInsertPoint(ready);
    return builder_.CreateSelect(repeat, builder_.getInt32(shape.start | pc_call_tag), address);
  }
  // PCSTK bit 25 is set when the sample exits while CALL is in Address.
  const auto sample =
      shape.counted() ? (shape.end - loop_fetch_distance) & maximum_pc : shape.condition_pc();
  if (shape.term == true_condition || op.address != ((sample + 1) & maximum_pc)) { return address; }
  return builder_.CreateOr(address, call_termination_tag(values, op, terminating));
}

llvm::Value *Lowering::call_termination_tag(const Values &values,
                                            const Instruction &op,
                                            llvm::Value *terminated) {
  // An interlock holds the CALL in Decode while the previous word retires.
  auto *same_clock = builder_.CreateICmpEQ(pipeline_stall(values, op), builder_.getInt64(0));
  return builder_.CreateSelect(builder_.CreateAnd(terminated, same_clock),
                               builder_.getInt32(pc_sequencer_tag),
                               builder_.getInt32(0));
}

llvm::Value *
Lowering::suspend_short_call(Values &values, const Instruction &op, const LoopShape &shape) {
  auto *remaining = short_word(short_remaining);
  auto *active = short_active(shape, remaining);
  auto *descriptor = short_word(short_descriptor);
  if (shape.counted() && op.address == shape.start) {
    // The initial fetch belongs to the DO fill. The CALL replaces the
    // rest of that fill and saves the buffer after the count sample.
    short_loop_counter(values, op.address, shape, descriptor);
  }
  auto *zero = builder_.getInt64(0);
  auto *left = short_words_after(remaining, shape);
  auto *packed = left;
  if (!shape.counted()) {
    // A present short payload with zero words encodes an unbounded pass.
    packed = builder_.CreateSelect(
        builder_.CreateICmpEQ(left, builder_.getInt64(short_unbounded)), zero, left);
  }
  llvm::Value *target = builder_.getInt32(call_resume(op));
  if (op.address == shape.end) {
    auto *repeat = builder_.CreateAnd(active, builder_.CreateICmpNE(left, zero));
    target = builder_.CreateSelect(repeat, builder_.getInt32(shape.start), target);
  }
  auto *buffer =
      builder_.CreateOr(builder_.CreateShl(packed, builder_.getInt64(return_short_left_shift)),
                        builder_.CreateZExt(target, builder_.getInt64Ty()));
  if (shape.counted() && shape.length == 3 && op.address != shape.end) {
    auto *compact = short_flag(descriptor, short_compact_count);
    buffer = builder_.CreateOr(
        buffer, builder_.CreateSelect(compact, builder_.getInt64(return_short_compact), zero));
  }
  if (lce_endpoints_.contains(shape.end)) {
    auto *expired = counted_tail_matches(shape);
    buffer = builder_.CreateOr(
        buffer, builder_.CreateSelect(expired, builder_.getInt64(return_short_lce), zero));
    clear_counted_tail(shape, active);
  }
  auto *saved = builder_.CreateSelect(
      builder_.CreateICmpEQ(left, zero),
      builder_.getInt64(loop_call_live),
      builder_.CreateOr(buffer, builder_.getInt64(loop_call_live | return_short_loop)));
  auto *slot = loop_cell(loop_call_slot, values[pc_stack_depth_value], 64);
  builder_.CreateStore(builder_.CreateSelect(active, saved, zero), slot);
  write_short(short_remaining, builder_.CreateSelect(active, zero, remaining));
  write_short(short_addresses, builder_.CreateSelect(active, zero, short_word(short_addresses)));
  llvm::Value *tags = builder_.getInt32(pc_call_tag);
  if (shape.arithmetic() || (shape.counted() && shape.length == 3)) {
    auto *exit = short_flag(descriptor, short_call_exit);
    tags =
        builder_.CreateOr(tags, call_termination_tag(values, op, builder_.CreateAnd(active, exit)));
  }
  return builder_.CreateOr(target, tags);
}

llvm::Value *Lowering::pop(Values &values, const Instruction &op, bool partial) {
  auto *depth = values[pc_stack_depth_value];
  stack_guard(values,
              op,
              builder_.CreateAnd(
                  builder_.CreateICmpNE(depth, builder_.getInt32(0)),
                  builder_.CreateICmpULE(depth, builder_.getInt32(Registers::pc_stack_capacity))),
              partial);
  auto *popped_depth = builder_.CreateSub(depth, builder_.getInt32(1));
  auto *top = builder_.CreateLoad(builder_.getInt32Ty(), stack_cell(popped_depth));
  auto *tagged = builder_.CreateICmpNE(builder_.CreateAnd(top, builder_.getInt32(pc_sequencer_tag)),
                                       builder_.getInt32(0));
  auto *allowed = builder_.CreateNot(tagged);
  const bool reentry = (op.word & loop_reentry) != 0;
  if (reentry) {
    auto *call = builder_.CreateICmpNE(builder_.CreateAnd(top, builder_.getInt32(pc_call_tag)),
                                       builder_.getInt32(0));
    allowed = builder_.CreateOr(allowed, call);
  }
  auto *payload =
      reentry_calls_.empty() ? nullptr : check_loop_return(values, op, popped_depth, partial);
  stack_guard(values, op, allowed, partial);
  if (payload != nullptr && (reentry || has_plain_loop_returns_)) {
    store(builder_.CreateAnd(payload, builder_.getInt64(~loop_call_live)), return_loop_tail_slot);
    builder_.CreateStore(builder_.getInt64(0), loop_cell(loop_call_slot, popped_depth, 64));
  }
  values[pc_stack_depth_value] = popped_depth;
  stack_status(values);
  return builder_.CreateAnd(top, builder_.getInt32(maximum_pc));
}

llvm::Value *Lowering::check_loop_return(const Values &values,
                                         const Instruction &op,
                                         llvm::Value *popped_depth,
                                         bool partial) {
  auto *slot = loop_cell(loop_call_slot, popped_depth, 64);
  auto *pending = builder_.CreateLoad(builder_.getInt64Ty(), slot);
  if ((op.word & loop_reentry) == 0) {
    llvm::Value *ordinary = builder_.CreateICmpEQ(pending, builder_.getInt64(0));
    if (has_plain_loop_returns_) {
      // A CALL before the end word reached Fetch1 has no sampled count
      // to freeze. Its ordinary RTS reaches the first sample naturally.
      auto *fetch =
          builder_.CreateAnd(pending, builder_.getInt64(return_loop_fetch | return_short_loop));
      ordinary = builder_.CreateOr(
          ordinary, builder_.CreateICmpEQ(fetch, builder_.getInt64(return_loop_fetch)));
    }
    stack_guard(values, op, ordinary, partial, NativeExit::unsupported_loop_reentry);
  }
  return pending;
}

void Lowering::restore_loop_tail(llvm::Value *tail) {
  auto *refill = short_flag(tail, loop_refill_live);
  store(builder_.CreateSelect(refill, tail, load(64, loop_refill_slot)), loop_refill_slot);
  auto *counted = short_flag(tail, counted_loop_tail);
  for (std::size_t index = 0; index < long_tail_count_; ++index) {
    const auto slot = counted_tail_slot + index;
    auto *present = counted;
    if (long_tail_count_ > 1) {
      auto *saved = builder_.CreateAnd(tail, builder_.getInt64(counted_tail_index_mask));
      auto *selected = builder_.CreateICmpEQ(
          saved, builder_.getInt64(std::uint64_t{index} << counted_tail_index_shift));
      present = builder_.CreateAnd(counted, selected);
    }
    auto *restored = tail;
    if (lce_slots_.contains(slot)) {
      auto *mature =
          builder_.CreateOr(builder_.CreateAnd(tail, builder_.getInt64(~lce_before_credit)),
                            builder_.getInt64(lce_age_mask));
      restored = builder_.CreateSelect(short_flag(tail, lce_clock_mask), mature, tail);
    }
    store(builder_.CreateSelect(present, restored, load(64, slot)), slot);
  }
}

void Lowering::resume_loop_tail(llvm::Value *target) {
  auto *payload = load(64, return_loop_tail_slot);
  auto *zero = builder_.getInt64(0);
  llvm::Value *short_ready = builder_.getFalse();
  llvm::Value *long_payload = payload;
  if (has_short_loop_returns_) {
    auto *kind = builder_.CreateAnd(payload, builder_.getInt64(return_short_loop));
    auto *is_short = builder_.CreateICmpNE(kind, zero);
    auto *address = builder_.CreateAnd(builder_.CreateTrunc(payload, builder_.getInt32Ty()),
                                       builder_.getInt32(maximum_pc));
    short_ready = builder_.CreateAnd(is_short, builder_.CreateICmpEQ(target, address));
    long_payload = builder_.CreateSelect(is_short, zero, payload);
  }
  restore_loop_tail(long_payload);
  llvm::Value *valid = short_ready;
  if (has_loop_sample_returns_) {
    auto *kind = builder_.CreateAnd(long_payload,
                                    builder_.getInt64(return_loop_fetch | return_loop_condition));
    auto *is_fetch = builder_.CreateICmpEQ(kind, builder_.getInt64(return_loop_fetch));
    auto *distance = builder_.CreateSelect(is_fetch,
                                           builder_.getInt32(loop_fetch_distance - 1),
                                           builder_.getInt32(loop_decode_distance - 1));
    auto *end = builder_.CreateTrunc(
        builder_.CreateLShr(long_payload, builder_.getInt64(loop_refill_end_shift)),
        builder_.getInt32Ty());
    auto *resume =
        builder_.CreateAnd(builder_.CreateSub(end, distance), builder_.getInt32(maximum_pc));
    valid = builder_.CreateOr(valid,
                              builder_.CreateAnd(builder_.CreateICmpNE(kind, zero),
                                                 builder_.CreateICmpEQ(target, resume)));
  }
  auto *ready = builder_.CreateSelect(
      valid, builder_.CreateOr(payload, builder_.getInt64(return_loop_ready)), zero);
  store(ready, return_loop_tail_slot);
}

void Lowering::finish_loop_return(Values &values, const Instruction &op) {
  const auto *shape = loop_return_at(op.address, reentry_calls_);
  if (shape == nullptr) { return; }
  auto *payload = load(64, return_loop_tail_slot);
  if (shape->short_body()) {
    restore_short_call(values, *shape, op.address, payload);
    return;
  }
  const auto sample = loop_return_payload((op.address - 1) & maximum_pc, *shape);
  auto *ready = builder_.CreateICmpEQ(payload, builder_.getInt64(sample | return_loop_ready));
  store(builder_.CreateSelect(ready, builder_.getInt64(0), payload), return_loop_tail_slot);
  if ((sample & return_loop_fetch) != 0) {
    loop_fetch(values, *shape, ready, true);
  } else {
    loop_test(values, values[status_value], *shape, ready);
  }
}

void Lowering::restore_short_call(Values &values,
                                  const LoopShape &shape,
                                  std::uint32_t pc,
                                  llvm::Value *payload) {
  constexpr auto flags = return_short_loop | return_loop_ready;
  auto *signature = builder_.CreateAnd(payload, builder_.getInt64(maximum_pc | flags));
  auto *ready = builder_.CreateICmpEQ(signature, builder_.getInt64(pc | flags));
  auto *restore = llvm::BasicBlock::Create(module_.getContext(), "short.return", function_);
  auto next = node("short.return.continue");
  next.incoming(values, builder_.GetInsertBlock());
  builder_.CreateCondBr(ready, restore, next.block);
  builder_.SetInsertPoint(restore);
  auto *left =
      builder_.CreateAnd(builder_.CreateLShr(payload, builder_.getInt64(return_short_left_shift)),
                         builder_.getInt64(return_short_left_mask));
  write_short(short_fetch_progress, builder_.getInt64(0));
  if (lce_endpoints_.contains(shape.end)) {
    auto *expired = short_flag(payload, return_short_lce);
    auto *tail = builder_.getInt64(counted_tail_word(shape) | lce_age_mask);
    const auto slot = counted_tail_slot_for(shape);
    store(builder_.CreateSelect(expired, tail, load(64, slot)), slot);
  }
  const auto call_pc = pc == shape.start ? shape.end : ((pc - 1) & maximum_pc);
  llvm::Value *descriptor = builder_.getInt64(shape.end);
  if (shape.counted()) {
    descriptor = resume_short_count(values, shape, pc, payload);
  } else {
    left = builder_.CreateSelect(builder_.CreateICmpEQ(left, builder_.getInt64(0)),
                                 builder_.getInt64(short_unbounded),
                                 left);
    if (shape.arithmetic() && call_pc == shape.condition_pc()) {
      // The fourth return fetch reaches the condition word after the freeze.
      auto *depth = values[pc_stack_depth_value];
      left = short_loop_test(values, shape, {.left = left, .old_status = values[status_value]});
      if (reentry_calls_.contains(pc)) {
        descriptor = short_exit_descriptor(descriptor, values, depth);
      }
    }
  }
  write_short(short_remaining, left);
  write_short(short_descriptor, descriptor);
  auto stages = short_lookahead(call_pc, shape, left);
  if (shape.counted() && shape.length == 3 && call_pc == shape.start) {
    // The compact entry still holds E for the sample at its second word.
    auto *compact = short_flag(descriptor, short_compact_count);
    stages.fetch = builder_.CreateSelect(compact, builder_.getInt64(shape.end), stages.fetch);
  }
  write_short_addresses(stages, builder_.getTrue());
  store(builder_.getInt64(0), return_loop_tail_slot);
  jump(next, values);
  builder_.SetInsertPoint(next.block);
  values = next.values();
}

llvm::Value *Lowering::resume_short_count(Values &values,
                                          const LoopShape &shape,
                                          std::uint32_t pc,
                                          llvm::Value *payload) {
  llvm::Value *descriptor = builder_.getInt64(shape.end);
  if (shape.length == 3) {
    auto *compact = short_flag(payload, return_short_compact);
    descriptor = builder_.CreateSelect(
        compact, builder_.getInt64(shape.end | short_compact_count), descriptor);
    // E, B, B+1, E puts the steady sample after the frozen fetches.
    // A one-pass loop retains its unsampled end and compact drain state.
    if (pc == shape.end) {
      auto *depth = values[pc_stack_depth_value];
      short_loop_counter(values, (pc - 1) & maximum_pc, shape, descriptor, true);
      if (reentry_calls_.contains(pc)) {
        descriptor = short_exit_descriptor(descriptor, values, depth);
      }
    }
  } else if (pc == shape.start) {
    // The repeating two- and four-word endpoints sample on the fourth fetch.
    short_loop_counter(values, shape.end, shape, descriptor, true);
  }
  return descriptor;
}

void Lowering::return_to_caller(Values values, llvm::Value *target, bool partial) {
  auto *unknown = llvm::BasicBlock::Create(module_.getContext(), "cold.return", function_);
  auto *dispatch = builder_.CreateSwitch(target, unknown);
  for (const auto address : call_continuations(instructions_, reentry_calls_)) {
    const auto found = addresses_.find(address);
    if (found == addresses_.end()) { continue; }
    const bool admission = !partial || loop_return_at(address, reentry_calls_) != nullptr;
    auto &destination = admission ? admission_[found->second] : partial_[found->second];
    destination.incoming(values, builder_.GetInsertBlock());
    dispatch->addCase(builder_.getInt32(address), destination.block);
  }
  builder_.SetInsertPoint(unknown);
  exit(values, target, NativeExit::returned);
}

void Lowering::credit(Values &values, llvm::Value *count, llvm::Value *cycles, bool partial) {
  for (const auto slot : lce_slots_) { advance_lce(cycles, slot); }
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

llvm::Value *Lowering::lce_age(llvm::Value *tail) {
  auto *masked = builder_.CreateAnd(tail, builder_.getInt64(lce_age_mask));
  return builder_.CreateLShr(masked, builder_.getInt64(lce_age_shift));
}

void Lowering::advance_lce(llvm::Value *cycles, std::size_t slot) {
  auto *tail = load(64, slot);
  auto *age = lce_age(tail);
  auto *limit = builder_.getInt64(lce_branch_age);
  auto *elapsed = builder_.CreateBinaryIntrinsic(llvm::Intrinsic::umin, cycles, limit);
  auto *advanced = builder_.CreateBinaryIntrinsic(
      llvm::Intrinsic::umin, builder_.CreateAdd(age, elapsed), limit);
  // The sampling instruction records just the clocks from its own fetch to
  // retirement. Its earlier stalls must not age a newly sampled termination.
  auto *next = builder_.CreateSelect(short_flag(tail, lce_before_credit), age, advanced);
  auto *clocked = builder_.CreateOr(builder_.CreateAnd(tail, builder_.getInt64(~lce_clock_mask)),
                                    builder_.CreateShl(next, builder_.getInt64(lce_age_shift)));
  auto *live = builder_.CreateAnd(short_flag(tail, lce_clock_mask),
                                  builder_.CreateICmpNE(cycles, builder_.getInt64(0)));
  store(builder_.CreateSelect(live, clocked, tail), slot);
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
  auto *lanes = condition(values, op);
  if (call_branch_instruction(op) && op.compute.kind != ComputeKind::none) {
    auto *room = builder_.CreateICmpULT(values[pc_stack_depth_value],
                                        builder_.getInt32(Registers::pc_stack_capacity));
    stack_guard(
        values, op, builder_.CreateOr(builder_.CreateNot(branch_taken(lanes)), room), partial);
  }
  const auto site = reentry_calls_.find(op.address);
  if (site != reentry_calls_.end() && site->second.counted() && site->second.length == 2) {
    auto *one_turn = short_flag(short_word(short_descriptor), short_compact_count);
    auto *active = short_active(site->second, short_word(short_remaining));
    auto *invalid = builder_.CreateAnd(branch_taken(lanes), builder_.CreateAnd(active, one_turn));
    stack_guard(
        values, op, builder_.CreateNot(invalid), partial, NativeExit::invalid_short_loop_call);
  }
  if (loop_abort(op)) { guard_loop_abort(values, op, lanes, partial); }
  return lanes;
}

llvm::Value *Lowering::jump_target(const Values &values, const Instruction &op) {
  if (!indirect_branch(op)) { return builder_.getInt32(branch_target(op)); }
  auto *address = builder_.CreateAdd(values[index_value + branch_index(op)],
                                     values[modifier_value + branch_modifier(op)]);
  return builder_.CreateAnd(address, builder_.getInt32(maximum_pc));
}

void Lowering::complete(Values values,
                        const Instruction &op,
                        bool partial,
                        llvm::Value *lanes,
                        PipelineEffects pipeline,
                        llvm::Value *old_status) {
  if (!partial && loop_ends_.contains(addresses_.at(op.address))) {
    complete_loop(values, op);
    return;
  }
  auto *count = partial ? builder_.getInt64(1) : values[credit_value];
  const auto next = next_pc(op);
  if (!branch(op)) {
    retire_short_observation(op.address);
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
  if (loop_abort(op)) { retire_loop_abort(branch_values, aborts_.at(op.address)); }
  auto *target = op.type == return_branch_type ? pop(branch_values, op, partial) : nullptr;
  retire_short_observation(op.address);
  if (indirect_branch(op)) { target = jump_target(values, op); }
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
  retire_short_observation(op.address);
  retire_pipeline(values, pipeline);
  values[hazard_value] = builder_.getInt1(op.compute.kind != ComputeKind::none);
  if (loop_abort(op) || hardware_ends_.contains(op.address) ||
      reentry_calls_.contains(op.address)) {
    loop_retirement(values, op, old_status, partial);
  }
  credit(values, count, builder_.CreateAdd(count, values[extra_cycles_value]), partial);
  finish(values, next, partial);
}

void Lowering::redirect(Values values, const Instruction &op, llvm::Value *target, bool partial) {
  if (op.type == return_branch_type) {
    if (!reentry_calls_.empty() && ((op.word & loop_reentry) != 0 || has_plain_loop_returns_)) {
      resume_loop_tail(target);
    }
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

bool Lowering::sequencer_boundary(std::uint32_t pc) const {
  return delay_boundary(pc) || loop_setups_.contains(pc) || loop_fetches_.contains(pc) ||
         loop_tests_.contains(pc) || hardware_ends_.contains(pc) || hardware_grants_.contains(pc) ||
         short_bodies_.contains(pc) || short_successors_.contains(pc) || aborts_.contains(pc) ||
         overlapping_fetches_.contains(pc) || not_lce_.contains(pc);
}

void Lowering::plan_hardware_loops(const LoopMap &shapes) {
  for (const auto &op : instructions_) {
    if (loop_setup(op)) { loop_setups_.insert(op.address); }
  }
  for (const auto &[end, shape] : shapes) {
    if (shape.short_body()) {
      plan_short_body(shape);
      continue;
    }
    if (addresses_.contains(end)) { hardware_ends_.emplace(end, shape); }
    const auto fetch = (end - loop_fetch_distance) & maximum_pc;
    if (addresses_.contains(fetch)) { loop_fetches_.emplace(fetch, shape); }
    const auto test = shape.condition_pc();
    if (shape.arithmetic() && addresses_.contains(test)) { loop_tests_.emplace(test, shape); }
  }
  plan_overlapping_loop_fetches(shapes);
}

void Lowering::plan_short_body(const LoopShape &shape) {
  for (unsigned i = 0; i < shape.length; ++i) {
    const auto pc = (shape.start + i) & maximum_pc;
    if (!addresses_.contains(pc)) { continue; }
    const auto [site, inserted] = short_bodies_.try_emplace(pc, shape);
    if (!inserted && shape.length < site->second.length) { site->second = shape; }
  }
  if (shape.length == loop_fetch_distance) {
    short_successors_.insert((shape.end + 1) & maximum_pc);
  }
}

void Lowering::plan_overlapping_loop_fetches(const LoopMap &shapes) {
  for (const auto &[end, child] : shapes) {
    if (!child.arithmetic() && !child.short_body()) { continue; }
    // A given child appends the candidate fetches in refill clock order.
    // Events for other children are gated by the active refill identity.
    for (unsigned gap = 1; gap <= loop_fetch_distance; ++gap) {
      const auto parent = shapes.find((end + gap) & maximum_pc);
      if (parent == shapes.end()) { continue; }
      if (overlapping_short_tail(child, parent->second)) {
        short_parents_[end].push_back(parent->second);
      }
      if (!overlapping_refill(child, parent->second)) { continue; }
      const auto pc = (end - (gap == 1 ? 1U : 0U)) & maximum_pc;
      if (!addresses_.contains(pc)) { continue; }
      overlapping_fetches_[pc].push_back({.child_end = end, .parent = parent->second});
      shifted_fetches_.insert(parent->first);
    }
  }
}

void Lowering::loop_push(Values &values, const Instruction &op, bool partial) {
  auto *depth = builder_.CreateZExt(load(8, loop_address_depth_slot), builder_.getInt32Ty());
  auto *counter_depth =
      builder_.CreateZExt(load(8, loop_counter_depth_slot), builder_.getInt32Ty());
  auto *pc_depth = values[pc_stack_depth_value];
  const auto shape = loop_shape(op);
  auto *room = builder_.CreateAnd(
      builder_.CreateICmpULT(depth, builder_.getInt32(loop_stack_capacity)),
      builder_.CreateICmpULT(pc_depth, builder_.getInt32(Registers::pc_stack_capacity)));
  const auto parent = short_nested_.find(shape.end);
  llvm::Value *parent_active = nullptr;
  if (shape.short_body()) {
    auto *left = short_word(short_remaining);
    auto *available = builder_.CreateICmpEQ(left, builder_.getInt64(0));
    if (parent != short_nested_.end()) {
      parent_active = short_active(parent->second, left);
      auto *free = builder_.CreateICmpEQ(short_word(short_parent_remaining), builder_.getInt64(0));
      available = builder_.CreateOr(available, builder_.CreateAnd(parent_active, free));
    }
    room = builder_.CreateAnd(room, available);
  }
  stack_guard(values,
              op,
              builder_.CreateAnd(room, builder_.CreateICmpEQ(depth, counter_depth)),
              partial,
              NativeExit::unsupported_loop_stack);
  if (parent_active != nullptr) {
    suspend_short_parent(values, op, parent->second, shape.length + 1, parent_active);
    depth = builder_.CreateZExt(load(8, loop_address_depth_slot), builder_.getInt32Ty());
    pc_depth = values[pc_stack_depth_value];
  }
  auto *count = load(32, lcntr_slot);
  if (op.type == counted_loop_type) {
    if (register_count(op)) {
      auto *raw = builder_.CreateExtractElement(read_register(values, count_register(op)),
                                                builder_.getInt32(0));
      count = builder_.CreateTrunc(builder_.CreateLShr(raw, builder_.getInt64(raw_extension_bits)),
                                   builder_.getInt32Ty());
    } else {
      count = builder_.getInt32(static_cast<std::uint16_t>(op.word >> immediate_count_shift));
    }
    store(count, lcntr_slot);
  }
  if (shape.short_body()) { setup_short_loop(shape, count); }
  builder_.CreateStore(builder_.getInt32(shape.address_word()),
                       loop_cell(loop_address_slot, depth));
  builder_.CreateStore(count, loop_cell(loop_counter_slot, depth));
  auto *metadata = builder_.CreateOr(pc_depth, builder_.getInt32(loop_frame_live));
  builder_.CreateStore(metadata, loop_cell(loop_frames_slot, depth));
  auto *next_depth = builder_.CreateAdd(depth, builder_.getInt32(1));
  auto *raw_depth = builder_.CreateTrunc(next_depth, builder_.getInt8Ty());
  store(raw_depth, loop_address_depth_slot);
  store(raw_depth, loop_counter_depth_slot);
  push_pc(values, builder_.getInt32(shape.start));
  loop_status(values, next_depth);
}

void Lowering::suspend_short_parent(Values &values,
                                    const Instruction &op,
                                    const LoopShape &parent,
                                    unsigned words,
                                    llvm::Value *active) {
  auto next = node("short.child.enter");
  auto *suspend = llvm::BasicBlock::Create(module_.getContext(), "short.parent.suspend", function_);
  next.incoming(values, builder_.GetInsertBlock());
  builder_.CreateCondBr(active, suspend, next.block);
  builder_.SetInsertPoint(suspend);
  auto *descriptor = short_word(short_descriptor);
  auto *left = short_word(short_remaining);
  if (parent.counted()) {
    short_loop_counter(values, op.address, parent, descriptor);
    left = builder_.CreateSub(left, builder_.getInt64(words));
  }
  if (parent.length == loop_fetch_distance) {
    auto *startup = short_flag(descriptor, short_startup);
    values[extra_cycles_value] = builder_.CreateAdd(
        values[extra_cycles_value], builder_.CreateZExt(startup, builder_.getInt64Ty()));
    auto *history = values[dag_history_value];
    auto *older = builder_.CreateAnd(builder_.CreateShl(history, builder_.getInt32(8)),
                                     builder_.getInt32(history_older_mask));
    values[dag_history_value] = builder_.CreateSelect(startup, older, history);
  }
  write_short(short_parent_remaining, left);
  write_short(short_parent_descriptor,
              builder_.CreateAnd(descriptor, builder_.getInt64(~short_startup)));
  jump(next, values);
  builder_.SetInsertPoint(next.block);
  values = next.values();
}

void Lowering::resume_short_parent(Values &values,
                                   const LoopShape &child,
                                   const LoopShape &parent) {
  auto *left = short_word(short_parent_remaining);
  auto *resume = llvm::BasicBlock::Create(module_.getContext(), "short.parent.resume", function_);
  auto next = node("short.child.leave");
  next.incoming(values, builder_.GetInsertBlock());
  builder_.CreateCondBr(builder_.CreateICmpNE(left, builder_.getInt64(0)), resume, next.block);
  builder_.SetInsertPoint(resume);
  auto *descriptor = short_word(short_parent_descriptor);
  if (parent.counted()) { short_loop_counter(values, child.end, parent, descriptor, true); }
  write_short(short_remaining, left);
  write_short(short_descriptor, descriptor);
  write_short(short_parent_remaining, builder_.getInt64(0));
  write_short(short_fetch_progress, builder_.getInt64(0));
  write_short_addresses(short_lookahead(child.end, parent, left), builder_.getTrue());
  jump(next, values);
  builder_.SetInsertPoint(next.block);
  values = next.values();
}

llvm::Value *Lowering::short_flag(llvm::Value *descriptor, std::uint64_t mask) {
  return builder_.CreateICmpNE(builder_.CreateAnd(descriptor, builder_.getInt64(mask)),
                               builder_.getInt64(0));
}

llvm::Value *Lowering::short_active(const LoopShape &shape, llvm::Value *left) {
  auto *end = builder_.CreateAnd(short_word(short_descriptor), builder_.getInt64(maximum_pc));
  return builder_.CreateAnd(builder_.CreateICmpNE(left, builder_.getInt64(0)),
                            builder_.CreateICmpEQ(end, builder_.getInt64(shape.end)));
}

void Lowering::write_short_addresses(ShortLookahead addresses, llvm::Value *live) {
  auto *packed = builder_.CreateOr(
      builder_.CreateAnd(addresses.fetch, builder_.getInt64(maximum_pc)),
      builder_.CreateShl(builder_.CreateAnd(addresses.decode, builder_.getInt64(maximum_pc)),
                         builder_.getInt64(short_decode_shift)));
  write_short(
      short_addresses,
      builder_.CreateSelect(live,
                            builder_.CreateOr(packed, builder_.getInt64(short_addresses_live)),
                            builder_.getInt64(0)));
}

void Lowering::setup_short_loop(const LoopShape &shape, llvm::Value *count) {
  // The startup states hold or substitute prefetched words for the short bodies.
  constexpr std::array fetch_offsets{0U, 1U, 1U, 3U};
  constexpr std::array decode_offsets{0U, 1U, 2U, 1U};
  const auto index = shape.length - 1;
  llvm::Value *remaining = builder_.getInt64(short_unbounded);
  llvm::Value *descriptor = builder_.getInt64(shape.end | short_startup);
  if (shape.counted()) {
    auto *zero = builder_.CreateICmpEQ(count, builder_.getInt32(0));
    auto *iterations = builder_.CreateSelect(
        zero, builder_.getInt64(unsigned_limit), builder_.CreateZExt(count, builder_.getInt64Ty()));
    remaining = builder_.CreateMul(iterations, builder_.getInt64(shape.length));
    auto *compact = builder_.CreateAnd(
        builder_.CreateNot(zero),
        builder_.CreateICmpULT(count, builder_.getInt32(shape.short_threshold())));
    descriptor = builder_.CreateOr(descriptor,
                                   builder_.CreateSelect(compact,
                                                         builder_.getInt64(short_compact_count),
                                                         builder_.getInt64(0)));
  }
  write_short(short_remaining, remaining);
  write_short(short_descriptor, descriptor);
  write_short(short_fetch_progress, builder_.getInt64(0));
  write_short_addresses({.fetch = builder_.getInt64(shape.start + fetch_offsets[index]),
                         .decode = builder_.getInt64(shape.start + decode_offsets[index])},
                        builder_.getTrue());
}

void Lowering::short_loop_counter(Values &values,
                                  std::uint32_t pc,
                                  const LoopShape &shape,
                                  llvm::Value *descriptor,
                                  bool after_credit) {
  const auto frame = active_loop(values, shape);
  auto *startup = short_flag(descriptor, short_startup);
  auto *compact = short_flag(descriptor, short_compact_count);
  const auto second = (shape.start + 1) & maximum_pc;
  if (shape.length == 3) { startup = builder_.CreateAnd(startup, builder_.CreateNot(compact)); }
  // Once started, Fetch1 encounters the end when E executes (lengths 1/2/4),
  // or when the second word executes (length 3). The startup can count a
  // held end word before the ordinary rolling window is established.
  const bool steady_fetch = pc == (shape.length == 3 ? second : shape.end);
  auto *attempts = builder_.CreateAdd(builder_.getInt32(steady_fetch ? 1 : 0),
                                      builder_.CreateZExt(startup, builder_.getInt32Ty()));
  auto *consider = llvm::BasicBlock::Create(module_.getContext(), "short.fetch.end", function_);
  auto next = node("short.fetch.continue");
  next.incoming(values, builder_.GetInsertBlock());
  builder_.CreateCondBr(
      builder_.CreateAnd(frame.matches, builder_.CreateICmpNE(attempts, builder_.getInt32(0))),
      consider,
      next.block);
  builder_.SetInsertPoint(consider);
  auto *cell = loop_cell(loop_counter_slot, frame.index);
  auto *current = builder_.CreateLoad(builder_.getInt32Ty(), cell);
  auto *threshold = builder_.CreateSelect(
      compact, builder_.getInt32(1), builder_.getInt32(shape.short_threshold()));
  auto *terminate = builder_.CreateICmpULT(builder_.CreateSub(current, threshold), attempts);
  auto *updated = builder_.CreateSelect(terminate,
                                        builder_.CreateSub(threshold, builder_.getInt32(1)),
                                        builder_.CreateSub(current, attempts));
  builder_.CreateStore(updated, cell);
  auto *pop = llvm::BasicBlock::Create(module_.getContext(), "short.fetch.pop", function_);
  next.incoming(values, builder_.GetInsertBlock());
  builder_.CreateCondBr(terminate, pop, next.block);
  builder_.SetInsertPoint(pop);
  loop_pop(values, frame);
  if (lce_endpoints_.contains(shape.end)) {
    llvm::Value *age = builder_.getInt64(1);
    if (shape.length != 3 && !after_credit) {
      // The held two- or four-word startup fetch ages through its entry bubble.
      age = builder_.CreateAdd(age, builder_.CreateZExt(startup, builder_.getInt64Ty()));
    }
    save_counted_tail(shape, age, after_credit);
  }
  jump(next, values);
  builder_.SetInsertPoint(next.block);
  values = next.values();
}

llvm::Value *Lowering::short_exit_descriptor(llvm::Value *descriptor,
                                             const Values &values,
                                             llvm::Value *previous_depth) {
  // A following CALL can be in Address when the sample pops the loop.
  auto *popped = builder_.CreateICmpNE(previous_depth, values[pc_stack_depth_value]);
  return builder_.CreateOr(
      descriptor,
      builder_.CreateSelect(popped, builder_.getInt64(short_call_exit), builder_.getInt64(0)));
}

llvm::Value *Lowering::short_words_after(llvm::Value *remaining, const LoopShape &shape) {
  auto *left = builder_.CreateSub(remaining, builder_.getInt64(1));
  if (shape.counted()) { return left; }
  auto *unbounded = builder_.CreateICmpEQ(remaining, builder_.getInt64(short_unbounded));
  return builder_.CreateSelect(unbounded, remaining, left);
}

llvm::Value *
Lowering::short_loop_test(Values &values, const LoopShape &shape, ShortRetirement retirement) {
  const auto [left, old_status] = retirement;
  auto *unbounded = builder_.CreateICmpEQ(left, builder_.getInt64(short_unbounded));
  auto before = values;
  before[status_value] = old_status;
  auto *satisfied = branch_taken(condition(before, shape.term));
  if (shape.length == 1) {
    // The single-word loop samples on the startup bubble as well.
    auto *startup = short_flag(short_word(short_descriptor), short_startup);
    satisfied = builder_.CreateOr(
        satisfied, builder_.CreateAnd(startup, branch_taken(condition(values, shape.term))));
  }
  const auto frame = active_loop(values, shape);
  auto *done = builder_.CreateAnd(builder_.CreateAnd(frame.matches, unbounded), satisfied);
  auto *pop = llvm::BasicBlock::Create(module_.getContext(), "short.condition.pop", function_);
  auto next = node("short.condition.continue");
  next.incoming(values, builder_.GetInsertBlock());
  builder_.CreateCondBr(done, pop, next.block);
  builder_.SetInsertPoint(pop);
  loop_pop(values, frame);
  jump(next, values);
  builder_.SetInsertPoint(next.block);
  values = next.values();
  // The sample commits the rest of this pass and the documented extra passes.
  const auto current_suffix = (shape.end - shape.condition_pc()) & maximum_pc;
  return builder_.CreateSelect(
      done, builder_.getInt64(current_suffix + shape.short_condition_tail()), left);
}

llvm::Value *
Lowering::short_stage(const LoopShape &shape, std::uint32_t pc, llvm::Value *left, unsigned lag) {
  const auto position = (pc - shape.start + 1) & maximum_pc;
  const auto body_pc = shape.start + ((position + lag) % shape.length);
  auto *following = builder_.CreateSub(builder_.getInt64(shape.end + 1 + lag), left);
  return builder_.CreateSelect(
      builder_.CreateICmpUGT(left, builder_.getInt64(lag)), builder_.getInt64(body_pc), following);
}

Lowering::ShortLookahead
Lowering::short_lookahead(std::uint32_t pc, const LoopShape &shape, llvm::Value *left) {
  if (!shape.counted()) {
    auto *repeat = builder_.getInt64(short_unbounded);
    auto *fetch = short_stage(shape, pc, repeat, loop_fetch_distance);
    auto *decode = short_stage(shape, pc, repeat, loop_decode_distance);
    auto *redirect = builder_.CreateICmpULE(left, builder_.getInt64(loop_decode_distance));
    fetch =
        builder_.CreateSelect(redirect, short_stage(shape, pc, left, loop_decode_distance), fetch);
    return {.fetch = fetch, .decode = decode};
  }
  auto *descriptor = short_word(short_descriptor);
  auto *fetch = short_stage(shape, pc, left, loop_fetch_distance);
  auto *decode =
      short_stage(shape, pc, left, shape.length == loop_fetch_distance ? 1U : loop_decode_distance);
  auto *startup = short_flag(descriptor, short_startup);
  auto *compact = short_flag(descriptor, short_compact_count);
  if (shape.length == 1) {
    auto *last = builder_.CreateICmpEQ(left, builder_.getInt64(1));
    auto *compact_fetch = builder_.CreateSelect(
        last, builder_.getInt64(shape.end + 1), builder_.getInt64(shape.start));
    fetch = builder_.CreateSelect(compact, compact_fetch, fetch);
    // A suppressed decode still names the held short-loop instruction.
    decode = builder_.CreateSelect(compact, builder_.getInt64(shape.start), decode);
  } else if (shape.length == 2) {
    auto *compact_fill = builder_.CreateAnd(startup, compact);
    decode = builder_.CreateSelect(compact_fill, builder_.getInt64(shape.end), decode);
    fetch = builder_.CreateSelect(compact_fill, builder_.getInt64(shape.end + 2), fetch);
  } else if (shape.length == 3) {
    auto *compact_fetch = builder_.CreateSelect(
        startup, builder_.getInt64(shape.end), builder_.getInt64(shape.end + 1));
    fetch = builder_.CreateSelect(compact, compact_fetch, fetch);
    decode = builder_.CreateSelect(compact, builder_.getInt64(shape.end), decode);
  }
  return {.fetch = fetch, .decode = decode};
}

void Lowering::short_loop_observation(Values &values,
                                      const Instruction &op,
                                      const LoopShape &shape,
                                      ShortRetirement retirement) {
  const auto [left, old_status] = retirement;
  auto stages = short_lookahead(op.address, shape, left);
  auto *live = builder_.CreateICmpNE(left, builder_.getInt64(0));
  const auto parent = short_nested_.find(shape.end);
  if (parent != short_nested_.end()) {
    stages = project_short_parent(stages, shape, parent->second);
  } else if (short_parents_.contains(shape.end)) {
    stages.fetch = builder_.CreateSelect(
        live, stages.fetch, builder_.getInt64(shape.end + 1 + loop_fetch_distance));
    stages = overlapping_short_fetches(values, shape, stages, old_status);
  }
  if (shape.length == loop_fetch_distance) { live = builder_.getTrue(); }
  write_short_addresses(stages, live);
}

Lowering::ShortLookahead Lowering::project_short_parent(ShortLookahead stages,
                                                        const LoopShape &child,
                                                        const LoopShape &parent) {
  const auto gap = (parent.end - child.end) & maximum_pc;
  auto *left = short_word(short_parent_remaining);
  auto *distance =
      builder_.CreateAnd(builder_.CreateSub(stages.fetch, builder_.getInt64(child.end)),
                         builder_.getInt64(maximum_pc));
  auto *wrap = builder_.CreateAnd(builder_.CreateICmpUGT(left, builder_.getInt64(gap)),
                                  builder_.CreateICmpUGT(distance, builder_.getInt64(gap)));
  wrap = builder_.CreateAnd(
      wrap, builder_.CreateICmpULE(distance, builder_.getInt64(loop_fetch_distance + 1)));
  const auto address_at = [&](llvm::Value *step) {
    auto *offset = builder_.CreateAdd(step, builder_.getInt64(parent.length - gap));
    auto *body = builder_.CreateAdd(builder_.getInt64(parent.start),
                                    builder_.CreateURem(offset, builder_.getInt64(parent.length)));
    auto *outside =
        builder_.CreateSub(builder_.CreateAdd(builder_.getInt64(parent.end + 1), step), left);
    return builder_.CreateSelect(builder_.CreateICmpULT(step, left), body, outside);
  };
  auto *index = builder_.CreateSub(distance, builder_.getInt64(1));
  auto *decode = builder_.CreateAnd(
      wrap, builder_.CreateICmpUGT(distance, builder_.getInt64(gap + loop_decode_distance)));
  return {.fetch = builder_.CreateSelect(wrap, address_at(index), stages.fetch),
          .decode = builder_.CreateSelect(
              decode,
              address_at(builder_.CreateSub(index, builder_.getInt64(loop_decode_distance))),
              stages.decode)};
}

llvm::Value *Lowering::short_fetch_crossed(ShortFetchWindow window, unsigned offset) {
  auto *limit = builder_.getInt64(offset);
  return builder_.CreateAnd(builder_.CreateICmpULE(window.before, limit),
                            builder_.CreateICmpUGT(window.after, limit));
}

Lowering::ShortLookahead Lowering::redirect_short_parent(ShortLookahead stages,
                                                         const LoopShape &child,
                                                         const LoopShape &parent,
                                                         ShortFetchWindow window,
                                                         llvm::Value *selected) {
  const auto gap = (parent.end - child.end) & maximum_pc;
  auto *delta = builder_.CreateSub(window.after, builder_.getInt64(gap));
  auto *fetch = builder_.CreateAdd(builder_.getInt64(parent.start - 1), delta);
  if (parent.arithmetic()) {
    auto *terminating = refill_at(load(64, loop_refill_slot), parent.end);
    auto *following =
        builder_.CreateAdd(builder_.getInt64(parent.end - loop_decode_distance), delta);
    fetch = builder_.CreateSelect(terminating, following, fetch);
  }
  constexpr unsigned normal_decode_lag = loop_decode_distance;
  const unsigned decode_lag = child.length == loop_fetch_distance ? 3U : normal_decode_lag;
  auto *decoder_redirected =
      builder_.CreateAnd(selected, builder_.CreateICmpUGT(delta, builder_.getInt64(decode_lag)));
  auto *decode = builder_.CreateSub(builder_.CreateAdd(builder_.getInt64(parent.start), delta),
                                    builder_.getInt64(decode_lag + 1));
  return {.fetch = builder_.CreateSelect(selected, fetch, stages.fetch),
          .decode = builder_.CreateSelect(decoder_redirected, decode, stages.decode)};
}

Lowering::ShortLookahead Lowering::overlapping_short_fetches(Values &values,
                                                             const LoopShape &shape,
                                                             ShortLookahead stages,
                                                             llvm::Value *old_status) {
  auto *distance =
      builder_.CreateAnd(builder_.CreateSub(stages.fetch, builder_.getInt64(shape.end)),
                         builder_.getInt64(maximum_pc));
  auto *linear = builder_.CreateICmpULE(distance, builder_.getInt64(loop_fetch_distance + 1));
  const ShortFetchWindow window{.before = short_word(short_fetch_progress),
                                .after =
                                    builder_.CreateSelect(linear, distance, builder_.getInt64(0)),
                                .old_status = old_status};
  write_short(short_fetch_progress, window.after);
  llvm::Value *redirected = builder_.getFalse();
  for (const auto &parent : short_parents_.at(shape.end)) {
    const auto gap = (parent.end - shape.end) & maximum_pc;
    auto *reachable = builder_.CreateNot(redirected);
    llvm::Value *age = nullptr;
    if (lce_endpoints_.contains(parent.end)) {
      // The remaining linear fetches are clocks after this sample, including
      // clocks inside a held or draining short instruction.
      auto *after_sample = builder_.CreateSub(window.after, builder_.getInt64(gap));
      age = builder_.CreateBinaryIntrinsic(
          llvm::Intrinsic::umin, after_sample, builder_.getInt64(lce_branch_age));
    }
    loop_fetch(values,
               parent,
               builder_.CreateAnd(reachable, short_fetch_crossed(window, gap)),
               false,
               age);
    const auto frame = active_loop(values, parent);
    auto *repeat = builder_.CreateAnd(frame.matches, repeat_scheduled(frame));
    llvm::Value *selection = repeat;
    if (parent.arithmetic()) {
      const auto sample = gap + loop_decode_distance;
      auto *at_execute = builder_.CreateICmpEQ(window.before, builder_.getInt64(sample));
      auto *status = builder_.CreateSelect(at_execute, window.old_status, values[status_value]);
      auto *sampled = builder_.CreateAnd(builder_.CreateAnd(reachable, repeat),
                                         short_fetch_crossed(window, sample));
      loop_test(values, status, parent, sampled);
      selection = builder_.CreateOr(repeat, refill_at(load(64, loop_refill_slot), parent.end));
    }
    auto *selected =
        builder_.CreateAnd(builder_.CreateAnd(reachable, selection),
                           builder_.CreateICmpUGT(window.after, builder_.getInt64(gap)));
    stages = redirect_short_parent(stages, shape, parent, window, selected);
    redirected = builder_.CreateOr(redirected, selected);
  }
  return stages;
}

llvm::Value *Lowering::short_loop_refill(const LoopShape &shape, llvm::Value *done) {
  auto *descriptor = short_word(short_descriptor);
  llvm::Value *refill = builder_.getInt64(0);
  if (shape.counted()) {
    auto *compact = short_flag(descriptor, short_compact_count);
    refill = builder_.CreateSelect(builder_.CreateAnd(compact, done),
                                   builder_.getInt64(shape.length == 2 ? 2 : 3),
                                   builder_.getInt64(0));
  } else if (shape.arithmetic()) {
    refill =
        builder_.CreateSelect(done, builder_.getInt64(loop_decode_distance), builder_.getInt64(0));
  }
  if (shape.length == 3) { return refill; }
  auto *startup = short_flag(descriptor, short_startup);
  return builder_.CreateAdd(refill, builder_.CreateZExt(startup, builder_.getInt64Ty()));
}

void Lowering::short_loop_retirement(Values values,
                                     const Instruction &op,
                                     const LoopShape &shape,
                                     llvm::Value *old_status,
                                     bool partial) {
  auto *remaining = short_word(short_remaining);
  auto *descriptor = short_word(short_descriptor);
  auto *active = short_active(shape, remaining);
  auto *advance = llvm::BasicBlock::Create(module_.getContext(), "short.retire", function_);
  auto *ordinary = llvm::BasicBlock::Create(module_.getContext(), "short.inactive", function_);
  builder_.CreateCondBr(active, advance, ordinary);
  builder_.SetInsertPoint(advance);
  auto *depth = values[pc_stack_depth_value];
  auto *next_descriptor =
      builder_.CreateAnd(descriptor, builder_.getInt64(~(short_startup | short_call_exit)));
  auto *left = short_words_after(remaining, shape);
  const bool arithmetic_sample = shape.arithmetic() && op.address == shape.condition_pc();
  if (shape.counted()) {
    short_loop_counter(values, op.address, shape, descriptor);
  } else if (arithmetic_sample) {
    left = short_loop_test(values, shape, {.left = left, .old_status = old_status});
  }
  const bool counted_tag = shape.counted() && shape.length == 3 && next_pc(op) == shape.end;
  const auto following = op.address == shape.end ? shape.start : next_pc(op);
  if ((counted_tag || arithmetic_sample) && reentry_calls_.contains(following)) {
    next_descriptor = short_exit_descriptor(next_descriptor, values, depth);
  }
  auto *done = builder_.CreateICmpEQ(left, builder_.getInt64(0));
  auto *refill = short_loop_refill(shape, done);
  auto *stalled = builder_.CreateICmpNE(refill, builder_.getInt64(0));
  values[hazard_value] = builder_.CreateAnd(values[hazard_value], builder_.CreateNot(stalled));
  auto *older =
      builder_.CreateAnd(builder_.CreateShl(values[dag_history_value], builder_.getInt32(8)),
                         builder_.getInt32(history_older_mask));
  values[dag_history_value] = builder_.CreateSelect(
      stalled,
      builder_.CreateSelect(
          builder_.CreateICmpEQ(refill, builder_.getInt64(1)), older, builder_.getInt32(0)),
      values[dag_history_value]);
  auto *cycles = builder_.CreateAdd(builder_.CreateAdd(builder_.getInt64(1), refill),
                                    values[extra_cycles_value]);
  short_loop_observation(values, op, shape, {.left = left, .old_status = old_status});
  credit(values, builder_.getInt64(1), cycles, partial);
  write_short(short_remaining, left);
  write_short(short_descriptor, next_descriptor);
  if (op.address == shape.end) {
    if (lce_endpoints_.contains(shape.end)) { clear_counted_tail(shape, done); }
    auto *repeat = llvm::BasicBlock::Create(module_.getContext(), "short.repeat", function_);
    auto *finished = llvm::BasicBlock::Create(module_.getContext(), "short.finished", function_);
    builder_.CreateCondBr(done, finished, repeat);
    builder_.SetInsertPoint(repeat);
    finish(values, shape.start, partial);
    builder_.SetInsertPoint(finished);
    if (const auto parent = short_nested_.find(shape.end); parent != short_nested_.end()) {
      resume_short_parent(values, shape, parent->second);
    }
  }
  finish(values, next_pc(op), partial);
  builder_.SetInsertPoint(ordinary);
}

void Lowering::loop_status(Values &values, llvm::Value *depth) {
  constexpr auto empty_mask = 1U << 26;
  auto *sticky = builder_.CreateExtractElement(values[sticky_value], builder_.getInt32(0));
  sticky = builder_.CreateAnd(sticky, builder_.getInt32(~empty_mask));
  sticky =
      builder_.CreateOr(sticky,
                        builder_.CreateSelect(builder_.CreateICmpEQ(depth, builder_.getInt32(0)),
                                              builder_.getInt32(empty_mask),
                                              builder_.getInt32(0)));
  values[sticky_value] =
      builder_.CreateInsertElement(values[sticky_value], sticky, builder_.getInt32(0));
}

LoopFrame Lowering::active_loop(const Values &values, const LoopShape &shape) {
  auto *depth = builder_.CreateZExt(load(8, loop_address_depth_slot), builder_.getInt32Ty());
  auto *occupied =
      builder_.CreateAnd(builder_.CreateICmpNE(depth, builder_.getInt32(0)),
                         builder_.CreateICmpULE(depth, builder_.getInt32(loop_stack_capacity)));
  auto *index = builder_.CreateSelect(
      occupied, builder_.CreateSub(depth, builder_.getInt32(1)), builder_.getInt32(0));
  auto *metadata = builder_.CreateLoad(builder_.getInt32Ty(), loop_cell(loop_frames_slot, index));
  auto *address = builder_.CreateLoad(builder_.getInt32Ty(), loop_cell(loop_address_slot, index));
  auto *pc_index = builder_.CreateAnd(metadata, builder_.getInt32(loop_pc_index_mask));
  auto *pc = builder_.CreateLoad(builder_.getInt32Ty(), stack_cell(pc_index));
  auto *live = builder_.CreateICmpNE(
      builder_.CreateAnd(metadata, builder_.getInt32(loop_frame_live)), builder_.getInt32(0));
  auto *pc_depth = builder_.CreateAdd(pc_index, builder_.getInt32(1));
  auto *matches =
      builder_.CreateAnd(builder_.CreateICmpEQ(address, builder_.getInt32(shape.address_word())),
                         builder_.CreateICmpEQ(pc, builder_.getInt32(shape.start)));
  matches =
      builder_.CreateAnd(matches, builder_.CreateICmpEQ(pc_depth, values[pc_stack_depth_value]));
  return {.index = index,
          .metadata = metadata,
          .matches = builder_.CreateAnd(matches, builder_.CreateAnd(occupied, live))};
}

void Lowering::loop_pop(Values &values, const LoopFrame &frame) {
  auto *depth = builder_.CreateTrunc(frame.index, builder_.getInt8Ty());
  store(depth, loop_address_depth_slot);
  store(depth, loop_counter_depth_slot);
  builder_.CreateStore(builder_.getInt32(0), loop_cell(loop_frames_slot, frame.index));
  values[pc_stack_depth_value] =
      builder_.CreateSub(values[pc_stack_depth_value], builder_.getInt32(1));
  stack_status(values);
  loop_status(values, frame.index);
}

std::size_t Lowering::counted_tail_slot_for(const LoopShape &shape) const {
  if (shape.short_body()) {
    return short_parent_tails_.contains(shape.end) ? short_parent_tail_slot
                                                   : short_counted_tail_slot;
  }
  const auto index = long_tail_indices_.find(shape.end);
  return counted_tail_slot + (index == long_tail_indices_.end() ? 0 : index->second);
}

std::uint64_t Lowering::counted_tail_word(const LoopShape &shape) const {
  const auto index = shape.short_body() ? 0 : counted_tail_slot_for(shape) - counted_tail_slot;
  return counted_loop_tail | (std::uint64_t{index} << counted_tail_index_shift) |
         (std::uint64_t{shape.end} << loop_refill_end_shift) | shape.start;
}

llvm::Value *Lowering::counted_tail_matches(const LoopShape &shape) {
  if (!shape.counted()) { return builder_.getFalse(); }
  auto *saved = load(64, counted_tail_slot_for(shape));
  if (lce_endpoints_.contains(shape.end)) {
    saved = builder_.CreateAnd(saved, builder_.getInt64(~lce_clock_mask));
  }
  return builder_.CreateICmpEQ(saved, builder_.getInt64(counted_tail_word(shape)));
}

void Lowering::save_counted_tail(const LoopShape &shape, llvm::Value *age, bool returning) {
  const bool tracked = lce_endpoints_.contains(shape.end);
  const auto marker = tracked && !returning ? lce_before_credit : 0;
  llvm::Value *tail = builder_.getInt64(counted_tail_word(shape) | marker);
  if (tracked) {
    tail = builder_.CreateOr(tail, builder_.CreateShl(age, builder_.getInt64(lce_age_shift)));
  }
  store(tail, counted_tail_slot_for(shape));
}

void Lowering::clear_counted_tail(const LoopShape &shape, llvm::Value *enabled) {
  auto *clear = counted_tail_matches(shape);
  if (enabled != nullptr) { clear = builder_.CreateAnd(clear, enabled); }
  const auto slot = counted_tail_slot_for(shape);
  store(builder_.CreateSelect(clear, builder_.getInt64(0), load(64, slot)), slot);
}

llvm::Value *Lowering::abort_target_in_parent(llvm::Value *target,
                                              const Values &values,
                                              llvm::Value *already_exited) {
  auto *remove = builder_.CreateZExt(builder_.CreateNot(already_exited), builder_.getInt32Ty());
  auto *depth = builder_.CreateSub(
      builder_.CreateZExt(load(8, loop_address_depth_slot), builder_.getInt32Ty()), remove);
  auto *occupied =
      builder_.CreateAnd(builder_.CreateICmpNE(depth, builder_.getInt32(0)),
                         builder_.CreateICmpULE(depth, builder_.getInt32(loop_stack_capacity)));
  auto *index = builder_.CreateSelect(
      occupied, builder_.CreateSub(depth, builder_.getInt32(1)), builder_.getInt32(0));
  auto *metadata = builder_.CreateLoad(builder_.getInt32Ty(), loop_cell(loop_frames_slot, index));
  auto *parent_pc_slot = builder_.CreateAnd(metadata, builder_.getInt32(loop_pc_index_mask));
  auto *parent_pc_depth = builder_.CreateAdd(parent_pc_slot, builder_.getInt32(1));
  auto *remaining_pc_depth = builder_.CreateSub(values[pc_stack_depth_value], remove);
  // A hardware loop in a caller is suspended behind a CALL stack slot.
  auto *same_scope = builder_.CreateICmpEQ(parent_pc_depth, remaining_pc_depth);
  auto *live = builder_.CreateICmpNE(
      builder_.CreateAnd(metadata, builder_.getInt32(loop_frame_live)), builder_.getInt32(0));
  auto *parent = builder_.CreateAnd(occupied, builder_.CreateAnd(same_scope, live));
  auto *start = builder_.CreateLoad(builder_.getInt32Ty(), stack_cell(parent_pc_slot));
  auto *end = builder_.CreateLoad(builder_.getInt32Ty(), loop_cell(loop_address_slot, index));
  auto *mask = builder_.getInt32(maximum_pc);
  auto *offset = builder_.CreateAnd(builder_.CreateSub(target, start), mask);
  auto *last = builder_.CreateAnd(builder_.CreateSub(end, start), mask);
  auto *term =
      builder_.CreateAnd(builder_.CreateLShr(end, builder_.getInt32(immediate_count_shift)),
                         builder_.getInt32(true_condition));
  auto *arithmetic =
      builder_.CreateAnd(builder_.CreateICmpNE(term, builder_.getInt32(loop_counter_expired)),
                         builder_.CreateICmpNE(term, builder_.getInt32(true_condition)));
  // A nested LA cannot skip to the endpoint of an arithmetic parent.
  auto *within = builder_.CreateOr(
      builder_.CreateICmpULT(offset, last),
      builder_.CreateAnd(builder_.CreateNot(arithmetic), builder_.CreateICmpEQ(offset, last)));
  return builder_.CreateOr(builder_.CreateNot(parent), within);
}

void Lowering::guard_loop_abort(const Values &values,
                                const Instruction &op,
                                llvm::Value *lanes,
                                bool partial) {
  const auto &shape = aborts_.at(op.address);
  const auto frame = active_loop(values, shape);
  auto *already_exited = counted_tail_matches(shape);
  auto *target = jump_target(values, op);
  auto *offset = builder_.CreateAnd(builder_.CreateSub(target, builder_.getInt32(shape.start)),
                                    builder_.getInt32(maximum_pc));
  auto *outside = builder_.CreateICmpUGE(offset, builder_.getInt32(shape.length));
  auto *one_level =
      builder_.CreateAnd(outside, abort_target_in_parent(target, values, already_exited));
  auto *stack_matches =
      builder_.CreateICmpEQ(load(8, loop_address_depth_slot), load(8, loop_counter_depth_slot));
  auto *owned = builder_.CreateAnd(stack_matches, builder_.CreateOr(frame.matches, already_exited));
  auto *allowed = builder_.CreateOr(builder_.CreateNot(branch_taken(lanes)),
                                    builder_.CreateAnd(owned, one_level));
  stack_guard(values, op, allowed, partial, NativeExit::invalid_loop_abort);
}

void Lowering::retire_loop_abort(Values &values, const LoopShape &shape) {
  const auto frame = active_loop(values, shape);
  auto *pop = llvm::BasicBlock::Create(module_.getContext(), "loop.abort.pop", function_);
  auto next = node("loop.abort.continue");
  next.incoming(values, builder_.GetInsertBlock());
  builder_.CreateCondBr(frame.matches, pop, next.block);
  builder_.SetInsertPoint(pop);
  loop_pop(values, frame);
  jump(next, values);
  builder_.SetInsertPoint(next.block);
  values = next.values();
  if (shape.counted()) {
    // If the count already popped this loop, consume its tail without
    // popping the parent or applying the discarded loop's redirect.
    clear_counted_tail(shape);
  }
}

void Lowering::loop_fetch(Values &values,
                          const LoopShape &shape,
                          llvm::Value *enabled,
                          bool returning,
                          llvm::Value *age) {
  const auto frame = active_loop(values, shape);
  auto *pending = builder_.CreateAnd(frame.metadata, builder_.getInt32(loop_repeat_scheduled));
  auto *consider = llvm::BasicBlock::Create(module_.getContext(), "loop.fetch.end", function_);
  auto continuation = node("loop.fetch.continue");
  continuation.incoming(values, builder_.GetInsertBlock());
  builder_.CreateCondBr(
      builder_.CreateAnd(
          enabled,
          builder_.CreateAnd(frame.matches, builder_.CreateICmpEQ(pending, builder_.getInt32(0)))),
      consider,
      continuation.block);
  builder_.SetInsertPoint(consider);
  if (shape.counted()) {
    auto *cell = loop_cell(loop_counter_slot, frame.index);
    auto *count = builder_.CreateLoad(builder_.getInt32Ty(), cell);
    auto *remaining = builder_.CreateSub(count, builder_.getInt32(1));
    builder_.CreateStore(remaining, cell);
    auto *repeat = llvm::BasicBlock::Create(module_.getContext(), "loop.fetch.repeat", function_);
    auto *terminate = llvm::BasicBlock::Create(module_.getContext(), "loop.fetch.exit", function_);
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(remaining, builder_.getInt32(0)), terminate, repeat);
    builder_.SetInsertPoint(terminate);
    auto popped = values;
    loop_pop(popped, frame);
    if (shape.has_reentry || shape.has_tail_abort || lce_endpoints_.contains(shape.end)) {
      save_counted_tail(shape, age != nullptr ? age : builder_.getInt64(1), returning);
    }
    jump(continuation, popped);
    builder_.SetInsertPoint(repeat);
  }
  auto *scheduled = builder_.CreateOr(frame.metadata, builder_.getInt32(loop_repeat_scheduled));
  builder_.CreateStore(scheduled, loop_cell(loop_frames_slot, frame.index));
  jump(continuation, values);
  builder_.SetInsertPoint(continuation.block);
  values = continuation.values();
}

void Lowering::loop_test(Values &values,
                         llvm::Value *old_status,
                         const LoopShape &shape,
                         llvm::Value *enabled) {
  const auto frame = active_loop(values, shape);
  auto before = values;
  before[status_value] = old_status;
  auto *done = branch_taken(condition(before, shape.term));
  auto *leave = llvm::BasicBlock::Create(module_.getContext(), "loop.condition.exit", function_);
  auto next = node("loop.condition.continue");
  next.incoming(values, builder_.GetInsertBlock());
  builder_.CreateCondBr(
      builder_.CreateAnd(enabled, builder_.CreateAnd(frame.matches, done)), leave, next.block);
  builder_.SetInsertPoint(leave);
  auto popped = values;
  loop_pop(popped, frame);
  const auto refill =
      loop_refill_live | (std::uint64_t{shape.end} << loop_refill_end_shift) | shape.start;
  store(builder_.getInt64(refill), loop_refill_slot);
  jump(next, popped);
  builder_.SetInsertPoint(next.block);
  values = next.values();
}

void Lowering::loop_refill(Values &values, const Instruction &op) {
  auto *pending = load(64, loop_refill_slot);
  auto *end =
      builder_.CreateAnd(builder_.CreateLShr(pending, builder_.getInt64(loop_refill_end_shift)),
                         builder_.getInt64(maximum_pc));
  auto *active = builder_.CreateICmpNE(
      builder_.CreateAnd(pending, builder_.getInt64(loop_refill_live)), builder_.getInt64(0));
  auto *at_end = builder_.CreateICmpEQ(end, builder_.getInt64(op.address));
  auto *here = builder_.CreateAnd(active, at_end);
  store(builder_.CreateSelect(at_end, builder_.getInt64(0), pending), loop_refill_slot);
  values[extra_cycles_value] = builder_.CreateAdd(
      values[extra_cycles_value],
      builder_.CreateSelect(here, builder_.getInt64(loop_decode_distance), builder_.getInt64(0)));
  values[hazard_value] = builder_.CreateAnd(values[hazard_value], builder_.CreateNot(here));
  values[dag_history_value] =
      builder_.CreateSelect(here, builder_.getInt32(0), values[dag_history_value]);
}

void Lowering::check_loop_end(const Values &values, const Instruction &op, bool partial) {
  const auto shape = hardware_ends_.find(op.address);
  if (shape == hardware_ends_.end()) { return; }
  const auto frame = active_loop(values, shape->second);
  auto *known = builder_.CreateOr(builder_.CreateNot(frame.matches), repeat_scheduled(frame));
  stack_guard(values, op, known, partial, NativeExit::unsupported_loop_stack);
}

llvm::Value *Lowering::repeat_scheduled(const LoopFrame &frame) {
  auto *pending = builder_.CreateAnd(frame.metadata, builder_.getInt32(loop_repeat_scheduled));
  return builder_.CreateICmpNE(pending, builder_.getInt32(0));
}

void Lowering::clear_loop_repeat(const LoopFrame &frame) {
  auto *metadata = builder_.CreateAnd(frame.metadata, builder_.getInt32(~loop_repeat_scheduled));
  builder_.CreateStore(metadata, loop_cell(loop_frames_slot, frame.index));
}

void Lowering::loop_backedge(const Values &values, const LoopShape &shape, bool partial) {
  const auto frame = active_loop(values, shape);
  auto *repeat = llvm::BasicBlock::Create(module_.getContext(), "loop.repeat", function_);
  auto *next = llvm::BasicBlock::Create(module_.getContext(), "loop.next", function_);
  builder_.CreateCondBr(builder_.CreateAnd(frame.matches, repeat_scheduled(frame)), repeat, next);
  builder_.SetInsertPoint(repeat);
  clear_loop_repeat(frame);
  auto turn = values;
  credit(turn,
         builder_.getInt64(1),
         builder_.CreateAdd(builder_.getInt64(1), values[extra_cycles_value]),
         partial);
  finish(turn, shape.start, partial);
  builder_.SetInsertPoint(next);
}

void Lowering::retire_short_observation(std::uint32_t pc) {
  if (!short_successors_.contains(pc)) { return; }
  auto *live = builder_.CreateICmpNE(short_word(short_remaining), builder_.getInt64(0));
  write_short(short_addresses,
              builder_.CreateSelect(live, short_word(short_addresses), builder_.getInt64(0)));
}

llvm::Value *Lowering::refill_at(llvm::Value *refill, std::uint32_t child_end) {
  constexpr auto mask = loop_refill_live | (std::uint64_t{maximum_pc} << loop_refill_end_shift);
  const auto signature = loop_refill_live | (std::uint64_t{child_end} << loop_refill_end_shift);
  return builder_.CreateICmpEQ(builder_.CreateAnd(refill, builder_.getInt64(mask)),
                               builder_.getInt64(signature));
}

void Lowering::overlapping_loop_fetches(Values &values,
                                        std::uint32_t pc,
                                        llvm::Value *refill,
                                        bool whole_turn) {
  const auto overlaps = overlapping_fetches_.find(pc);
  if (overlaps == overlapping_fetches_.end()) { return; }
  llvm::Value *redirected = builder_.getFalse();
  for (const auto &[child_end, parent] : overlaps->second) {
    auto *enabled =
        builder_.CreateAnd(builder_.CreateNot(redirected), refill_at(refill, child_end));
    const auto gap = (parent.end - child_end) & maximum_pc;
    // Gaps 2 through 4 sample during the endpoint or the two refill clocks.
    // Gap 1 samples one word earlier. A whole-turn grant already retired it.
    const auto early_age = whole_turn ? lce_branch_age : 1U;
    const auto age = gap == 1 ? early_age : loop_fetch_distance + 1 - gap;
    loop_fetch(values, parent, enabled, false, builder_.getInt64(age));
    const auto frame = active_loop(values, parent);
    auto *repeat =
        builder_.CreateAnd(enabled, builder_.CreateAnd(frame.matches, repeat_scheduled(frame)));
    redirected = builder_.CreateOr(redirected, repeat);
    if (parent.arithmetic() && gap == loop_decode_distance) {
      // Fetch1 reaches the parent's second word on the last child refill
      // clock. Decode sees this decision before any more guest work retires.
      loop_test(values, values[status_value], parent, repeat);
    }
  }
}

void Lowering::loop_retirement(Values &values,
                               const Instruction &op,
                               llvm::Value *old_status,
                               bool partial) {
  const auto body = short_bodies_.find(op.address);
  if (body != short_bodies_.end()) {
    short_loop_retirement(values, op, body->second, old_status, partial);
  }
  const auto end = hardware_ends_.find(op.address);
  const auto fetch = loop_fetches_.find(op.address);
  const bool shifted = fetch != loop_fetches_.end() && shifted_fetches_.contains(fetch->second.end);
  const bool overlapping = overlapping_fetches_.contains(op.address);
  auto *refill = shifted || overlapping ? load(64, loop_refill_slot) : builder_.getInt64(0);
  if (end != hardware_ends_.end()) {
    if (end->second.arithmetic()) { loop_refill(values, op); }
    if (end->second.counted() && (end->second.has_reentry || end->second.has_tail_abort ||
                                  lce_endpoints_.contains(op.address))) {
      clear_counted_tail(end->second);
    }
  }
  if (fetch != loop_fetches_.end()) {
    llvm::Value *enabled = builder_.getTrue();
    if (shifted) {
      enabled = builder_.CreateNot(
          builder_.CreateOr(refill_at(refill, op.address), refill_at(refill, next_pc(op))));
    }
    loop_fetch(values, fetch->second, enabled);
  }
  const auto test = loop_tests_.find(op.address);
  if (test != loop_tests_.end()) {
    auto *enabled = builder_.CreateNot(refill_at(refill, op.address));
    loop_test(values, old_status, test->second, enabled);
  }
  if (overlapping) { overlapping_loop_fetches(values, op.address, refill); }
  if (end != hardware_ends_.end()) { loop_backedge(values, end->second, partial); }
}

void Lowering::plan_hardware_grants(const LoopMap &shapes) {
  for (const auto &[end, shape] : shapes) {
    const auto start = addresses_.find(shape.start);
    const auto last = addresses_.find(end);
    if (start == addresses_.end() || last == addresses_.end() || last->second < start->second ||
        last->second - start->second + 1 != shape.length) {
      continue;
    }
    const auto body = std::span(instructions_).subspan(start->second, shape.length);
    if (!std::ranges::all_of(body, [this, &shape](const Instruction &op) {
          const auto fetch = loop_fetches_.find(op.address);
          const auto test = loop_tests_.find(op.address);
          return !branch(op) && !loop_setup(op) && !delay_boundary(op.address) &&
                 (test == loop_tests_.end() || test->second.end == shape.end ||
                  overlapping_refill(shape, test->second) ||
                  overlapping_short_tail(shape, test->second)) &&
                 (op.address == shape.end || !hardware_ends_.contains(op.address)) &&
                 (fetch == loop_fetches_.end() || fetch->second.end == shape.end ||
                  overlapping_refill(shape, fetch->second) ||
                  overlapping_short_tail(shape, fetch->second) ||
                  (shape.counted() && fetch->second.counted() &&
                   overlapping_tail(shape, fetch->second))) &&
                 op.type != index_modify_type && !dag_register(loaded_register(op));
        })) {
      continue;
    }
    hardware_grants_.emplace(shape.start, shape);
  }
}

void Lowering::admit_hardware_turns(const Values &values, const LoopShape &shape) {
  const auto frame = active_loop(values, shape);
  auto *length = builder_.getInt64(shape.length);
  auto *instructions = builder_.CreateUDiv(values[instruction_remaining_value], length);
  auto *available_cycles = values[cycle_remaining_value];
  if (shape.arithmetic() && !shape.short_body()) {
    // At most one terminating turn in this grant owes a two-cycle refill.
    available_cycles = builder_.CreateBinaryIntrinsic(
        llvm::Intrinsic::usub_sat, available_cycles, builder_.getInt64(loop_decode_distance));
  }
  auto *cycles = builder_.CreateUDiv(available_cycles, length);
  auto *turns = builder_.CreateBinaryIntrinsic(llvm::Intrinsic::umin, instructions, cycles);
  llvm::Value *count = nullptr;
  llvm::Value *steady = builder_.getTrue();
  if (shape.short_body()) {
    steady = builder_.CreateNot(short_flag(short_word(short_descriptor), short_startup));
  }
  if (shape.counted()) {
    count = builder_.CreateLoad(builder_.getInt32Ty(), loop_cell(loop_counter_slot, frame.index));
    // A raw zero represents 2^32 turns. The final turn always takes the exact path.
    llvm::Value *before_last = nullptr;
    if (shape.short_body()) {
      // Leave the fetch that pops the frame and starts draining buffered words to the exact path.
      before_last = builder_.CreateBinaryIntrinsic(
          llvm::Intrinsic::usub_sat, count, builder_.getInt32(shape.short_threshold()));
    } else {
      before_last = builder_.CreateSub(count, builder_.getInt32(1));
    }
    turns = builder_.CreateBinaryIntrinsic(
        llvm::Intrinsic::umin, turns, builder_.CreateZExt(before_last, builder_.getInt64Ty()));
  }
  auto *pending = builder_.CreateAnd(frame.metadata, builder_.getInt32(loop_repeat_scheduled));
  auto *clean = builder_.CreateAnd(
      builder_.CreateAnd(steady, builder_.CreateICmpEQ(pending, builder_.getInt32(0))),
      builder_.CreateICmpEQ(values[dag_history_value], builder_.getInt32(0)));
  auto *admitted = builder_.CreateAnd(
      builder_.CreateAnd(frame.matches, clean),
      builder_.CreateAnd(memory_admitted_, builder_.CreateICmpNE(turns, builder_.getInt64(0))));
  auto body = node("hardware.loop.body");
  auto *fallback = llvm::BasicBlock::Create(module_.getContext(), "hardware.loop.exact", function_);
  auto initial = values;
  initial[loop_grant_value] = turns;
  initial[loop_remaining_value] = turns;
  body.incoming(initial, builder_.GetInsertBlock());
  builder_.CreateCondBr(admitted, body.block, fallback);
  emit_hardware_turns(body, frame, count, shape);
  builder_.SetInsertPoint(fallback);
}

void Lowering::emit_hardware_turns(const Node &body,
                                   const LoopFrame &frame,
                                   llvm::Value *count,
                                   const LoopShape &shape) {
  const auto start = addresses_.at(shape.start);
  builder_.SetInsertPoint(body.block);
  auto values = body.values();
  values[dag_history_value] = builder_.getInt32(0);
  values[extra_cycles_value] = builder_.getInt64(0);
  const auto words = std::span(instructions_).subspan(start, shape.length);
  llvm::Value *done = builder_.getFalse();
  for (const auto &op : words) {
    builder_.SetCurrentDebugLocation(
        llvm::DILocation::get(module_.getContext(), op.address + 1, 0, function_->getSubprogram()));
    if (shape.arithmetic() && op.address == shape.condition_pc()) {
      done = branch_taken(condition(values, shape.term));
    }
    instruction(values, op, false);
  }
  auto *left = builder_.CreateSub(values[loop_remaining_value], builder_.getInt64(1));
  values[loop_remaining_value] = left;
  auto *account =
      llvm::BasicBlock::Create(module_.getContext(), "hardware.loop.account", function_);
  body.incoming(values, builder_.GetInsertBlock());
  auto *again = builder_.CreateAnd(builder_.CreateNot(done),
                                   builder_.CreateICmpNE(left, builder_.getInt64(0)));
  builder_.CreateCondBr(again, body.block, account);
  builder_.SetInsertPoint(account);
  auto *turns = builder_.CreateSub(values[loop_grant_value], left);
  auto *retired = builder_.CreateMul(turns, builder_.getInt64(shape.length));
  if (count != nullptr) {
    auto *updated = builder_.CreateSub(count, builder_.CreateTrunc(turns, builder_.getInt32Ty()));
    builder_.CreateStore(updated, loop_cell(loop_counter_slot, frame.index));
  }
  if (shape.short_body() && shape.counted()) {
    // At a steady header, a whole turn leaves FADDR and DADDR at the same body addresses.
    write_short(short_remaining, builder_.CreateSub(short_word(short_remaining), retired));
  }
  if (!shape.arithmetic()) {
    credit(values, retired, retired, false);
    jump(partial_[start], values);
    return;
  }
  auto *finished =
      llvm::BasicBlock::Create(module_.getContext(), "hardware.loop.finished", function_);
  auto *prefix = llvm::BasicBlock::Create(module_.getContext(), "hardware.loop.prefix", function_);
  builder_.CreateCondBr(done, finished, prefix);
  builder_.SetInsertPoint(prefix);
  auto tail = values;
  credit(tail, retired, retired, false);
  jump(partial_[start], tail);
  builder_.SetInsertPoint(finished);
  loop_pop(values, frame);
  if (shape.short_body()) {
    auto *buffered = builder_.getInt64(shape.short_condition_tail());
    write_short(short_remaining, buffered);
    short_loop_observation(
        values, words.back(), shape, {.left = buffered, .old_status = values[status_value]});
    credit(values, retired, retired, false);
    jump(partial_[start], values);
    return;
  }
  values[hazard_value] = builder_.getFalse();
  auto *refill = builder_.getInt64(
      loop_refill_live | (std::uint64_t{shape.end} << loop_refill_end_shift) | shape.start);
  overlapping_loop_fetches(values, (shape.end - 1) & maximum_pc, refill, true);
  overlapping_loop_fetches(values, shape.end, refill);
  credit(
      values, retired, builder_.CreateAdd(retired, builder_.getInt64(loop_decode_distance)), false);
  readmit(values, (shape.end + 1) & maximum_pc);
}

void Lowering::finish(Values values, std::uint32_t next, bool partial) {
  const auto target = addresses_.find(next);
  if (target != addresses_.end()) {
    const auto i = target->second;
    if (hardware_grants_.contains(next)) {
      jump(admission_[i], values);
    } else if (sequencer_boundary(next)) {
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

void Lowering::readmit(const Values &values, std::uint32_t next) {
  const auto successor = addresses_.find(next);
  if (successor == addresses_.end()) {
    finish(values, next, false);
  } else {
    jump(admission_[successor->second], values);
  }
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
    if (sequencer_boundary(op.address)) {
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
    for (const auto reg : {source_register(op), destination}) {
      if (dag_register(reg)) { load_dag(dag_number(reg)); }
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
        sequencer_boundary(op.address)) {
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
  auto *lanes = condition(values, op);
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
  readmit(values, next_pc(op));
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
    if (sequencer_boundary(op.address)) { return std::nullopt; }
    const bool is_branch = branch(op);
    const auto next = grant_successors(op);
    BlockCost suffix{.instructions = 0, .cycles = 0};
    for (const auto address : next) {
      const auto found = addresses_.find(address);
      if (found == addresses_.end() || sequencer_boundary(address)) { continue; }
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
  finish_loop_return(values, op);
  if (const auto hardware = hardware_grants_.find(op.address); hardware != hardware_grants_.end()) {
    const auto source = builder_.getCurrentDebugLocation();
    admit_hardware_turns(values, hardware->second);
    builder_.SetCurrentDebugLocation(source);
  }
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
  fast_entry_[i].incoming(hot, builder_.GetInsertBlock());
  auto tail = values;
  tail[tail_value] = builder_.getInt1(loop != loop_headers_.end() || grant.has_value());
  partial_[i].incoming(tail, builder_.GetInsertBlock());
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
  check_loop_end(values, op, false);
  instruction(values, op, false);
  // Admitted memory accesses have no resumable exit inside this block.
  if (dead_status) { values[status_value] = old_status; }
  if (!branch(op)) {
    retire_pipeline(values, pipeline);
    loop_retirement(values, op, old_status, false);
  }
  if (costs_[i].instructions > 1) {
    jump(fast_[i + 1], values);
  } else {
    complete(values, op, false, lanes, pipeline, old_status);
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
  check_loop_end(values, op, true);
  lanes = branch_lanes(values, op, true);
  pipeline = {.stall = pipeline_stall(values, op), .tag = loaded_tag(values, op)};
  old_status = values[status_value];
  instruction(values, op, true);
  if (!branch(op)) {
    retire_pipeline(values, pipeline);
    loop_retirement(values, op, old_status, true);
  }
  if (costs_[i].instructions > 1) {
    credit(values,
           builder_.getInt64(1),
           builder_.CreateAdd(builder_.getInt64(1), values[extra_cycles_value]),
           true);
    jump(partial_[i + 1], values);
  } else {
    complete(values, op, true, lanes, pipeline, old_status);
  }
}

void Lowering::emit(const LoopMap &shapes) {
  has_plain_loop_returns_ = std::ranges::any_of(reentry_calls_, [](const auto &site) {
    return (loop_return_payload(site.first, site.second) & return_loop_fetch) != 0;
  });
  has_loop_sample_returns_ =
      has_plain_loop_returns_ || std::ranges::any_of(reentry_calls_, [](const auto &site) {
        return (loop_return_payload(site.first, site.second) & return_loop_condition) != 0;
      });
  has_short_loop_returns_ = std::ranges::any_of(
      reentry_calls_, [](const auto &site) { return site.second.short_body(); });
  for (const auto &op : instructions_) {
    if (!delayed(op)) { continue; }
    delay_slots_.insert(next_pc(op));
    delay_slots_.insert(next_pc(op, delay_slot_count));
    delay_ends_.emplace(next_pc(op, delay_slot_count), &op);
  }
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
  plan_hardware_loops(shapes);
  plan_hardware_grants(shapes);
  stack_written_ = !short_bodies_.empty() || !loop_setups_.empty() || !hardware_ends_.empty() ||
                   !loop_fetches_.empty() || !loop_tests_.empty() ||
                   std::ranges::any_of(instructions_, [this](const auto &op) {
                     return op.type == return_branch_type || call_branch_instruction(op) ||
                            loop_return_at(op.address, reentry_calls_) != nullptr;
                   });
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
  for (const auto reg : {source_register(instruction), loaded_register(instruction)}) {
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
  if (instruction.type == register_move_type || instruction.type == immediate_move_type ||
      register_count(instruction)) {
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

std::uint32_t context_mask(const std::vector<Instruction> &instructions, const LoopPlan &plan) {
  std::uint32_t mask{};
  for (const auto &instruction : instructions) {
    mask |= instruction_context(instruction);
    const auto condition = plan.shapes.find(next_pc(instruction, loop_decode_distance));
    const auto endpoint = plan.shapes.find(instruction.address);
    const bool short_sample = endpoint != plan.shapes.end() && endpoint->second.short_body() &&
                              endpoint->second.arithmetic() &&
                              endpoint->second.condition_pc() == instruction.address;
    if ((condition != plan.shapes.end() && condition->second.arithmetic()) || short_sample ||
        (loop_setup(instruction) && loop_shape(instruction).arithmetic())) {
      mask |= simd_mode | saturation_mode;
    }
    const auto *resumed = loop_return_at(instruction.address, plan.reentry_calls);
    if (resumed == nullptr || !resumed->arithmetic()) { continue; }
    const auto sample = resumed->condition_pc();
    const auto resume = sample == resumed->end ? resumed->start : ((sample + 1) & maximum_pc);
    if (instruction.address == resume) { mask |= simd_mode | saturation_mode; }
  }
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
      if (loop_setup(instruction)) {
        reject(instruction, identity, FaultReason::unsupported_instruction, "DO in a delay slot");
      }
      if (branch(instruction) && (i != 1 || !complementary_jumps(op, instruction))) {
        reject(instruction, identity, FaultReason::invalid_encoding, "branch in a delay slot");
      }
    }
  }
}

void validate_loop_call(LoopPlan &plan,
                        LoopShape &shape,
                        const Instruction &op,
                        std::string_view identity) {
  const bool short_site = shape.length >= 2;
  if ((shape.short_body() && !short_site) || !call_branch_instruction(op) || delayed(op)) {
    reject(op, identity, FaultReason::unsupported_instruction, "branch near the DO endpoint");
  }
  shape.has_reentry = true;
  if (!plan.reentry_calls.emplace(op.address, shape).second) {
    reject(op, identity, FaultReason::unsupported_instruction, "overlapping loop CALL");
  }
}

void validate_nested_loop(const InstructionMap &code,
                          LoopPlan &plan,
                          const LoopShape &outer,
                          const Instruction &op,
                          unsigned remaining,
                          std::string_view identity) {
  const auto child = loop_shape(op);
  if (child.length >= remaining) {
    reject(op, identity, FaultReason::invalid_encoding, "nested DO crosses or shares its endpoint");
  }
  const auto gap = remaining - child.length;
  if (outer.arithmetic() && gap < loop_decode_distance) {
    reject(op, identity, FaultReason::invalid_encoding, "arithmetic DO endpoints are too close");
  }
  if (outer.short_body()) { plan.short_nested.emplace(child.end, outer); }
  if (overlapping_refill(child, outer) || overlapping_short_tail(child, outer)) {
    const auto overlap_words = child.short_body() ? child.length : loop_decode_distance;
    for (unsigned distance = 0; distance < overlap_words; ++distance) {
      const auto word = code.find((child.end - distance) & maximum_pc);
      if (word != code.end() && branch(*word->second)) {
        reject(*word->second,
               identity,
               FaultReason::unsupported_instruction,
               "branch across overlapping loop refill is unqualified");
      }
    }
  }
}

void validate_loop_body(const InstructionMap &code,
                        const Instruction &setup,
                        LoopPlan &plan,
                        std::string_view identity) {
  auto shape = loop_shape(setup);
  for (unsigned offset = 1; offset <= shape.length; ++offset) {
    const auto assigned = code.find(next_pc(setup, offset));
    if (assigned == code.end()) {
      reject(setup, identity, FaultReason::undeclared_entry, "DO body has an undeclared word");
    }
    const auto &op = *assigned->second;
    const auto remaining = shape.length - offset;
    if (loop_abort(op)) {
      const auto owner = plan.aborts.find(op.address);
      if (owner == plan.aborts.end() || shape.length < owner->second.length) {
        plan.aborts.insert_or_assign(op.address, shape);
      }
    } else if (branch(op) && remaining <= loop_fetch_distance) {
      validate_loop_call(plan, shape, op, identity);
    }
    if (loop_setup(op)) { validate_nested_loop(code, plan, shape, op, remaining, identity); }
  }
  if (!plan.shapes.emplace(shape.end, shape).second) {
    reject(setup,
           identity,
           FaultReason::invalid_encoding,
           "multiple DO instructions share an endpoint");
  }
}

void validate_loop_aborts(const InstructionMap &code, LoopPlan &plan, std::string_view identity) {
  for (const auto &[pc, op] : code) {
    if (!loop_abort(*op)) { continue; }
    const auto site = plan.aborts.find(pc);
    if (site == plan.aborts.end()) {
      reject(*op, identity, FaultReason::unsupported_instruction, "LA has no declared DO body");
    }
    const auto &shape = site->second;
    const auto remaining = (shape.end - pc) & maximum_pc;
    if (shape.short_body() || remaining < loop_decode_distance) {
      reject(*op,
             identity,
             FaultReason::unsupported_instruction,
             "LA at this DO endpoint is unqualified");
    }
    if (!indirect_branch(*op) && shape.contains(branch_target(*op))) {
      reject(*op, identity, FaultReason::invalid_encoding, "LA target is inside its loop");
    }
    if (remaining < loop_fetch_distance) { plan.shapes.at(shape.end).has_tail_abort = true; }
  }
}

void validate_not_lce(const InstructionMap &code, LoopPlan &plan, std::string_view identity) {
  for (const auto &[pc, op] : code) {
    if (!observes_not_lce(*op)) { continue; }
    const LoopShape *owner = nullptr;
    for (const auto &[end, shape] : plan.shapes) {
      if (shape.contains(pc) && (owner == nullptr || shape.length < owner->length)) {
        owner = &shape;
      }
    }
    const auto previous = code.find((pc - 1) & maximum_pc);
    if (owner == nullptr || !owner->counted() ||
        ((owner->end - pc) & maximum_pc) > loop_fetch_distance ||
        (previous != code.end() && loop_setup(*previous->second))) {
      reject(*op,
             identity,
             FaultReason::unsupported_instruction,
             "NOT LCE outside an admitted counted DO tail");
    }
    const auto unqualified_overlap = [&plan](const LoopShape &parent, const LoopShape &child) {
      const auto nested = plan.short_nested.find(child.end);
      return overlapping_tail(child, parent) &&
             ((!child.arithmetic() && !child.counted()) ||
              (parent.short_body() &&
               (nested == plan.short_nested.end() || nested->second.end != parent.end)));
    };
    const bool overlap = std::ranges::any_of(plan.shapes, [&](const auto &entry) {
      return unqualified_overlap(*owner, entry.second) || unqualified_overlap(entry.second, *owner);
    });
    if (overlap) {
      reject(
          *op, identity, FaultReason::unsupported_instruction, "NOT LCE at an overlapping DO tail");
    }
    plan.not_lce.emplace(pc, *owner);
    plan.lce_endpoints.insert(owner->end);
  }
}

void plan_short_counted_tails(LoopPlan &plan) {
  for (const auto &[end, parent] : plan.short_nested) {
    if (plan.lce_endpoints.contains(parent.end)) { plan.short_parent_tails.insert(parent.end); }
  }
  for (const auto end : plan.lce_endpoints) {
    if (!plan.shapes.at(end).short_body()) { continue; }
    const auto slot =
        plan.short_parent_tails.contains(end) ? short_parent_tail_slot : short_counted_tail_slot;
    plan.lce_slots.insert(slot);
  }
}

void plan_counted_tails(LoopPlan &plan) {
  plan_short_counted_tails(plan);
  std::vector<const LoopShape *> ordered;
  for (const auto &[end, shape] : plan.shapes) {
    if (!shape.counted() || shape.short_body()) { continue; }
    if (shape.has_reentry || shape.has_tail_abort || plan.lce_endpoints.contains(end)) {
      ordered.push_back(&shape);
    }
  }
  // A long counted tail can coexist with an ancestor's tail while both drain.
  // Only endpoints within four words can overlap, including across PC wrap.
  std::ranges::sort(ordered, std::ranges::greater{}, &LoopShape::length);
  for (const auto *shape : ordered) {
    std::array<bool, loop_stack_capacity> used{};
    for (unsigned gap = 1; gap <= loop_fetch_distance; ++gap) {
      const auto parent = plan.long_tail_indices.find((shape->end + gap) & maximum_pc);
      if (parent != plan.long_tail_indices.end() &&
          overlapping_tail(*shape, plan.shapes.at(parent->first))) {
        used[parent->second] = true;
      }
    }
    const auto index = static_cast<std::size_t>(std::ranges::find(used, false) - used.begin());
    plan.long_tail_indices.emplace(shape->end, index);
    plan.long_tail_count = std::max(plan.long_tail_count, index + 1);
    if (plan.lce_endpoints.contains(shape->end)) {
      plan.lce_slots.insert(counted_tail_slot + index);
    }
  }
}

LoopPlan validate_loop_bodies(const InstructionMap &code, std::string_view identity) {
  LoopPlan plan;
  for (const auto &entry : code) {
    const auto &op = *entry.second;
    if (!loop_setup(op)) { continue; }
    validate_loop_body(code, op, plan, identity);
  }
  for (const auto &[end, parent] : plan.short_nested) {
    for (unsigned gap = 1; gap <= loop_fetch_distance; ++gap) {
      const auto ancestor = plan.shapes.find((parent.end + gap) & maximum_pc);
      if (ancestor == plan.shapes.end() || !overlapping_tail(parent, ancestor->second)) {
        continue;
      }
      const auto setup = (plan.shapes.at(end).start - 1) & maximum_pc;
      reject(*code.at(setup),
             identity,
             FaultReason::unsupported_instruction,
             "three overlapping DO tails are unqualified");
    }
  }
  validate_loop_aborts(code, plan, identity);
  validate_not_lce(code, plan, identity);
  plan_counted_tails(plan);
  return plan;
}

bool has_sequential_successor(const Instruction &op) {
  return !branch(op) || branch_condition(op) != true_condition ||
         (call_branch_instruction(op) && !delayed(op));
}

std::vector<Instruction>
connected_code(const InstructionMap &code, const LoopMap &shapes, std::uint32_t entry) {
  std::set<std::uint32_t> visited;
  std::vector<std::uint32_t> pending{entry};
  std::set<std::uint32_t> slots;
  while (!pending.empty()) {
    const auto address = pending.back();
    pending.pop_back();
    const auto found = code.find(address);
    if (found == code.end() || !visited.insert(address).second) { continue; }
    const auto &op = *found->second;
    const auto loop = shapes.find(address);
    if (loop != shapes.end()) { pending.push_back(loop->second.start); }
    if (delayed(op)) {
      slots.insert(next_pc(op));
      slots.insert(next_pc(op, delay_slot_count));
      if (call_branch_instruction(op)) { pending.push_back(call_resume(op)); }
    }
    if ((op.type == direct_branch_type || op.type == computed_branch_type) &&
        !indirect_branch(op)) {
      pending.push_back(branch_target(op));
    }
    if (has_sequential_successor(op)) { pending.push_back(next_pc(op)); }
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
                           const LoopMap &reentry_calls,
                           std::uint32_t entry) {
  for (const auto next : call_continuations(connected, reentry_calls)) {
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
  std::map<std::uint32_t, LoopPlan> variant_loops;
  for (const auto &variant : image.variants) {
    validate_memory_layout(variant, result->identity);
    const auto &code = variant_code[variant.id];
    validate_delay_slots(code, result->identity);
    const auto &plan =
        variant_loops.emplace(variant.id, validate_loop_bodies(code, result->identity))
            .first->second;
    std::set<std::uint32_t> entries(variant.entries.begin(), variant.entries.end());
    for (const auto &request : variant.interrupt_vectors) { entries.insert(request.vector); }
    for (const auto entry : entries) {
      const auto connected = connected_code(code, plan.shapes, entry);
      collect_continuations(continuations, connected, variant, plan.reentry_calls, entry);
      const auto mask = context_mask(connected, plan);
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
    const auto &plan = variant_loops.at(unit.variant);
    Lowering(module,
             debug,
             options.memory_mode,
             abi,
             *variant,
             connected_code(variant_code.at(unit.variant), plan.shapes, unit.entry),
             unit,
             plan)
        .emit(plan.shapes);
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
