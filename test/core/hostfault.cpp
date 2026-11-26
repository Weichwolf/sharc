// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "../support/native_provider.hpp"
#include "../support/signals.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {
using fixture::check;
using MemoryMode = sharc::CompileOptions::MemoryMode;
constexpr std::uint64_t words = std::uint64_t{1} << 32, view_bytes = words * 4;
constexpr std::uint32_t ram_id = 11, ram_address = 0x90000, last_pc = 0xffffff;
constexpr std::uint64_t load_r0 = 0xa00000000000, store_r0 = 0xa10000000000;
constexpr std::uint64_t program_dag = std::uint64_t{1} << 44;
constexpr std::uint64_t raw_x = 0x12345678ab, raw_y = 0x98765432cd;
constexpr std::uint32_t simd = 1U << 21, expected_word = 0x13579bdf;
constexpr unsigned instruction_bits = 48, byte_bits = 8, raw_shift = 8;
constexpr int handler_failure = 90, unexpected_return = 92;

int claim_fault(const void *address, int writing, unsigned long pc) noexcept {
  return sharc::Core::host_fault(reinterpret_cast<std::uintptr_t>(address), writing != 0, pc) ? 1
                                                                                              : 0;
}

class ConsumerHandler {
public:
  ConsumerHandler() {
    check(sharc_test_signals_begin(claim_fault) == 0, "could not install consumer fault handler");
  }

  ~ConsumerHandler() { sharc_test_signals_end(); }

  ConsumerHandler(const ConsumerHandler &) = delete;
  ConsumerHandler &operator=(const ConsumerHandler &) = delete;
};

void check_mask() {
  check(sharc_test_mask_restored() != 0,
        "escape lost interrupted mask or retained handler-only blocking");
}

enum class Protection : std::uint8_t { read = PROT_READ, read_write = PROT_READ | PROT_WRITE };

struct Cell {
  std::uint32_t address{}, word{};
};

class Space {
public:
  explicit Space(bool truncated_file = false) {
    const auto page = sysconf(_SC_PAGESIZE);
    check(page > 0, "invalid host page size");
    page_size_ = static_cast<std::size_t>(page);
    const int fd = truncated_file ? memfd_create("sharc-hostfault", MFD_CLOEXEC) : -1;
    if (truncated_file && (fd < 0 || ftruncate(fd, page) != 0)) {
      if (fd >= 0) { (void)close(fd); }
      throw std::runtime_error("could not create short backing file");
    }
    data_ = mmap(nullptr,
                 view_bytes,
                 truncated_file ? PROT_READ | PROT_WRITE : PROT_NONE,
                 MAP_PRIVATE | MAP_NORESERVE | (truncated_file ? 0 : MAP_ANONYMOUS),
                 fd,
                 0);
    if (fd >= 0) { (void)close(fd); }
    check(data_ != MAP_FAILED, "could not reserve complete word space");
  }

  ~Space() { (void)munmap(data_, view_bytes); }

  Space(const Space &) = delete;
  Space &operator=(const Space &) = delete;

  [[nodiscard]] std::span<std::byte> bytes() const {
    return {static_cast<std::byte *>(data_), view_bytes};
  }

  void allow(std::uint32_t address, Protection protection) const {
    const auto offset = (std::uint64_t{address} * 4 / page_size_) * page_size_;
    check(mprotect(bytes().data() + offset, page_size_, static_cast<int>(protection)) == 0,
          "could not protect page");
  }

  void put(Cell cell) const {
    const auto offset = std::uint64_t{cell.address} * 4;
    // LLVM 19 SLP truncates large pointer distances. Keep this host-side
    // initialization independent of the guest compiler under test.
    volatile std::byte *data = bytes().data();
    for (unsigned byte = 0; byte < 4; ++byte) {
      data[offset + byte] = static_cast<std::byte>(cell.word >> (byte * byte_bits));
    }
  }

