// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sharc/memory.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sharc {
enum class ExecutionMode : std::uint8_t { strict, fast_fp32 };
enum class InstructionSet : std::uint8_t { isa, visa };
enum class ExecutionState : std::uint8_t {
  stopped,
  running,
  sleeping,
  yielded,
  faulted,
  code_transfer
};
enum class FaultReason : std::uint8_t {
  invalid_image,
  truncated_instruction,
  invalid_encoding,
  undeclared_entry,
  unsupported_instruction,
  unprepared,
  unsupported_feature,
  access_fault
};

// Raw little-significance limbs, not host floating-point values.
struct Accumulator {
  std::uint32_t low{}, middle{};
  std::uint16_t high{};
  bool operator==(const Accumulator &) const = default;
};

struct ProcessingElement {
  // [physical bank: primary, alternate][register], low 40 bits significant.
  std::array<std::array<std::uint64_t, 16>, 2> data{};
  std::array<Accumulator, 2> multiplier{}; // Foreground, background.
  std::uint64_t bit_fifo{};
  std::uint8_t bit_fifo_write_pointer{};
  std::uint32_t astat{}, stky{};
  bool operator==(const ProcessingElement &) const = default;
};

struct AddressGenerator {
  std::uint32_t index{}, modifier{}, length{}, base{};
  bool operator==(const AddressGenerator &) const = default;
};

struct StatusFrame {
  std::uint32_t mode1{}, astat_x{}, astat_y{};
  bool operator==(const StatusFrame &) const = default;
};

struct Registers {
  static constexpr std::size_t pc_stack_capacity = 30;
  std::array<ProcessingElement, 2> pe{};                 // PEx, PEy.
  std::array<std::array<AddressGenerator, 16>, 2> dag{}; // Primary, alternate.
  std::uint32_t pc{}, faddr{}, daddr{};
  std::array<std::uint32_t, pc_stack_capacity> pc_stack{}; // Includes the two PCSTK tag bits.
  std::uint8_t pc_stack_pointer{};
  std::array<std::uint32_t, 6> loop_address_stack{}, loop_counter_stack{};
  std::uint8_t loop_address_depth{}, loop_counter_depth{};
  std::array<StatusFrame, 15> status_stack{};
  std::uint8_t status_depth{};
  std::uint32_t lcntr{};
  std::uint64_t px{}; // PX1 = low 32 bits; PX2 = high 32 bits.
  std::uint32_t mode1{}, mode2{}, mmask{}, flags{};
  std::uint32_t irptl{}, imask{}, imaskp{}, lirptl{};
  std::array<std::uint32_t, 4> ustat{};
  std::uint32_t tperiod{}, tcount{};
  std::uint64_t emuclk{}; // EMUCLK = low 32 bits; EMUCLK2 = high 32 bits.
  bool operator==(const Registers &) const = default;
};

struct Progress {
  std::uint64_t instructions{}, base_cycles{};
  bool operator==(const Progress &) const = default;
};

struct Fault {
  FaultReason reason{FaultReason::unprepared};
  std::string identity;
  std::uint32_t variant{}, address{};
  std::uint64_t instruction{}; // MSB-aligned in a 48-bit word.
  std::uint8_t instruction_bits{};
  std::string detail;
  bool operator==(const Fault &) const = default;
};

class PreparationError : public std::runtime_error {
public:
  explicit PreparationError(Fault fault);
  [[nodiscard]] const Fault &fault() const noexcept;

private:
  Fault fault_;
};

struct State {
  Registers registers;
  Progress progress;
  ExecutionState execution{ExecutionState::stopped};
  InstructionSet instruction_set{InstructionSet::isa};
  bool has_fault{};
  Fault fault;
  bool operator==(const State &) const = default;
};

struct RunResult {
  Progress progress;
  ExecutionState execution{ExecutionState::stopped};
  bool has_fault{};
  Fault fault;
};

struct CodeSpan {
  std::uint32_t address{};
  InstructionSet instruction_set{InstructionSet::isa};
  // Numerical 16-bit parcels in fetch order, most significant parcel first.
  // An ISA word is three parcels at one PC; VISA PCs count individual parcels.
  std::vector<std::uint16_t> parcels;
};

struct VariantLayout {
  std::vector<RegionLayout> regions;
  // Bind this variant through canonical regions without complete views.
  bool ordinary_regions{};
};

