// SPDX-License-Identifier: GPL-3.0-only
#include "compiler_identity.hpp"
#include "decode.hpp"
#include "native.hpp"
#include "sharc/memory.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <llvm/ADT/ScopeExit.h>
#include <map>
#include <memory>
#include <optional>
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
  std::shared_ptr<const detail::PreparedImage> image;
};

struct NativeImage::Impl {
  std::shared_ptr<const detail::PreparedImage> image;
  std::vector<detail::NativeEntry> entries;
  std::shared_ptr<void> lifetime;
  PreparationReport report;
  bool interrupts_supported{};
};

std::span<const std::byte> SemanticImage::bitcode() const noexcept {
  return impl_ ? std::span<const std::byte>(impl_->image->bitcode) : std::span<const std::byte>{};
}

std::span<const std::string> SemanticImage::exports() const noexcept {
  return impl_ ? std::span<const std::string>(impl_->image->exports)
               : std::span<const std::string>{};
}

std::string_view SemanticImage::identity() const noexcept {
  return impl_ ? std::string_view(impl_->image->identity) : std::string_view{};
}

std::vector<std::byte> SemanticImage::artifact() const {
  if (!impl_) { unavailable("empty semantic image"); }
  return detail::serialize_image(*impl_->image);
}

PreparationReport NativeImage::report() const {
  if (!impl_) { unavailable("empty native image"); }
  return impl_->report;
}

namespace {
struct Resume {
  std::string identity;
  std::uint32_t variant{}, address{}, entry{};
};
} // namespace

struct Core::Impl {
  explicit Impl(Memory input, ExecutionMode input_mode)
      : memory(std::move(input)), mode(input_mode) {
    for (unsigned lane = 0; lane < 2; ++lane) {
      for (unsigned bank = 0; bank < 2; ++bank) {
        for (unsigned reg = 0; reg < 16; ++reg) {
          pointers[(lane * 32) + (bank * 16) + reg] = &state.registers.pe[lane].data[bank][reg];
        }
      }
      pointers[detail::astat_slot + lane] = &state.registers.pe[lane].astat;
      pointers[detail::stky_slot + lane] = &state.registers.pe[lane].stky;
    }
    pointers[detail::pc_slot] = &state.registers.pc;
    pointers[detail::faddr_slot] = &state.registers.faddr;
    pointers[detail::daddr_slot] = &state.registers.daddr;
    pointers[detail::clock_slot] = &state.registers.emuclk;
    pointers[detail::hazard_slot] = &previous_compute;
  }

  void stopped_owner() const {
    if (executing) { throw std::logic_error("a running Core cannot be mutated or re-entered"); }
  }

  RunResult fail(FaultReason reason, std::string detail) {
    state.has_fault = true;
    state.execution = ExecutionState::faulted;
    state.fault = {.reason = reason,
                   .identity = installed ? installed->image->identity
                                         : std::string(Core::compiler_identity()),
                   .variant = variant,
                   .address = state.registers.pc,
                   .instruction = 0,
                   .instruction_bits = 0,
                   .detail = std::move(detail)};
    return {.progress = {}, .execution = state.execution, .has_fault = true, .fault = state.fault};
  }

  RunResult execute(std::uint64_t instructions, std::uint64_t cycles);

  Memory memory;
  const ExecutionMode mode;
  State state;
  bool yield_requested{};
  bool executing{};
  std::uint8_t previous_compute{};
  std::shared_ptr<const NativeImage::Impl> installed;
  std::uint32_t variant{};
  std::optional<Resume> resume;
  detail::RegisterPointers pointers{};
};

struct Snapshot::Impl {
  ExecutionMode mode;
  State state;
  bool yield_requested{};
  std::optional<Resume> resume;
  std::uint8_t previous_compute{};
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
  SemanticImage result;
  result.impl_ = std::make_shared<const SemanticImage::Impl>(
      SemanticImage::Impl{detail::lower_image(image, options)});
  return result;
}

SemanticImage Core::load(const CodeImage &image,
                         const CompileOptions &options,
                         std::span<const std::byte> artifact) {
  SemanticImage result;
  result.impl_ = std::make_shared<const SemanticImage::Impl>(
      SemanticImage::Impl{detail::load_image(image, options, artifact)});
  return result;
}