  [[nodiscard]] std::uint32_t get(std::uint32_t address) const {
    std::uint32_t result{};
    const auto offset = std::uint64_t{address} * 4;
    const volatile std::byte *data = bytes().data();
    for (unsigned byte = 0; byte < 4; ++byte) {
      result |= std::uint32_t{std::to_integer<std::uint8_t>(data[offset + byte])}
                << (byte * byte_bits);
    }
    return result;
  }

  [[nodiscard]] std::size_t page_size() const { return page_size_; }

private:
  std::size_t page_size_{};
  void *data_{};
};

sharc::Memory lend(fixture::Program &program,
                   const Space &space,
                   sharc::MemorySpace target,
                   bool writable = true) {
  program.code.variants[0].layout.regions.push_back({.space = target,
                                                     .view = sharc::MemoryView::normal_word,
                                                     .first = 0,
                                                     .count = words,
                                                     .storage_id = ram_id,
                                                     .byte_offset = 0,
                                                     .byte_stride = 4,
                                                     .readable = true,
                                                     .writable = writable,
                                                     .initialized = true});
  auto memory = program.memory();
  memory.regions.push_back({.storage_id = ram_id, .bytes = space.bytes(), .generation = 1});
  return memory;
}

sharc::Registers initial(std::uint32_t pc, sharc::MemorySpace target, std::uint32_t address) {
  sharc::Registers result;
  result.pc = pc;
  result.dag[0][target == sharc::MemorySpace::data ? 0U : 8U].index = address;
  result.pe[0].data[0][0] = raw_x;
  result.pe[1].data[0][0] = raw_y;
  return result;
}

sharc::NativeImage consumer_compile(const sharc::CodeImage &code,
                                    const sharc::CompileOptions &options,
                                    fixture::ConsumerEffects effects = {},
                                    std::uintptr_t *entry_address = nullptr) {
  const auto semantic = sharc::Core::lower(code, options);
  const auto loaded = sharc::Core::load(code, options, semantic.artifact());
  auto provider = std::make_shared<fixture::Provider>();
  auto symbols = fixture::link(loaded, provider, false, effects);
  if (entry_address != nullptr) { *entry_address = symbols.entries.front().address; }
  auto missing = symbols;
  missing.fault_map = {};
  fixture::throws<sharc::PreparationError>([&] { (void)sharc::Core::bind(loaded, missing); },
                                           "direct binding accepted absent fault attribution");
  const auto native = sharc::Core::bind(loaded, symbols);
  // Actual faults below must still resolve after the provider's input arrays are gone.
  provider->ranges.clear();
  provider->sites.clear();
  return native;
}

struct Case {
  sharc::MemorySpace space{sharc::MemorySpace::data};
  std::uint32_t pc{};
  bool writing{}, consumer{}, prefix{}, split{}, paired{}, bus{};
};

void nonresumable(sharc::Core &core,
                  const sharc::NativeImage &native,
                  sharc::Memory memory,
                  const sharc::Snapshot &snapshot) {
  const auto before = core.state();
  check(core.run(1).fault == before.fault &&
            core.run_until(std::numeric_limits<std::uint64_t>::max()).progress == sharc::Progress{},
        "terminal instance resumed execution");
  fixture::throws<std::logic_error>([&] { core.restore(snapshot); }, "terminal instance restored");
  fixture::throws<std::logic_error>([&] { core.set_registers(before.registers); },
                                    "terminal registers replaced");
  fixture::throws<std::logic_error>([&] { core.set_memory(memory); }, "terminal memory rebound");
  fixture::throws<std::logic_error>(
      [&] { (void)core.install(native, fixture::Program::variant_id); },
      "terminal image reinstalled");
  fixture::throws<std::logic_error>([&] { (void)core.save(); },
                                    "terminal instance saved as a boundary snapshot");
  check(core.state() == before, "rejected terminal mutation changed state");
}