struct InterruptRequest {
  std::uint8_t source{};
  std::uint32_t vector{};
};

struct CodeVariant {
  std::uint32_t id{};
  std::vector<CodeSpan> spans;
  std::vector<std::uint32_t> entries;
  std::vector<InterruptRequest> interrupt_vectors;
  VariantLayout layout;
};

struct CodeTransition {
  std::uint32_t from_variant{}, address{};
  std::vector<std::uint32_t> to_variants;
};

struct CodeImage {
  std::vector<CodeVariant> variants;
  std::vector<CodeTransition> transitions;
};

struct CallbackBinding {
  std::string role, symbol;
};

struct CompileOptions {
  enum class MemoryMode : std::uint8_t { exact, direct };
  ExecutionMode mode{ExecutionMode::strict};
  MemoryMode memory_mode{MemoryMode::exact};
  std::vector<CallbackBinding> callbacks;
};

struct NativeSymbol {
  std::string name;
  std::uintptr_t address{};
};

struct NativeSymbols {
  struct TextRange {
    std::uintptr_t address{};
    std::uint64_t size{};
  };

  struct Site {
    std::uintptr_t address{};
    std::uint64_t size{}, variant{};
    std::uint32_t guest_pc{};
  };

  struct FaultMap {
    std::span<const TextRange> text_ranges;
    std::span<const Site> sites;
  };

  std::vector<NativeSymbol> entries;
  std::shared_ptr<void> lifetime;
  // Linked executable sections and guest debug-line intervals; copied by bind.
  FaultMap fault_map{};
};

struct PreparationReport {
  std::string identity;
  double seconds{};
  std::size_t native_bytes{};
};

class SemanticImage {
public:
  SemanticImage() = default;
  [[nodiscard]] std::span<const std::byte> bitcode() const noexcept;
  [[nodiscard]] std::span<const std::string> exports() const noexcept;
  [[nodiscard]] std::string_view identity() const noexcept;
  [[nodiscard]] std::vector<std::byte> artifact() const;

private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  friend class Core;
};

class NativeImage {
public:
  NativeImage() = default;
  [[nodiscard]] PreparationReport report() const;

private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  friend class Core;
};

class Snapshot {
public:
  Snapshot() = default;

private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  friend class Core;
};

// One owner thread. The numerical mode is fixed for the entire lifetime.
class Core {
public:
  explicit Core(Memory memory = {}, ExecutionMode mode = ExecutionMode::strict);
  ~Core();
  Core(const Core &other) = delete;
  Core &operator=(const Core &other) = delete;

  [[nodiscard]] static std::string_view compiler_identity() noexcept;
  [[nodiscard]] static SemanticImage lower(const CodeImage &image,
                                           const CompileOptions &options = {});
  [[nodiscard]] static SemanticImage
  load(const CodeImage &image, const CompileOptions &options, std::span<const std::byte> artifact);
  [[nodiscard]] static NativeImage bind(const SemanticImage &image, const NativeSymbols &symbols);
  [[nodiscard]] static NativeImage compile(const CodeImage &image,
                                           const CompileOptions &options = {});
  // Called with the interrupted signal mask in effect. A claimed host fault is terminal;
  // Only fault diagnostics and progress at the last completed owner return are defined.
  // Register backing may be partially materialized; it is not an architectural snapshot.
  [[nodiscard]] static bool
  host_fault(std::uintptr_t address, bool writing, std::uintptr_t native_pc) noexcept;
  [[nodiscard]] PreparationReport install(const NativeImage &image, std::uint32_t variant);
  [[nodiscard]] RunResult run(std::uint64_t instruction_budget);
  [[nodiscard]] RunResult run_until(std::uint64_t base_cycle_deadline);
  void request_yield() noexcept;
  [[nodiscard]] bool interrupt_ready(const InterruptRequest &request) const noexcept;
  void interrupt(const InterruptRequest &request);
  // Reset at the consumer's restart address, keeping the installed image and elapsed progress.
  void reset(std::uint32_t address = 0, InstructionSet instruction_set = InstructionSet::isa);
  void resume_from_stop();
  void set_memory(Memory memory);
  [[nodiscard]] Registers registers() const;
  void set_registers(const Registers &registers);
  [[nodiscard]] const State &state() const noexcept;
  [[nodiscard]] Snapshot save() const;
  void restore(const Snapshot &snapshot);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace sharc