NativeImage Core::bind(const SemanticImage &image, const NativeSymbols &symbols) {
  if (!image.impl_) { unavailable("empty semantic image"); }
  auto bound = std::make_shared<NativeImage::Impl>();
  bound->image = image.impl_->image;
  bound->lifetime = symbols.lifetime;
  bound->report.identity = bound->image->identity;
  std::map<std::string, std::uintptr_t> addresses;
  for (const auto &symbol : symbols.entries) {
    if (symbol.name.empty() || symbol.address == 0 ||
        !addresses.emplace(symbol.name, symbol.address).second) {
      detail::refuse(FaultReason::invalid_image,
                     image.identity(),
                     0,
                     0,
                     "invalid or duplicated native symbol");
    }
  }
  for (const auto &name : image.exports()) {
    const auto found = addresses.find(name);
    if (found == addresses.end()) {
      detail::refuse(
          FaultReason::invalid_image, image.identity(), 0, 0, "missing native export " + name);
    }
    bound->entries.push_back(std::bit_cast<detail::NativeEntry>(found->second));
  }
  NativeImage result;
  result.impl_ = std::move(bound);
  return result;
}

NativeImage Core::compile(const CodeImage &image, const CompileOptions &options) {
  const auto start = std::chrono::steady_clock::now();
  const auto semantic = lower(image, options);
  auto compiled = detail::compile_image(*semantic.impl_->image);
  auto result = bind(semantic, compiled.symbols);
  auto report = std::make_shared<NativeImage::Impl>(*result.impl_);
  report->report.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  report->report.native_bytes = compiled.native_bytes;
  result.impl_ = std::move(report);
  return result;
}

PreparationReport Core::install(const NativeImage &image, std::uint32_t variant) {
  impl_->stopped_owner();
  if (!image.impl_) { unavailable("empty native image", impl_->state.registers.pc); }
  if (image.impl_->image->options.mode != impl_->mode) {
    detail::refuse(FaultReason::invalid_image,
                   image.impl_->image->identity,
                   variant,
                   impl_->state.registers.pc,
                   "native image uses a different numerical mode");
  }
  impl_->installed.reset();
  {
    impl_->executing = true;
    const auto release = llvm::make_scope_exit([&] { impl_->executing = false; });
    detail::validate_code_storage(*image.impl_->image, variant, impl_->memory);
  }
  impl_->installed = image.impl_;
  impl_->variant = variant;
  return image.impl_->report;
}

RunResult Core::run(std::uint64_t instruction_budget) {
  return impl_->execute(instruction_budget, std::numeric_limits<std::uint64_t>::max());
}

RunResult Core::run_until(std::uint64_t base_cycle_deadline) {
  const auto now = impl_->state.progress.base_cycles;
  return impl_->execute(std::numeric_limits<std::uint64_t>::max(),
                        base_cycle_deadline > now ? base_cycle_deadline - now : 0);
}

void Core::request_yield() noexcept {
  impl_->yield_requested = true;
}

bool Core::interrupt_ready(const InterruptRequest & /*request*/) const noexcept {
  return impl_->installed && impl_->installed->interrupts_supported;
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
  impl_->stopped_owner();
  impl_->memory = std::move(memory);
  impl_->installed.reset();
}

Registers Core::registers() const {
  return impl_->state.registers;
}

