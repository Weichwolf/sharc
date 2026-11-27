// SPDX-License-Identifier: GPL-3.0-only
#include "compiler_identity.hpp"
#include "decode.hpp"
#include "escape.hpp"
#include "native.hpp"
#include "sharc/memory.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <llvm/ADT/ScopeExit.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
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

std::uint32_t read_io(const Io *io,
                      MemorySpace space,
                      MemoryView view,
                      std::uint32_t address,
                      std::uint64_t &word) noexcept {
  return static_cast<std::uint32_t>(io->read(io->owner, space, view, address, word));
}

std::uint32_t write_io(const Io *io,
                       MemorySpace space,
                       MemoryView view,
                       std::uint32_t address,
                       const std::uint64_t &word) noexcept {
  return static_cast<std::uint32_t>(io->write(io->owner, space, view, address, word));
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

namespace {
struct FaultAccess {
  std::uint32_t variant{}, pc{};
  std::uint64_t word{};
  std::uint8_t bits{};
  std::array<unsigned, 2> spaces{}; // Read and write separately.
};

unsigned space_mask(MemorySpace space) {
  return 1U << static_cast<unsigned>(space);
}

std::vector<FaultAccess> fault_accesses(std::span<const detail::Instruction> decoded) {
  std::vector<FaultAccess> accesses;
  for (const auto &op : decoded) {
    const auto moves = detail::memory_moves(op);
    if (moves.count == 0) { continue; }
    FaultAccess access{.variant = op.variant, .pc = op.address, .word = op.word, .bits = op.bits};
    for (const auto &move : moves) { access.spaces[move.write ? 1 : 0] |= space_mask(move.space); }
    accesses.push_back(access);
  }
  return accesses;
}
} // namespace

struct NativeImage::Impl {
  std::shared_ptr<const detail::PreparedImage> image;
  std::vector<detail::NativeEntry> entries;
  std::shared_ptr<void> lifetime;
  PreparationReport report;
  std::vector<NativeSymbols::TextRange> text_ranges;
  std::vector<NativeSymbols::Site> sites;
  std::vector<FaultAccess> accesses;
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
void validate_fault_map(const detail::PreparedImage &image,
                        std::vector<NativeSymbols::TextRange> &ranges,
                        std::vector<NativeSymbols::Site> &sites,
                        std::span<const detail::Instruction> decoded) {
  const auto invalid = [&] {
    detail::refuse(FaultReason::invalid_image, image.identity, 0, 0, "invalid native fault map");
  };
  const auto valid = [](const auto &range) {
    return range.address != 0 && range.size != 0 &&
           range.size <= std::numeric_limits<std::uintptr_t>::max() - range.address;
  };
  std::ranges::sort(ranges, {}, &NativeSymbols::TextRange::address);
  std::uintptr_t end{};
  for (const auto &range : ranges) {
    if (!valid(range) || range.address < end) { invalid(); }
    end = range.address + range.size;
  }
  std::set<std::pair<std::uint64_t, std::uint32_t>> instructions;
  for (const auto &op : decoded) { instructions.emplace(op.variant, op.address); }
  std::ranges::sort(sites, {}, &NativeSymbols::Site::address);
  end = 0;
  for (const auto &site : sites) {
    if (!valid(site) || site.address < end ||
        !instructions.contains({site.variant, site.guest_pc})) {
      invalid();
    }
    end = site.address + site.size;
    auto covered = site.address;
    for (const auto &range : ranges) {
      if (range.address > covered) { break; }
      const auto next = range.address + range.size;
      covered = std::max(covered, next);
      if (covered >= end) { break; }
    }
    if (covered < end) { invalid(); }
  }
}

struct Resume {
  std::string identity;
  std::uint32_t variant{}, address{}, entry{};
};

struct PendingTransfer {
  std::string identity;
  std::vector<std::uint32_t> targets;

  [[nodiscard]] bool accepts(const detail::PreparedImage &image, std::uint32_t variant) const {
    return image.identity == identity && std::ranges::find(targets, variant) != targets.end();
  }
};

struct LoopPipeline {
  std::array<std::uint32_t, detail::loop_stack_capacity> frames{};
  std::uint64_t refill{};
  std::array<std::uint64_t, detail::loop_stack_capacity> counted_tails{};
  std::uint64_t short_counted_tail{};
  std::uint64_t short_parent_tail{};
  std::array<std::uint64_t, detail::short_state_words> short_loop{};
  std::array<std::uint64_t, Registers::pc_stack_capacity> calls{};
  std::array<std::uint64_t, Registers::pc_stack_capacity * detail::suspended_call_state_words>
      suspended_calls{};
  std::uint64_t return_tail{};
  std::uint64_t interrupt_deadline{};

  [[nodiscard]] bool active() const {
    return short_loop[detail::short_remaining] != 0 ||
           short_loop[detail::short_parent_remaining] != 0 || refill != 0 ||
           short_counted_tail != 0 || short_parent_tail != 0 || return_tail != 0 ||
           std::ranges::any_of(counted_tails, [](auto tail) { return tail != 0; }) ||
           std::ranges::any_of(frames, [](auto frame) { return frame != 0; }) ||
           std::ranges::any_of(calls, [](auto call) { return call != 0; });
  }

  [[nodiscard]] bool blocks_interrupt_at_setup(const Registers &registers) const noexcept {
    const auto descriptor = short_loop[detail::short_descriptor];
    const auto depth = registers.loop_address_depth;
    if ((descriptor & detail::short_startup) == 0 || depth == 0) { return false; }
    const auto address = registers.loop_address_stack[depth - 1];
    constexpr unsigned termination_shift = 24;
    constexpr unsigned termination_mask = 31;
    constexpr unsigned counter_expired = 15;
    constexpr unsigned type_shift = 29;
    constexpr unsigned three_word_counted_type = 6;
    // A counted DO with one, two or four body words blocks the following cycle.
    return (address & detail::maximum_pc) == (descriptor & detail::maximum_pc) &&
           ((address >> termination_shift) & termination_mask) == counter_expired &&
           (address >> type_shift) != three_word_counted_type;
  }

  void observe(Registers &registers) const {
    if (observe_short(registers)) { return; }
    const auto pc = registers.pc;
    const bool refilling = observe_refill(registers, pc);
    for (auto depth = registers.loop_address_depth; depth != 0; --depth) {
      const auto frame = frames[depth - 1];
      if ((frame & detail::loop_repeat_scheduled) == 0) { continue; }
      const auto pc_slot = frame & detail::loop_pc_index_mask;
      if (pc_slot + 1 != registers.pc_stack_pointer) { continue; }
      const auto end = registers.loop_address_stack[depth - 1] & detail::maximum_pc;
      const auto distance = (end - pc) & detail::maximum_pc;
      if (distance >= detail::loop_fetch_distance) { continue; }
      const auto start = registers.pc_stack[pc_slot];
      if (refilling) {
        // The child owns Decode, but Fetch1 may already have reached
        // the parent ending immediately after the child.
        const auto child_end = static_cast<std::uint32_t>(refill >> detail::loop_refill_end_shift) &
                               detail::maximum_pc;
        if (pc == child_end && distance == 1) { registers.faddr = start; }
        return;
      }
      registers.faddr = (start + detail::loop_fetch_distance - distance - 1) & detail::maximum_pc;
      if (distance < detail::loop_decode_distance) {
        registers.daddr =
            (start + detail::loop_decode_distance - distance - 1) & detail::maximum_pc;
      }
      return;
    }
  }

private:
  bool observe_short(Registers &registers) const {
    const auto addresses = short_loop[detail::short_addresses];
    if ((addresses & detail::short_addresses_live) == 0) { return false; }
    const auto end =
        static_cast<std::uint32_t>(short_loop[detail::short_descriptor]) & detail::maximum_pc;
    if (short_loop[detail::short_remaining] == 0 &&
        registers.pc != ((end + 1) & detail::maximum_pc)) {
      return false;
    }
    registers.faddr = static_cast<std::uint32_t>(addresses) & detail::maximum_pc;
    registers.daddr =
        static_cast<std::uint32_t>(addresses >> detail::short_decode_shift) & detail::maximum_pc;
    return true;
  }

  bool observe_refill(Registers &registers, std::uint32_t pc) const {
    if ((refill & detail::loop_refill_live) == 0) { return false; }
    const auto start = static_cast<std::uint32_t>(refill) & detail::maximum_pc;
    const auto end =
        static_cast<std::uint32_t>(refill >> detail::loop_refill_end_shift) & detail::maximum_pc;
    const auto distance = (end - pc) & detail::maximum_pc;
    if (distance >= detail::loop_decode_distance) { return false; }
    registers.faddr = (end + detail::loop_decode_distance - distance) & detail::maximum_pc;
    registers.daddr = (start + detail::loop_decode_distance - distance - 1) & detail::maximum_pc;
    return true;
  }
};

constexpr std::uint32_t nesting_enable = 1U << 11;
constexpr std::uint32_t interrupt_enable = 1U << 12;
constexpr std::uint32_t stack_overflow_interrupt = 1U << 3;
constexpr std::uint32_t pc_stack_full = 1U << 21;
constexpr std::uint32_t pc_stack_empty = 1U << 22;
constexpr std::uint32_t status_stack_empty = 1U << 24;
constexpr std::uint32_t timer_enable = 1U << 5;
constexpr unsigned vector_refill_cycles = 4;
constexpr unsigned lirptl_enable_shift = 10;
constexpr unsigned lirptl_pointer_shift = 20;

struct InterruptLine {
  std::uint32_t latch, enable, active;
  bool extended{};

  [[nodiscard]] bool enabled(const Registers &registers) const noexcept {
    return ((extended ? registers.lirptl : registers.imask) & enable) != 0;
  }

  [[nodiscard]] bool in_service(const Registers &registers) const noexcept {
    return ((extended ? registers.lirptl : registers.imaskp) & active) != 0;
  }

  void set_service(Registers &registers, bool begin) const noexcept {
    auto &latches = extended ? registers.lirptl : registers.irptl;
    auto &pointers = extended ? registers.lirptl : registers.imaskp;
    latches &= ~latch;
    if (begin) {
      pointers |= active;
    } else {
      pointers &= ~active;
    }
  }
};

// Table B-2 gives vector priorities; Tables A-12 and A-13 map them to registers.
InterruptLine interrupt_line(unsigned source) noexcept {
  constexpr unsigned p6 = 17;
  constexpr unsigned p14 = 25;
  constexpr unsigned p17 = 28;
  constexpr unsigned cb7 = 30;
  if ((source >= p6 && source < p14) || (source >= p17 && source < cb7)) {
    const auto index = source < p14 ? source - p6 : (p14 - p6) + (source - p17);
    const auto bit = 1U << index;
    return {.latch = bit,
            .enable = bit << lirptl_enable_shift,
            .active = bit << lirptl_pointer_shift,
            .extended = true};
  }
  const auto skipped = (source >= p14 ? p14 - p6 : 0U) + (source >= cb7 ? cb7 - p17 : 0U);
  const auto bit = 1U << (source - skipped);
  return {.latch = bit, .enable = bit, .active = bit};
}

void clear_in_service_latches(Registers &registers) noexcept {
  constexpr std::uint32_t extended_latch_mask = (1U << lirptl_enable_shift) - 1;
  registers.irptl &= ~registers.imaskp;
  registers.lirptl &= ~((registers.lirptl >> lirptl_pointer_shift) & extended_latch_mask);
}

bool saves_interrupt_status(unsigned source) noexcept {
  constexpr unsigned timer_high = 4;
  constexpr unsigned timer_low = 32;
  constexpr unsigned irq2 = 8;
  constexpr unsigned irq0 = 10;
  return (source >= irq2 && source <= irq0) || source == timer_high || source == timer_low;
}

struct InterruptFrame {
  std::optional<Resume> resume;
  LoopPipeline loops;
  std::uint64_t entry_instruction{};
  std::uint8_t source{}, pc_depth{};
};
} // namespace

struct Core::Impl {
  explicit Impl(Memory input, ExecutionMode input_mode)
      : memory(std::move(input)), mode(input_mode), pointers(detail::register_pointer_count) {
    for (unsigned lane = 0; lane < 2; ++lane) {
      for (unsigned bank = 0; bank < 2; ++bank) {
        for (unsigned reg = 0; reg < 16; ++reg) {
          pointers[(lane * 32) + (bank * 16) + reg] = &state.registers.pe[lane].data[bank][reg];
        }
      }
      for (unsigned bank = 0; bank < detail::accumulator_banks; ++bank) {
        auto &mr = state.registers.pe[lane].multiplier[bank];
        pointers[detail::mr_slot(lane, bank, 0)] = &mr.low;
        pointers[detail::mr_slot(lane, bank, 1)] = &mr.middle;
        pointers[detail::mr_slot(lane, bank, 2)] = &mr.high;
      }
      pointers[detail::astat_slot + lane] = &state.registers.pe[lane].astat;
      pointers[detail::stky_slot + lane] = &state.registers.pe[lane].stky;
    }
    pointers[detail::pc_slot] = &state.registers.pc;
    pointers[detail::faddr_slot] = &state.registers.faddr;
    pointers[detail::daddr_slot] = &state.registers.daddr;
    pointers[detail::clock_slot] = &state.registers.emuclk;
    pointers[detail::pipeline_slot] = &pipeline_history;
    pointers[detail::delayed_target_slot] = &delayed_target;
    pointers[detail::mode_slot] = &state.registers.mode1;
    pointers[detail::stack_slot] = state.registers.pc_stack.data();
    pointers[detail::stack_depth_slot] = &state.registers.pc_stack_pointer;
    pointers[detail::loop_address_slot] = state.registers.loop_address_stack.data();
    pointers[detail::loop_counter_slot] = state.registers.loop_counter_stack.data();
    pointers[detail::loop_address_depth_slot] = &state.registers.loop_address_depth;
    pointers[detail::loop_counter_depth_slot] = &state.registers.loop_counter_depth;
    pointers[detail::lcntr_slot] = &state.registers.lcntr;
    pointers[detail::loop_frames_slot] = loops.frames.data();
    pointers[detail::loop_refill_slot] = &loops.refill;
    pointers[detail::short_loop_slot] = loops.short_loop.data();
    pointers[detail::loop_call_slot] = loops.calls.data();
    pointers[detail::return_loop_tail_slot] = &loops.return_tail;
    for (std::size_t index = 0; index < loops.counted_tails.size(); ++index) {
      pointers[detail::counted_tail_slot + index] = &loops.counted_tails[index];
    }
    pointers[detail::short_counted_tail_slot] = &loops.short_counted_tail;
    pointers[detail::short_parent_tail_slot] = &loops.short_parent_tail;
    pointers[detail::suspended_call_state_slot] = loops.suspended_calls.data();
    pointers[detail::interrupt_latch_slot] = &state.registers.irptl;
    pointers[detail::interrupt_frame_slot] = &interrupt_frame_guard;
    pointers[detail::status_depth_slot] = &state.registers.status_depth;
    pointers[detail::progress_cycle_slot] = &state.progress.base_cycles;
    pointers[detail::interrupt_deadline_slot] = &loops.interrupt_deadline;
    pointers[detail::io_slot] = &memory.io;
    pointers[detail::yield_slot] = &yield_requested;
    for (unsigned bank = 0; bank < 2; ++bank) {
      for (unsigned index = 0; index < 16; ++index) {
        auto &dag = state.registers.dag[bank][index];
        const std::array fields{&dag.index, &dag.modifier, &dag.length, &dag.base};
        for (unsigned field = 0; field < fields.size(); ++field) {
          pointers[detail::dag_slot(bank, index, field)] = fields[field];
        }
      }
    }
  }

  void idle_owner() const {
    if (executing) { throw std::logic_error("a running Core cannot be mutated or re-entered"); }
  }

  void stopped_owner() const {
    idle_owner();
    if (terminal_fault) {
      throw std::logic_error("a Core terminated by a host fault cannot be resumed");
    }
  }

  void clear_continuation() noexcept {
    resume.reset();
    pipeline_history = 0;
    delayed_target = 0;
    loops = {};
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

  std::optional<RunResult> native_failure(const detail::ControlWords &control, Progress progress) {
    const auto reason = static_cast<detail::NativeExit>(control[detail::exit_reason]);
    RunResult result;
    switch (reason) {
      case detail::NativeExit::unsupported_memory:
      case detail::NativeExit::access_fault:
      case detail::NativeExit::code_alias:
        result = fail(reason == detail::NativeExit::access_fault ? FaultReason::access_fault
                                                                 : FaultReason::unsupported_feature,
                      "memory space " + std::to_string(control[detail::memory_space]) +
                          ", address " + std::to_string(control[detail::memory_address]) +
                          (reason == detail::NativeExit::code_alias
                               ? ": store may alias prepared instruction storage"
                               : ": access denied or no prepared data binding"));
        result.fault.instruction = control[detail::fault_instruction];
        result.fault.instruction_bits = detail::full_instruction_bits;
        state.fault = result.fault;
        break;
      case detail::NativeExit::unsupported_stack:
        result = fail(FaultReason::unsupported_feature,
                      "PC stack underflow, overflow or tagged continuation is not implemented");
        break;
      case detail::NativeExit::unsupported_loop_stack:
        result = fail(FaultReason::unsupported_feature, "invalid or exhausted hardware loop stack");
        result.fault.instruction = control[detail::fault_instruction];
        result.fault.instruction_bits = detail::full_instruction_bits;
        state.fault = result.fault;
        break;
      case detail::NativeExit::unsupported_loop_reentry:
        result = fail(FaultReason::unsupported_feature, "loop continuation requires RTS (LR)");
        result.fault.instruction = control[detail::fault_instruction];
        result.fault.instruction_bits = detail::full_instruction_bits;
        state.fault = result.fault;
        break;
      case detail::NativeExit::invalid_loop_abort:
        result = fail(FaultReason::unsupported_feature,
                      "JUMP (LA) requires a valid exit from the innermost loop");
        result.fault.instruction = control[detail::fault_instruction];
        result.fault.instruction_bits = detail::full_instruction_bits;
        state.fault = result.fault;
        break;
      case detail::NativeExit::invalid_short_loop_call:
        result = fail(FaultReason::unsupported_feature, "CALL in a two-word, single-iteration DO");
        result.fault.instruction = control[detail::fault_instruction];
        result.fault.instruction_bits = detail::full_instruction_bits;
        state.fault = result.fault;
        break;
      case detail::NativeExit::invalid_interrupt_return:
        result = fail(FaultReason::unsupported_feature,
                      "RTI has no matching stack frame or uses DB with automatic status restore");
        result.fault.instruction = control[detail::fault_instruction];
        result.fault.instruction_bits = detail::full_instruction_bits;
        state.fault = result.fault;
        break;
      case detail::NativeExit::undeclared:
        result = fail(FaultReason::undeclared_entry, "execution reached an undeclared instruction");
        break;
      default: return std::nullopt;
    }
    result.progress = progress;
    return result;
  }

  [[nodiscard]] bool continuing() const {
    return resume && resume->identity == installed->image->identity && resume->variant == variant &&
           resume->address == state.registers.pc;
  }

  [[nodiscard]] const CodeVariant *installed_variant() const noexcept {
    const auto &variants = installed->image->code.variants;
    const auto code = std::ranges::find(variants, variant, &CodeVariant::id);
    return code != variants.end() ? &*code : nullptr;
  }

  [[nodiscard]] bool declared_interrupt(const InterruptRequest &request) const noexcept {
    const auto *code = installed_variant();
    return code != nullptr && std::ranges::any_of(code->interrupt_vectors, [&](const auto &entry) {
             return entry.source == request.source && entry.vector == request.vector;
           });
  }

  [[nodiscard]] bool entry_allowed() const {
    if (continuing()) { return true; }
    if (delayed_target != 0 || loops.active()) { return false; }
    const auto *code = installed_variant();
    return code != nullptr &&
           (std::ranges::find(code->entries, state.registers.pc) != code->entries.end() ||
            std::ranges::any_of(code->interrupt_vectors, [&](const auto &request) {
              return request.vector == state.registers.pc;
            }));
  }

  [[nodiscard]] std::optional<std::size_t> prepared_unit() const {
    const auto &units = installed->image->units;
    const auto entry = resume && continuing() ? resume->entry : state.registers.pc;
    const auto unit = std::ranges::find_if(units, [&](const auto &candidate) {
      return candidate.variant == variant && candidate.entry == entry &&
             (state.registers.mode1 & candidate.mask) == candidate.context;
    });
    if (unit == units.end()) { return std::nullopt; }
    return static_cast<std::size_t>(unit - units.begin());
  }

  RunResult execute(std::uint64_t instructions, std::uint64_t cycles);
  std::optional<RunResult> open_run(bool has_budget);
  std::optional<RunResult> native_stop(const detail::ControlWords &control, Progress progress);
  [[nodiscard]] const CodeTransition *transfer_target() const;

  RunResult yield(Progress progress) noexcept {
    yield_requested = 0;
    state.execution = ExecutionState::yielded;
    return {.progress = progress, .execution = state.execution, .has_fault = false, .fault = {}};
  }

  [[nodiscard]] bool continue_after(detail::NativeExit reason);
  void enter_idle();
  void refresh_interrupt_frame() noexcept;
  void return_from_interrupt();
  void observe_pipeline(Progress retired);
  RunResult host_failure(Progress progress);
  bool enter(std::size_t index, detail::ControlWords &control);

  Memory memory;
  const ExecutionMode mode;
  State state;
  std::uint8_t yield_requested{};
  bool executing{};
  bool terminal_fault{};
  detail::NativeCall native_call;
  std::vector<detail::HostView> views;
  std::uint32_t pipeline_history{};
  std::uint64_t delayed_target{}; // Latched PC, next slot and optional loop fetch address.
  LoopPipeline loops;
  std::shared_ptr<const NativeImage::Impl> installed;
  std::uint32_t variant{};
  std::optional<Resume> resume;
  std::optional<PendingTransfer> transfer;
  std::vector<InterruptFrame> interrupt_frames;
  std::uint32_t interrupt_frame_guard{};
  detail::RegisterPointers pointers;
};

struct Snapshot::Impl {
  ExecutionMode mode;
  State state;
  bool yield_requested{};
  std::optional<Resume> resume;
  std::optional<PendingTransfer> transfer;
  std::uint32_t pipeline_history{};
  std::uint64_t delayed_target{};
  LoopPipeline loops;
  std::vector<InterruptFrame> interrupt_frames;
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
  bound->text_ranges.assign(symbols.fault_map.text_ranges.begin(),
                            symbols.fault_map.text_ranges.end());
  bound->sites.assign(symbols.fault_map.sites.begin(), symbols.fault_map.sites.end());
  const auto decoded = bound->sites.empty() ? std::vector<detail::Instruction>{}
                                            : detail::decode(bound->image->code, image.identity());
  validate_fault_map(*bound->image, bound->text_ranges, bound->sites, decoded);
  if (bound->image->options.memory_mode == CompileOptions::MemoryMode::direct) {
    if (bound->text_ranges.empty() || bound->sites.empty()) {
      detail::refuse(FaultReason::invalid_image,
                     image.identity(),
                     0,
                     0,
                     "direct image requires native fault attribution");
    }
    bound->accesses = fault_accesses(decoded);
  }
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
    if (!bound->text_ranges.empty() &&
        !std::ranges::any_of(bound->text_ranges, [&](const auto &range) {
          return found->second >= range.address && found->second - range.address < range.size;
        })) {
      detail::refuse(FaultReason::invalid_image,
                     image.identity(),
                     0,
                     0,
                     "native export outside executable text");
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
  const auto &pending = impl_->transfer;
  if (pending && !pending->accepts(*image.impl_->image, variant)) {
    detail::refuse(FaultReason::invalid_image,
                   image.impl_->image->identity,
                   variant,
                   impl_->state.registers.pc,
                   "installation is outside the pending code transition");
  }
  impl_->installed.reset();
  impl_->views.clear();
  {
    impl_->executing = true;
    const auto release = llvm::make_scope_exit([&] { impl_->executing = false; });
    const auto storage = detail::direct_memory(*image.impl_->image, variant, impl_->memory);
    impl_->pointers.resize(detail::register_pointer_count);
    impl_->pointers[detail::io_read_slot] =
        impl_->memory.io.read == nullptr ? nullptr : std::bit_cast<void *>(&read_io);
    impl_->pointers[detail::io_write_slot] =
        impl_->memory.io.write == nullptr ? nullptr : std::bit_cast<void *>(&write_io);
    impl_->pointers.insert(impl_->pointers.end(), storage.begin(), storage.end());
    detail::validate_code_storage(*image.impl_->image, variant, impl_->memory);
    if (image.impl_->image->options.memory_mode == CompileOptions::MemoryMode::direct) {
      const auto &variants = image.impl_->image->code.variants;
      const auto &layout = std::ranges::find(variants, variant, &CodeVariant::id)->layout.regions;
      impl_->views = detail::host_views(layout, storage, impl_->memory.views());
    }
  }
  impl_->installed = image.impl_;
  impl_->variant = variant;
  impl_->native_call.bind(image.impl_->text_ranges, impl_->views);
  return image.impl_->report;
}

bool Core::host_fault(std::uintptr_t address, bool writing, std::uintptr_t native_pc) noexcept {
  return detail::NativeCall::claim(address, writing, native_pc);
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
  impl_->yield_requested = 1;
}

bool Core::interrupt_ready(const InterruptRequest &request) const noexcept {
  constexpr unsigned first_maskable = 2;
  constexpr unsigned reserved = 7;
  const auto &state = impl_->state;
  const auto &registers = state.registers;
  if (impl_->executing || !impl_->installed || state.has_fault ||
      state.instruction_set != InstructionSet::isa || (registers.mode2 & timer_enable) != 0 ||
      impl_->delayed_target != 0 || impl_->loops.blocks_interrupt_at_setup(registers) ||
      state.progress.base_cycles < impl_->loops.interrupt_deadline || impl_->transfer ||
      request.source < first_maskable || request.source == reserved ||
      request.source >= detail::interrupt_source_count ||
      (registers.mode1 & interrupt_enable) == 0 ||
      registers.pc_stack_pointer >= Registers::pc_stack_capacity ||
      (saves_interrupt_status(request.source) &&
       registers.status_depth >= registers.status_stack.size()) ||
      !interrupt_line(request.source).enabled(registers) || !impl_->declared_interrupt(request) ||
      !impl_->entry_allowed()) {
    return false;
  }
  if (!impl_->interrupt_frames.empty() &&
      impl_->interrupt_frames.back().entry_instruction == state.progress.instructions) {
    return false; // The first instruction of the current vector must retire.
  }
  const auto limit = (registers.mode1 & nesting_enable) != 0
                         ? static_cast<unsigned>(request.source) + 1
                         : detail::interrupt_source_count;
  for (unsigned source = 0; source < limit; ++source) {
    if (interrupt_line(source).in_service(registers)) { return false; }
  }
  return true;
}

void Core::interrupt(const InterruptRequest &request) {
  impl_->stopped_owner();
  if (!impl_->installed) { unavailable("interrupt requires an installed image", request.vector); }
  if (!impl_->declared_interrupt(request)) {
    detail::refuse(FaultReason::undeclared_entry,
                   impl_->installed->image->identity,
                   impl_->variant,
                   request.vector,
                   "interrupt source and vector are not declared together");
  }
  if (!interrupt_ready(request)) {
    throw std::invalid_argument("SHARC interrupt is not eligible at this boundary");
  }
  auto &state = impl_->state;
  auto &registers = state.registers;
  const auto next_depth = static_cast<std::uint8_t>(registers.pc_stack_pointer + 1);
  impl_->interrupt_frames.push_back({.resume = impl_->resume,
                                     .loops = impl_->loops,
                                     .entry_instruction = state.progress.instructions,
                                     .source = request.source,
                                     .pc_depth = next_depth});
  impl_->clear_continuation();
  impl_->refresh_interrupt_frame();

  registers.pc_stack[registers.pc_stack_pointer] = registers.pc | detail::pc_sequencer_tag;
  registers.pc_stack_pointer = next_depth;
  registers.pe[0].stky &= ~(pc_stack_full | pc_stack_empty);
  if (next_depth == Registers::pc_stack_capacity) { registers.pe[0].stky |= pc_stack_full; }
  if (next_depth == Registers::pc_stack_capacity - 1) {
    registers.irptl |= stack_overflow_interrupt;
  }
  if (saves_interrupt_status(request.source)) {
    registers.status_stack[registers.status_depth++] = {.mode1 = registers.mode1,
                                                        .astat_x = registers.pe[0].astat,
                                                        .astat_y = registers.pe[1].astat};
    registers.mode1 &= ~registers.mmask;
    registers.pe[0].stky &= ~status_stack_empty;
  }
  interrupt_line(request.source).set_service(registers, true);
  clear_in_service_latches(registers);
  registers.pc = request.vector;
  registers.daddr = (request.vector + detail::loop_decode_distance) & detail::maximum_pc;
  registers.faddr = (request.vector + detail::loop_fetch_distance) & detail::maximum_pc;
  registers.emuclk += vector_refill_cycles;
  state.progress.base_cycles += vector_refill_cycles;
  state.execution = ExecutionState::stopped;
}

void Core::reset(std::uint32_t address, InstructionSet instruction_set) {
  impl_->stopped_owner();
  if (address > detail::maximum_pc) {
    throw std::invalid_argument("reset address exceeds 24 bits");
  }
  if (instruction_set != InstructionSet::isa && instruction_set != InstructionSet::visa) {
    throw std::invalid_argument("unknown reset instruction set");
  }

  constexpr std::uint32_t mode2_reset = 0x42000000;
  constexpr std::uint32_t sticky_reset = 0x05400000;
  constexpr std::uint32_t interrupt_mask_reset = 0x00000003;
  constexpr std::uint32_t interrupt_mode_mask_reset = 0x00200000;
  constexpr std::uint32_t flag_level_mask = 0x55555555; // FLAGS pin levels are even bits.
  auto &state = impl_->state;
  auto &registers = state.registers;
  // The consumer supplies the restart address. Fetch and Decode start empty.
  registers.pc = registers.faddr = registers.daddr = address;
  registers.pc_stack_pointer = registers.loop_address_depth = registers.loop_counter_depth = 0;
  registers.status_depth = 0;
  for (auto &pe : registers.pe) {
    pe.astat = 0;
    pe.stky = sticky_reset;
  }
  registers.mode1 = 0;
  registers.mode2 = mode2_reset;
  registers.mmask = interrupt_mode_mask_reset;
  registers.flags &= flag_level_mask;
  registers.irptl = registers.imaskp = registers.lirptl = 0;
  registers.imask = interrupt_mask_reset;
  registers.ustat = {};

  state.instruction_set = instruction_set;
  state.execution = ExecutionState::stopped;
  state.has_fault = false;
  state.fault = {};
  impl_->yield_requested = 0;
  impl_->clear_continuation();
  impl_->transfer.reset();
  impl_->interrupt_frames.clear();
  impl_->refresh_interrupt_frame();
}

void Core::resume_from_stop() {
  impl_->stopped_owner();
  unavailable("SHARC IDLE requires interrupt vector entry", impl_->state.registers.pc);
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
  if (impl_->transfer) {
    throw std::logic_error("cannot replace registers during a pending code transition");
  }
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
  if (registers.pc != impl_->state.registers.pc) { impl_->clear_continuation(); }
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
                     .yield_requested = impl_->yield_requested != 0,
                     .resume = impl_->resume,
                     .transfer = impl_->transfer,
                     .pipeline_history = impl_->pipeline_history,
                     .delayed_target = impl_->delayed_target,
                     .loops = impl_->loops,
                     .interrupt_frames = impl_->interrupt_frames});
  return snapshot;
}

void Core::restore(const Snapshot &snapshot) {
  impl_->stopped_owner();
  if (!snapshot.impl_ || snapshot.impl_->mode != impl_->mode) {
    throw std::invalid_argument("empty snapshot or incompatible numerical mode");
  }
  auto restored = *snapshot.impl_;
  impl_->state = std::move(restored.state);
  impl_->yield_requested = static_cast<std::uint8_t>(restored.yield_requested);
  impl_->resume = std::move(restored.resume);
  impl_->transfer = std::move(restored.transfer);
  impl_->pipeline_history = restored.pipeline_history;
  impl_->delayed_target = restored.delayed_target;
  impl_->loops = restored.loops;
  impl_->interrupt_frames = std::move(restored.interrupt_frames);
  impl_->refresh_interrupt_frame();
}

void Core::Impl::refresh_interrupt_frame() noexcept {
  interrupt_frame_guard = 0;
  if (!interrupt_frames.empty()) {
    const auto &frame = interrupt_frames.back();
    interrupt_frame_guard =
        frame.pc_depth |
        (saves_interrupt_status(frame.source) ? detail::interrupt_status_frame : 0);
  }
}

void Core::Impl::return_from_interrupt() {
  auto &registers = state.registers;
  auto &frame = interrupt_frames.back();
  if (saves_interrupt_status(frame.source)) {
    const auto &status = registers.status_stack[--registers.status_depth];
    registers.mode1 = status.mode1;
    registers.pe[0].astat = status.astat_x;
    registers.pe[1].astat = status.astat_y;
    if (registers.status_depth == 0) { registers.pe[0].stky |= status_stack_empty; }
  }
  resume = std::move(frame.resume);
  frame.loops.interrupt_deadline =
      std::max(frame.loops.interrupt_deadline, loops.interrupt_deadline);
  loops = frame.loops;
  loops.observe(registers);
  interrupt_frames.pop_back();
  refresh_interrupt_frame();
}

bool Core::Impl::enter(std::size_t index, detail::ControlWords &control) {
  executing = true;
  const auto release = llvm::make_scope_exit([&] { executing = false; });
  if (views.empty()) {
    installed->entries[index](pointers.data(), control.data());
    return true;
  }
  return native_call.invoke(installed->entries[index], pointers.data(), control.data());
}

void Core::Impl::observe_pipeline(Progress retired) {
  if (retired.instructions == 0) { return; }
  auto &registers = state.registers;
  if (!interrupt_frames.empty()) { clear_in_service_latches(registers); }
  if (delayed_target == 0) {
    if (registers.loop_address_depth != 0 || loops.refill != 0 ||
        loops.short_loop[detail::short_addresses] != 0) {
      loops.observe(registers);
    }
    return;
  }
  const auto target = static_cast<std::uint32_t>(delayed_target) & detail::maximum_pc;
  const bool first = (delayed_target & detail::before_first_delay_slot) != 0;
  registers.faddr = (delayed_target & detail::delayed_fetch_live) != 0
                        ? static_cast<std::uint32_t>(delayed_target >> detail::delayed_fetch_shift)
                        : (target + (first ? 1U : 2U)) & detail::maximum_pc;
  registers.daddr = first ? (registers.pc + 2) & detail::maximum_pc : target;
}

void Core::Impl::enter_idle() {
  auto &registers = state.registers;
  if (delayed_target == 0 && registers.loop_address_depth == 0 && !loops.active()) {
    // IDLE leaves its successor in Decode and stops Fetch1 two words later.
    registers.daddr = registers.pc;
    registers.faddr = (registers.pc + detail::loop_decode_distance) & detail::maximum_pc;
  }
  state.execution = ExecutionState::sleeping;
}

bool Core::Impl::continue_after(detail::NativeExit reason) {
  switch (reason) {
    case detail::NativeExit::interrupt_return:
      interrupt_line(interrupt_frames.back().source).set_service(state.registers, false);
      [[fallthrough]];
    case detail::NativeExit::interrupt_return_redirect: return_from_interrupt(); return true;
    case detail::NativeExit::interrupt_return_slots:
      interrupt_line(interrupt_frames.back().source).set_service(state.registers, false);
      return true;
    case detail::NativeExit::indirect:
    case detail::NativeExit::returned: resume.reset(); return true;
    case detail::NativeExit::sleeping: enter_idle(); return false;
    default: return false;
  }
}

std::optional<RunResult> Core::Impl::open_run(bool has_budget) {
  if (state.has_fault) {
    return RunResult{
        .progress = {}, .execution = state.execution, .has_fault = true, .fault = state.fault};
  }
  if (state.execution == ExecutionState::sleeping) {
    return RunResult{.progress = {}, .execution = state.execution, .has_fault = false, .fault = {}};
  }
  if (transfer) {
    if (!has_budget || !installed || !transfer->accepts(*installed->image, variant)) {
      return RunResult{.progress = {},
                       .execution = ExecutionState::code_transfer,
                       .has_fault = false,
                       .fault = {}};
    }
    const auto pc = state.registers.pc;
    resume = Resume{.identity = transfer->identity, .variant = variant, .address = pc, .entry = pc};
    transfer.reset();
  }
  if (!installed) {
    return fail(FaultReason::unprepared, "execution requires an installed native image");
  }
  return std::nullopt;
}

const CodeTransition *Core::Impl::transfer_target() const {
  const auto pc = state.registers.pc;
  const auto &transitions = installed->image->code.transitions;
  const auto edge = std::ranges::find_if(transitions, [&](const auto &candidate) {
    return candidate.from_variant == variant && candidate.address == pc;
  });
  if (edge == transitions.end()) { return nullptr; }
  constexpr auto parcels_per_word = detail::full_instruction_bits / detail::short_instruction_bits;
  const auto resident = std::ranges::any_of(installed_variant()->spans, [&](const CodeSpan &span) {
    const auto words = span.parcels.size() / parcels_per_word;
    return pc >= span.address && pc - span.address < words;
  });
  return resident ? nullptr : &*edge;
}

std::optional<RunResult> Core::Impl::native_stop(const detail::ControlWords &control,
                                                 Progress progress) {
  const auto reason = static_cast<detail::NativeExit>(control[detail::exit_reason]);
  switch (reason) {
    case detail::NativeExit::bound:
    case detail::NativeExit::undeclared:
    case detail::NativeExit::indirect:
    case detail::NativeExit::returned:
      if (control[detail::retired_instructions] != 0) {
        if (const auto *edge = transfer_target()) {
          transfer =
              PendingTransfer{.identity = installed->image->identity, .targets = edge->to_variants};
          state.execution = ExecutionState::code_transfer;
          return RunResult{
              .progress = progress, .execution = state.execution, .has_fault = false, .fault = {}};
        }
      }
      break;
    default: break;
  }
  return native_failure(control, progress);
}

RunResult Core::Impl::execute(std::uint64_t instructions, std::uint64_t cycles) {
  idle_owner();
  if (const auto blocked = open_run(instructions != 0 && cycles != 0)) { return *blocked; }
  const auto &image = *installed->image;
  const auto &registers = state.registers;
  Progress progress;
  const auto failure = [&](FaultReason reason, std::string message) {
    auto result = fail(reason, std::move(message));
    result.progress = progress;
    return result;
  };
  for (;;) {
    if (!entry_allowed()) {
      return failure(FaultReason::undeclared_entry,
                     "execution PC is not a declared entry or saved continuation");
    }
    if (state.instruction_set != InstructionSet::isa || (registers.mode2 & timer_enable) != 0) {
      return failure(FaultReason::unsupported_feature,
                     "VISA execution or an enabled core timer is not implemented");
    }
    if (yield_requested != 0) { return yield(progress); }
    const auto cycle_budget = cycles - std::min(cycles, progress.base_cycles);
    if (instructions == progress.instructions || cycle_budget == 0) { break; }
    const auto index = prepared_unit();
    if (!index) { return failure(FaultReason::unsupported_feature, "no prepared MODE1 context"); }
    detail::ControlWords control{};
    control[detail::instruction_budget] = instructions - progress.instructions;
    control[detail::cycle_budget] = cycle_budget;
    state.execution = ExecutionState::running;
    if (!enter(*index, control)) { return host_failure(progress); }
    const Progress retired{.instructions = control[detail::retired_instructions],
                           .base_cycles = control[detail::retired_cycles]};
    observe_pipeline(retired);
    progress.instructions += retired.instructions;
    progress.base_cycles += retired.base_cycles;
    state.progress.instructions += retired.instructions;
    state.progress.base_cycles += retired.base_cycles;
    resume = Resume{.identity = image.identity,
                    .variant = variant,
                    .address = state.registers.pc,
                    .entry = static_cast<std::uint32_t>(control[detail::resume_entry])};
    if (const auto result = native_stop(control, progress)) { return *result; }
    const auto reason = static_cast<detail::NativeExit>(control[detail::exit_reason]);
    if (!continue_after(reason) || yield_requested != 0 || progress.instructions == instructions ||
        progress.base_cycles >= cycles) {
      break;
    }
  }
  if (yield_requested != 0) { return yield(progress); }
  if (state.execution != ExecutionState::sleeping) { state.execution = ExecutionState::stopped; }
  return {.progress = progress, .execution = state.execution, .has_fault = false, .fault = {}};
}

RunResult Core::Impl::host_failure(Progress progress) {
  terminal_fault = true;
  const auto record = native_call.fault();
  auto result = fail(FaultReason::access_fault,
                     std::string("host ") + (record.writing ? "write" : "read") + ", address " +
                         std::to_string(record.address));
  result.progress = progress;
  const auto site = std::ranges::find_if(installed->sites, [&](const auto &candidate) {
    return record.pc >= candidate.address && record.pc - candidate.address < candidate.size;
  });
  if (site == installed->sites.end()) {
    result.fault.reason = FaultReason::invalid_image;
    result.fault.detail += ": native PC has no guest line attribution";
  } else {
    result.fault.variant = static_cast<std::uint32_t>(site->variant);
    result.fault.address = site->guest_pc;
    const auto access = std::ranges::find_if(installed->accesses, [&](const auto &candidate) {
      return candidate.variant == site->variant && candidate.pc == site->guest_pc;
    });
    if (access == installed->accesses.end()) {
      result.fault.reason = FaultReason::invalid_image;
      result.fault.detail += ": faulting guest instruction has no memory access metadata";
    } else {
      result.fault.instruction = access->word;
      result.fault.instruction_bits = access->bits;
      bool attributed = false;
      const auto spaces = access->spaces[record.writing ? 1 : 0];
      for (const auto &view : views) {
        if ((spaces & space_mask(view.space)) == 0 || view.writing != record.writing ||
            record.address < view.address || record.address - view.address >= view.size) {
          continue;
        }
        result.fault.detail +=
            ", space " + std::to_string(static_cast<unsigned>(view.space)) + ", word address " +
            std::to_string(
                static_cast<std::uint32_t>((record.address - view.address) / view.stride));
        attributed = true;
      }
      if (!attributed) {
        result.fault.reason = FaultReason::invalid_image;
        result.fault.detail += ": fault address is outside the guest instruction's view";
      }
    }
  }
  state.fault = result.fault;
  return result;
}
} // namespace sharc