void terminal_access(sharc::ExecutionMode mode, Case item) {
  const Space space(item.bus);
  const auto page_words = static_cast<std::uint32_t>(space.page_size() / 4);
  auto address = item.bus ? page_words : ram_address;
  if (item.paired) {
    space.allow(address, Protection::read_write);
    address += page_words - 1;
    space.put({.address = address, .word = expected_word});
  } else if (item.writing && !item.bus) {
    space.allow(address, Protection::read_write);
    space.put({.address = address, .word = expected_word});
    space.allow(address, Protection::read);
  }
  const auto op = (item.writing ? store_r0 : load_r0) |
                  (item.space == sharc::MemorySpace::program ? program_dag : 0);
  const std::array one{op};
  const std::array prefix{fixture::immediate(1, 7), op};
  fixture::Program program(item.prefix ? std::span<const std::uint64_t>(prefix) : one, item.pc);
  const auto memory = lend(program, space, item.space, !item.writing || item.paired || item.bus);
  const sharc::CompileOptions options{
      .mode = mode, .memory_mode = MemoryMode::direct, .callbacks = {}};
  const auto native = item.consumer ? consumer_compile(program.code, options)
                                    : sharc::Core::compile(program.code, options);
  sharc::Core core(memory, mode);
  auto registers = initial(item.pc, item.space, address);
  registers.mode1 = item.paired ? simd : 0;
  core.set_registers(registers);
  (void)core.install(native, fixture::Program::variant_id);
  if (item.split) {
    check(core.run(1).progress.instructions == 1 &&
              core.registers().pe[0].data[0][1] == (std::uint64_t{7} << raw_shift),
          "prefix did not retire");
  }
  const auto snapshot = core.save();
  const auto before = core.state();
  const auto result = core.run(item.prefix && !item.split ? 2 : 1);
  const auto fault_word = address + (item.paired ? 1U : 0U);
  check(result.has_fault && result.execution == sharc::ExecutionState::faulted &&
            result.fault.reason == sharc::FaultReason::access_fault &&
            result.fault.address == item.pc + (item.prefix ? 1U : 0U) &&
            result.fault.variant == fixture::Program::variant_id &&
            result.fault.identity == native.report().identity && result.fault.instruction == op &&
            result.fault.instruction_bits == instruction_bits &&
            result.fault.detail.contains(item.writing ? "host write" : "host read") &&
            result.fault.detail.contains("word address " + std::to_string(fault_word)) &&
            result.fault.detail.contains("space " +
                                         std::to_string(static_cast<unsigned>(item.space))) &&
            result.progress == sharc::Progress{} && core.state().progress == before.progress &&
            (sharc_test_last_was_bus() != 0) == item.bus,
        "terminal access lost native fault attribution or completed owner progress: " +
            result.fault.detail);
  check_mask();
  if (item.paired && item.writing) {
    check(space.get(address) == raw_x >> raw_shift,
          "terminal implicit store fault lost the preceding explicit word store");
  }
  nonresumable(core, native, memory, snapshot);
}

const volatile std::uint32_t *cpp_target{};

void cpp_fault() noexcept {
  const auto value = *cpp_target;
  (void)value;
}

void chaining(bool outside_view) {
  const auto child = fork();
  check(child >= 0, "could not fork chaining fixture");
  if (child == 0) {
    if (sharc_test_chain_previous() != 0) { _exit(handler_failure); }
    const ConsumerHandler handlers;
    const Space space;
    const Space outside;
    const std::array op{load_r0};
    fixture::Program program(op, 0);
    const auto memory = lend(program, space, sharc::MemorySpace::data);
    fixture::ConsumerEffects effects;
    if (outside_view) {
      effects.fault_address = reinterpret_cast<std::uintptr_t>(outside.bytes().data());
    } else {
      cpp_target = reinterpret_cast<volatile std::uint32_t *>(space.bytes().data());
      effects.probe = cpp_fault;
    }
    const auto native = consumer_compile(
        program.code, {.memory_mode = MemoryMode::direct, .callbacks = {}}, effects);
    sharc::Core core(memory);
    core.set_registers(initial(0, sharc::MemorySpace::data, ram_address));
    (void)core.install(native, fixture::Program::variant_id);
    (void)core.run(1);
    _exit(unexpected_return);
  }
  check(sharc_test_wait_for_child(child) == 0,
        "fault outside generated code or lent view was claimed, or chaining lost handler mask");
}