void Core::set_registers(const Registers &registers) {
  impl_->stopped_owner();
  constexpr unsigned data_bits = 40;
  constexpr unsigned maximum_fifo_pointer = 0x7f;
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
    if (pe.bit_fifo_write_pointer > maximum_fifo_pointer) {
      throw std::invalid_argument("bit FIFO write pointer exceeds 7 bits");
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
  if (registers.pc != impl_->state.registers.pc) {
    impl_->resume.reset();
    impl_->previous_compute = 0;
  }
  impl_->state.registers = registers;
}

const State &Core::state() const noexcept {
  return impl_->state;
}

Snapshot Core::save() const {
  impl_->stopped_owner();
  Snapshot snapshot;
  snapshot.impl_ = std::make_shared<const Snapshot::Impl>(
      Snapshot::Impl{.mode = impl_->mode,
                     .state = impl_->state,
                     .yield_requested = impl_->yield_requested,
                     .resume = impl_->resume,
                     .previous_compute = impl_->previous_compute});
  return snapshot;
}

void Core::restore(const Snapshot &snapshot) {
  impl_->stopped_owner();
  if (!snapshot.impl_ || snapshot.impl_->mode != impl_->mode) {
    throw std::invalid_argument("empty snapshot or incompatible numerical mode");
  }
  impl_->state = snapshot.impl_->state;
  impl_->yield_requested = snapshot.impl_->yield_requested;
  impl_->resume = snapshot.impl_->resume;
  impl_->previous_compute = snapshot.impl_->previous_compute;
}

RunResult Core::Impl::execute(std::uint64_t instructions, std::uint64_t cycles) {
  stopped_owner();
  if (state.has_fault) {
    return {.progress = {}, .execution = state.execution, .has_fault = true, .fault = state.fault};
  }
  if (!installed) {
    return fail(FaultReason::unprepared, "execution requires an installed native image");
  }
  const auto &image = *installed->image;
  const auto &registers = state.registers;
  const auto code = std::ranges::find(image.code.variants, variant, &CodeVariant::id);
  const bool continuing = resume && resume->identity == image.identity &&
                          resume->variant == variant && resume->address == registers.pc;
  const bool declared = code != image.code.variants.end() &&
                        (std::ranges::find(code->entries, registers.pc) != code->entries.end() ||
                         std::ranges::any_of(code->interrupt_vectors, [&](const auto &request) {
                           return request.vector == registers.pc;
                         }));
  if (!continuing && !declared) {
    return fail(FaultReason::undeclared_entry,
                "execution PC is not a declared entry or saved continuation");
  }
  constexpr unsigned timer_enable = 1U << 5;
  if (state.instruction_set != InstructionSet::isa || (registers.mode2 & timer_enable) != 0) {
    return fail(FaultReason::unsupported_feature,
                "VISA execution or an enabled core timer is not implemented");
  }
  if (yield_requested) {
    yield_requested = false;
    state.execution = ExecutionState::yielded;
    return {.progress = {}, .execution = state.execution, .has_fault = false, .fault = {}};
  }
  if (instructions == 0 || cycles == 0) {
    state.execution = ExecutionState::stopped;
    return {.progress = {}, .execution = state.execution, .has_fault = false, .fault = {}};
  }
  const auto &units = image.units;
  const auto unit = std::ranges::find_if(units, [&](const auto &candidate) {
    return candidate.variant == variant &&
           candidate.entry == (continuing ? resume->entry : registers.pc) &&
           (registers.mode1 & candidate.mask) == candidate.context;
  });
  if (unit == units.end()) {
    return fail(FaultReason::unsupported_feature, "no prepared MODE1 context");
  }
  detail::ControlWords control{};
  control[detail::remaining_instructions] = instructions;
  control[detail::remaining_cycles] = cycles;
  const auto index = static_cast<std::size_t>(unit - units.begin());
  state.execution = ExecutionState::running;
  {
    executing = true;
    const auto release = llvm::make_scope_exit([&] { executing = false; });
    installed->entries[index](pointers.data(), control.data());
  }
  const Progress progress{.instructions = control[detail::retired_instructions],
                          .base_cycles = control[detail::retired_cycles]};
  state.progress.instructions += progress.instructions;
  state.progress.base_cycles += progress.base_cycles;
  resume = Resume{.identity = image.identity,
                  .variant = variant,
                  .address = state.registers.pc,
                  .entry = unit->entry};
  if (control[detail::exit_reason] == static_cast<std::uint64_t>(detail::NativeExit::undeclared)) {
    auto result =
        fail(FaultReason::undeclared_entry, "execution reached an undeclared instruction");
    result.progress = progress;
    return result;
  }
  state.execution = ExecutionState::stopped;
  return {.progress = progress, .execution = state.execution, .has_fault = false, .fault = {}};
}
} // namespace sharc
