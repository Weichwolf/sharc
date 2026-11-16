// SPDX-License-Identifier: GPL-3.0-only
#include "compiler_identity.hpp"
#include "decode.hpp"
#include "sharc/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <sharc/core.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sharc {
namespace {
[[noreturn]] void unavailable(std::string message, std::uint32_t pc = 0) {
  detail::refuse(
      FaultReason::unsupported_feature, Core::compiler_identity(), 0, pc, std::move(message));
}
} // namespace

PreparationError::PreparationError(Fault fault)
    : std::runtime_error(fault.identity + ": variant " + std::to_string(fault.variant) + ", PC " +
                         std::to_string(fault.address) + ": " + fault.detail),
      fault_(std::move(fault)) {}

const Fault &PreparationError::fault() const noexcept {
  return fault_;
}

struct SemanticImage::Impl {
  std::vector<std::byte> bitcode;
  std::vector<std::string> exports;
  std::string identity;
};

std::span<const std::byte> SemanticImage::bitcode() const noexcept {
  return impl_ ? std::span<const std::byte>(impl_->bitcode) : std::span<const std::byte>{};
}

std::span<const std::string> SemanticImage::exports() const noexcept {
  return impl_ ? std::span<const std::string>(impl_->exports) : std::span<const std::string>{};
}

std::string_view SemanticImage::identity() const noexcept {
  return impl_ ? std::string_view(impl_->identity) : std::string_view{};
}

std::vector<std::byte> SemanticImage::artifact() const {
  unavailable(impl_ ? "artifact serialization is not implemented" : "empty semantic image");
}

PreparationReport NativeImage::report() const {
  unavailable(impl_ ? "native preparation is not implemented" : "empty native image");
}

struct Core::Impl {
  explicit Impl(Memory input, ExecutionMode input_mode)
      : memory(std::move(input)), mode(input_mode) {}

  Memory memory;
  const ExecutionMode mode;
  State state;
  bool yield_requested{};
  bool installed{};
};

struct Snapshot::Impl {
  ExecutionMode mode;
  State state;
  bool yield_requested{};
};

Core::Core(Memory memory, ExecutionMode mode)
    : impl_(std::make_unique<Impl>(std::move(memory), mode)) {
  if (mode != ExecutionMode::strict && mode != ExecutionMode::fast_fp32) {
    throw std::invalid_argument("unknown execution mode");
  }
}

Core::~Core() = default;

std::string_view Core::compiler_identity() noexcept {
  return SHARC_COMPILER_IDENTITY;
}

SemanticImage Core::lower(const CodeImage &image, const CompileOptions &options) {
  const auto identity = detail::input_identity(image, options);
  if (options.mode != ExecutionMode::strict && options.mode != ExecutionMode::fast_fp32) {
    detail::refuse(FaultReason::invalid_image, identity, 0, 0, "unknown execution mode");
  }
  const auto instructions = detail::decode(image, identity);
  const auto &first = instructions.front();
  char suffix = 'c';
  if (first.bits == detail::full_instruction_bits) { suffix = 'a'; }
  if (first.bits == detail::wide_instruction_bits) { suffix = 'b'; }
  detail::refuse(FaultReason::unsupported_instruction,
                 identity,
                 first.variant,
                 first.address,
                 "type " + std::to_string(first.type) + suffix + " lowering is not implemented",
                 first.word,
                 first.bits);
}

SemanticImage Core::load(const CodeImage &image,
                         const CompileOptions &options,
                         std::span<const std::byte> /*artifact*/) {
  detail::refuse(FaultReason::unsupported_feature,
                 detail::input_identity(image, options),
                 0,
                 0,
                 "artifact loading is not implemented");
}

NativeImage Core::bind(const SemanticImage & /*image*/, const NativeSymbols & /*symbols*/) {
  unavailable("native binding is not implemented");
}

NativeImage Core::compile(const CodeImage &image, const CompileOptions &options) {
  return bind(lower(image, options), {});
}

PreparationReport Core::install(const NativeImage & /*image*/, std::uint32_t variant) {
  detail::refuse(FaultReason::unprepared,
                 compiler_identity(),
                 variant,
                 impl_->state.registers.pc,
                 "no native image can be installed before lowering is implemented");
}

RunResult Core::run(std::uint64_t /*instruction_budget*/) {
  auto &state = impl_->state;
  state.has_fault = true;
  state.execution = ExecutionState::faulted;
  state.fault = {.reason = FaultReason::unprepared,
                 .identity = std::string(compiler_identity()),
                 .variant = 0,
                 .address = state.registers.pc,
                 .instruction = 0,
                 .instruction_bits = 0,
                 .detail = "execution requires an installed native image"};
  return {.progress = {}, .execution = state.execution, .has_fault = true, .fault = state.fault};
}

RunResult Core::run_until(std::uint64_t /*base_cycle_deadline*/) {
  return run(0);
}

void Core::request_yield() noexcept {
  impl_->yield_requested = true;
}

bool Core::interrupt_ready(const InterruptRequest & /*request*/) const noexcept {
  return impl_->installed;
}

void Core::interrupt(const InterruptRequest & /*request*/) {
  unavailable("interrupt entry is not implemented", impl_->state.registers.pc);
}

void Core::reset(std::uint32_t /*address*/, InstructionSet /*instruction_set*/) {
  unavailable("architectural reset is not implemented", impl_->state.registers.pc);
}

void Core::resume_from_stop() {
  unavailable("sleep wake-up is not implemented", impl_->state.registers.pc);
}

void Core::set_memory(Memory memory) {
  impl_->memory = std::move(memory);
}

Registers Core::registers() const {
  return impl_->state.registers;
}

void Core::set_registers(const Registers &registers) {
  constexpr unsigned data_bits = 40;
  constexpr unsigned pc_stack_bits = 26;
  constexpr unsigned overflowed_pc_stack_pointer = 31;
  for (const auto &pe : registers.pe) {
    for (const auto &bank : pe.data) {
      for (const auto value : bank) {
        if (value >> data_bits != 0) {
          throw std::invalid_argument("data register exceeds 40 bits");
        }
      }
    }
    if (pe.bit_fifo_write_pointer > 64) {
      throw std::invalid_argument("bit FIFO write pointer exceeds 64");
    }
  }
  if (registers.pc > detail::maximum_pc || registers.faddr > detail::maximum_pc ||
      registers.daddr > detail::maximum_pc ||
      registers.pc_stack_pointer > overflowed_pc_stack_pointer ||
      registers.loop_address_depth > 6 || registers.loop_counter_depth > 6 ||
      registers.status_depth > 15) {
    throw std::invalid_argument("register address or stack pointer out of range");
  }
  for (const auto frame : registers.pc_stack) {
    if (frame >> pc_stack_bits != 0) {
      throw std::invalid_argument("PC stack entry exceeds 26 bits");
    }
  }
  impl_->state.registers = registers;
}

const State &Core::state() const noexcept {
  return impl_->state;
}

Snapshot Core::save() const {
  Snapshot snapshot;
  snapshot.impl_ = std::make_shared<const Snapshot::Impl>(Snapshot::Impl{
      .mode = impl_->mode, .state = impl_->state, .yield_requested = impl_->yield_requested});
  return snapshot;
}

void Core::restore(const Snapshot &snapshot) {
  if (!snapshot.impl_ || snapshot.impl_->mode != impl_->mode) {
    throw std::invalid_argument("empty snapshot or incompatible numerical mode");
  }
  impl_->state = snapshot.impl_->state;
  impl_->yield_requested = snapshot.impl_->yield_requested;
}
} // namespace sharc