void exact_reference(sharc::ExecutionMode mode) {
  const Space space;
  const std::array instructions{fixture::immediate(1, 7), store_r0};
  fixture::Program program(instructions, 0);
  const auto memory = lend(program, space, sharc::MemorySpace::data, false);
  program.code.variants[0].layout.ordinary_regions = true;
  const sharc::CompileOptions defaults{.mode = mode, .callbacks = {}};
  check(defaults.memory_mode == MemoryMode::exact, "memory mode does not default to exact");
  const auto exact = sharc::Core::lower(program.code, defaults);
  const sharc::CompileOptions direct{
      .mode = mode, .memory_mode = MemoryMode::direct, .callbacks = {}};
  const auto other = sharc::Core::lower(program.code, direct);
  check(exact.identity() != other.identity(), "memory contracts share prepared identities");
  fixture::throws<sharc::PreparationError>(
      [&] { (void)sharc::Core::load(program.code, direct, exact.artifact()); },
      "exact artifact loaded as direct");
  fixture::throws<sharc::PreparationError>(
      [&] { (void)sharc::Core::load(program.code, defaults, other.artifact()); },
      "direct artifact loaded as exact");
  auto invalid = defaults;
  invalid.memory_mode = static_cast<MemoryMode>(std::numeric_limits<std::uint8_t>::max());
  fixture::throws<sharc::PreparationError>([&] { (void)sharc::Core::lower(program.code, invalid); },
                                           "unknown memory mode accepted");
  const auto native = sharc::Core::compile(program.code, defaults);
  sharc::Core core(memory, mode);
  auto expected = initial(0, sharc::MemorySpace::data, ram_address);
  core.set_registers(expected);
  (void)core.install(native, fixture::Program::variant_id);
  const auto snapshot = core.save();
  const auto before_signal = sharc_test_signal_count();
  const auto result = core.run(2);
  expected.pe[0].data[0][1] = std::uint64_t{7} << raw_shift;
  expected.pc = 1;
  expected.faddr = 5;
  expected.daddr = 3;
  expected.emuclk = 1;
  check(result.has_fault && result.fault.reason == sharc::FaultReason::access_fault &&
            result.fault.address == 1 && result.fault.instruction == store_r0 &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            core.registers() == expected && sharc_test_signal_count() == before_signal,
        "exact fault lost precise prefix or entered consumer host handler");
  core.restore(snapshot);
  check(core.run(1).progress.instructions == 1 && core.registers() == expected,
        "architectural checked fault incorrectly terminated the instance");

  auto &region = program.code.variants[0].layout.regions.back();
  region.first = ram_address;
  region.count = 1;
  const auto sparse = sharc::Core::compile(program.code, direct);
  sharc::Core fallback(memory, mode);
  fallback.set_registers(initial(0, sharc::MemorySpace::data, ram_address));
  (void)fallback.install(sparse, fixture::Program::variant_id);
  const auto boundary = fallback.save();
  const auto denied = fallback.run(2);
  check(denied.has_fault && denied.fault.reason == sharc::FaultReason::access_fault &&
            denied.fault.address == 1 && denied.progress == result.progress &&
            fallback.registers() == expected && sharc_test_signal_count() == before_signal,
        "sparse Direct layout lost precise checked fallback");
  fallback.restore(boundary);
  check(!fallback.run(1).has_fault && fallback.registers() == expected,
        "checked Direct fallback incorrectly terminated the instance");
}

void wrapping(sharc::ExecutionMode mode, sharc::MemorySpace target) {
  constexpr auto last = std::numeric_limits<std::uint32_t>::max();
  const Space space;
  space.allow(0, Protection::read_write);
  space.allow(last, Protection::read_write);
  space.put({.address = 0, .word = expected_word});
  space.put({.address = last, .word = ~expected_word});
  const auto dag = target == sharc::MemorySpace::program ? program_dag : 0;
  const std::array instructions{load_r0 | dag, store_r0 | dag};
  fixture::Program program(instructions, 0);
  const auto memory = lend(program, space, target);
  const auto native = sharc::Core::compile(
      program.code, {.mode = mode, .memory_mode = MemoryMode::direct, .callbacks = {}});
  sharc::Core core(memory, mode);
  auto expected = initial(0, target, last);
  expected.mode1 = simd;
  core.set_registers(expected);
  (void)core.install(native, fixture::Program::variant_id);
  const auto first = core.run(1);
  expected.pe[0].data[0][0] = std::uint64_t{~expected_word} << raw_shift;
  expected.pe[1].data[0][0] = std::uint64_t{expected_word} << raw_shift;
  expected.pc = 1;
  expected.faddr = 5;
  expected.daddr = 3;
  expected.emuclk = 1;
  check(!first.has_fault &&
            first.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            core.registers() == expected,
        "direct SIMD load lost normal-word top-to-zero wrap");
  expected.pe[0].data[0][0] = raw_x;
  expected.pe[1].data[0][0] = raw_y;
  core.set_registers(expected);
  const auto second = core.run(1);
  expected.pc = 2;
  expected.faddr = 6;
  expected.daddr = 4;
  expected.emuclk = 2;
  check(!second.has_fault &&
            second.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            core.registers() == expected && space.get(last) == raw_x >> raw_shift &&
            space.get(0) == raw_y >> raw_shift,
        "direct SIMD store lost wrap or changed raw register extensions");
}

void constant_wrapping(sharc::ExecutionMode mode, MemoryMode memory_mode) {
  constexpr auto last = std::numeric_limits<std::uint32_t>::max();
  constexpr unsigned i0_code = 0x10;
  const Space space;
  space.allow(0, Protection::read_write);
  space.allow(last, Protection::read_write);
  const std::array instructions{fixture::immediate(i0_code, last), store_r0};
  fixture::Program program(instructions, 0);
  const auto memory = lend(program, space, sharc::MemorySpace::data);
  const auto native = sharc::Core::compile(
      program.code, {.mode = mode, .memory_mode = memory_mode, .callbacks = {}});
  sharc::Core core(memory, mode);
  auto expected = initial(0, sharc::MemorySpace::data, 0);
  expected.mode1 = simd;
  core.set_registers(expected);
  (void)core.install(native, fixture::Program::variant_id);
  const auto result = core.run(2);
  expected.dag[0][0].index = last;
  expected.pc = 2;
  expected.faddr = 6;
  expected.daddr = 4;
  expected.emuclk = 4; // Two complete instructions and the documented two-cycle DAG interlock.
  check(!result.has_fault &&
            result.progress == sharc::Progress{.instructions = 2, .base_cycles = 4} &&
            core.registers() == expected && space.get(last) == raw_x >> raw_shift &&
            space.get(0) == raw_y >> raw_shift,
        "constant SIMD wrap was merged across the end of the lent word space");
}

void effect_order(sharc::ExecutionMode mode, bool writing) {
  const Space space;
  if (writing) {
    space.allow(ram_address, Protection::read_write);
    space.put({.address = ram_address, .word = expected_word});
    space.allow(ram_address, Protection::read);
  }
  const std::array instructions{writing ? store_r0 : load_r0,
                                fixture::immediate(0, 7),
                                writing ? store_r0 : std::uint64_t{0}};
  fixture::Program program(instructions, 0);
  const auto memory = lend(program, space, sharc::MemorySpace::data, !writing);
  program.code.variants[0].layout.ordinary_regions = true;
  const auto native = sharc::Core::compile(
      program.code, {.mode = mode, .memory_mode = MemoryMode::direct, .callbacks = {}});
  sharc::Core core(memory, mode);
  core.set_registers(initial(0, sharc::MemorySpace::data, ram_address));
  (void)core.install(native, fixture::Program::variant_id);
  const auto result = core.run(instructions.size());
  check(result.has_fault && result.fault.reason == sharc::FaultReason::access_fault &&
            result.fault.address == 0 && result.fault.instruction == instructions[0] &&
            result.progress == sharc::Progress{},
        "dead guest load or overwritten store lost the first protected access");
}

void cross_unit_fault(sharc::ExecutionMode mode) {
  const Space space;
  constexpr std::uint32_t target = 2;
  constexpr std::uint64_t jump_m8_i8 = 0x083e00000000;
  const std::array instructions{fixture::immediate(6, 7), jump_m8_i8, load_r0};
  fixture::Program program(instructions, 0);
  program.code.variants[0].entries.push_back(target);
  const auto memory = lend(program, space, sharc::MemorySpace::data);
  const auto native = sharc::Core::compile(
      program.code, {.mode = mode, .memory_mode = MemoryMode::direct, .callbacks = {}});
  sharc::Core core(memory, mode);
  auto registers = initial(0, sharc::MemorySpace::data, ram_address);
  registers.dag[0][8].index = target;
  core.set_registers(registers);
  (void)core.install(native, fixture::Program::variant_id);
  const auto snapshot = core.save();
  const auto result = core.run(instructions.size());
  // A native unit exit can materialize unpinned registers before the owner returns.
  // Terminal register backing is intentionally not compared to an architectural state.
  check(result.has_fault && result.fault.reason == sharc::FaultReason::access_fault &&
            result.fault.address == target && result.fault.instruction == load_r0 &&
            result.fault.variant == fixture::Program::variant_id &&
            result.fault.detail.contains("word address " + std::to_string(ram_address)) &&
            result.progress == sharc::Progress{} && core.state().progress == sharc::Progress{},
        "cross-unit terminal access lost attribution or fabricated owner progress");
  check_mask();
  nonresumable(core, native, memory, snapshot);
}

struct ProbeContext {
  sharc::Core *nested{};
  std::uintptr_t view{}, active_pc{}, other_pc{};
  bool cpp_rejected{}, other_rejected{}, thread_rejected{}, nested_fault{};
  unsigned destroyed{};
};

ProbeContext *current_probe{};

struct ProbeScope {
  unsigned &destroyed;

  ~ProbeScope() { ++destroyed; }
};

void owner_probe() noexcept {
  auto &context = *current_probe;
  const ProbeScope scope{.destroyed = context.destroyed};
  context.cpp_rejected =
      !sharc::Core::host_fault(context.view, false, reinterpret_cast<std::uintptr_t>(&owner_probe));
  context.other_rejected = !sharc::Core::host_fault(context.view, false, context.other_pc);
  std::thread worker([&] {
    context.thread_rejected = !sharc::Core::host_fault(context.view, false, context.active_pc);
  });
  worker.join();
  context.nested_fault = context.nested->run(1).has_fault;
}

void nested_owners(sharc::ExecutionMode mode) {
  const Space space;
  space.allow(ram_address, Protection::read_write);
  space.put({.address = ram_address, .word = expected_word});
  const std::array instructions{load_r0};
  fixture::Program program(instructions, 0);
  const auto memory = lend(program, space, sharc::MemorySpace::data);
  const sharc::CompileOptions options{
      .mode = mode, .memory_mode = MemoryMode::direct, .callbacks = {}};
  ProbeContext context;
  context.view = reinterpret_cast<std::uintptr_t>(space.bytes().data());
  const auto nested_image = consumer_compile(program.code, options, {}, &context.other_pc);
  const auto outer_image =
      consumer_compile(program.code, options, {.probe = owner_probe}, &context.active_pc);
  sharc::Core nested(memory, mode);
  sharc::Core outer(memory, mode);
  context.nested = &nested;
  current_probe = &context;
  const auto hole = ram_address + static_cast<std::uint32_t>(space.page_size() / 4);
  nested.set_registers(initial(0, sharc::MemorySpace::data, hole));
  outer.set_registers(initial(0, sharc::MemorySpace::data, ram_address));
  (void)nested.install(nested_image, fixture::Program::variant_id);
  (void)outer.install(outer_image, fixture::Program::variant_id);
  const auto result = outer.run(1);
  check(!result.has_fault &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            outer.registers().pe[0].data[0][0] == std::uint64_t{expected_word} << raw_shift &&
            context.cpp_rejected && context.other_rejected && context.thread_rejected &&
            context.nested_fault && context.destroyed == 1 &&
            nested.state().fault.reason == sharc::FaultReason::access_fault,
        "nested owner escaped C++ scopes, claimed another image/thread, or lost its parent");
  check(!sharc::Core::host_fault(context.view, false, context.active_pc),
        "native return left an active owner");
  current_probe = nullptr;
}

void concurrent_owners(sharc::ExecutionMode mode) {
  const Space space;
  const std::array instructions{load_r0};
  fixture::Program program(instructions, 0);
  const auto memory = lend(program, space, sharc::MemorySpace::data);
  const auto native = sharc::Core::compile(
      program.code, {.mode = mode, .memory_mode = MemoryMode::direct, .callbacks = {}});
  std::array<sharc::RunResult, 2> results;
  std::array<std::thread, 2> workers;
  for (std::size_t i = 0; i < workers.size(); ++i) {
    workers[i] = std::thread([&, i] {
      sharc::Core core(memory, mode);
      core.set_registers(
          initial(0, sharc::MemorySpace::data, ram_address + static_cast<std::uint32_t>(i)));
      (void)core.install(native, fixture::Program::variant_id);
      results[i] = core.run(1);
    });
  }
  for (auto &worker : workers) { worker.join(); }
  for (std::size_t i = 0; i < workers.size(); ++i) {
    check(results[i].has_fault && results[i].fault.reason == sharc::FaultReason::access_fault &&
              results[i].fault.detail.contains("word address " + std::to_string(ram_address + i)) &&
              results[i].progress == sharc::Progress{},
          "concurrent cores shared a native escape or fault record");
  }
}
} // namespace

int main() try {
  const ConsumerHandler handlers;
  check(!sharc::Core::host_fault(0, false, 0), "inactive owner claimed a host fault");
  // Fork before preparing images or starting fixture threads.
  chaining(false);
  chaining(true);
  for (const auto mode : {sharc::ExecutionMode::strict, sharc::ExecutionMode::fast_fp32}) {
    effect_order(mode, false);
    effect_order(mode, true);
    cross_unit_fault(mode);
    constant_wrapping(mode, MemoryMode::exact);
    constant_wrapping(mode, MemoryMode::direct);
    for (const auto item :
         {Case{},
          Case{.writing = true},
          Case{.space = sharc::MemorySpace::program,
               .pc = last_pc,
               .writing = true,
               .consumer = true},
          Case{.space = sharc::MemorySpace::program, .consumer = true, .prefix = true},
          Case{.consumer = true, .prefix = true, .split = true},
          Case{.writing = true, .paired = true},
          Case{.paired = true},
          Case{.bus = true},
          Case{.writing = true, .consumer = true, .bus = true}}) {
      terminal_access(mode, item);
    }
    exact_reference(mode);
    wrapping(mode, sharc::MemorySpace::data);
    wrapping(mode, sharc::MemorySpace::program);
    nested_owners(mode);
    concurrent_owners(mode);
  }
  check_mask();
  std::cout << "native host-fault boundary passed\n";
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
