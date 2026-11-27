// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "../support/native_provider.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using fixture::check;
constexpr auto variant_id = fixture::Program::variant_id;
constexpr auto instruction_bytes = fixture::Program::word_bytes;
constexpr unsigned byte_shift = 8;
constexpr std::uint64_t preserved_raw = 0x81234567ef;
constexpr std::uint64_t negative_one_raw = 0xffffffff00;
constexpr std::uint64_t initial_clock = 123;

constexpr std::array sample_words{fixture::immediate(0, 0xffffffff),
                                  fixture::immediate(1, 1),
                                  fixture::add(2, 0, 1),
                                  fixture::add(3, 2, 1),
                                  std::uint64_t{0}};

sharc::Registers initial_registers() {
  sharc::Registers result;
  result.pc = fixture::code_address;
  result.emuclk = initial_clock;
  for (auto &lane : result.pe) { lane.data[1].fill(preserved_raw); }
  result.pe[1].data[0].fill(preserved_raw);
  return result;
}

std::uint64_t consumer_calls{};

void consumer_probe() noexcept {
  ++consumer_calls;
}

struct CodeReader {
  sharc::Core *core{};
  std::span<const std::byte> bytes;
  unsigned reads{}, reentry_refusals{};
  bool refuse{};

  static bool read(void *owner,
                   sharc::MemorySpace space,
                   sharc::MemoryView view,
                   std::uint32_t address,
                   std::uint64_t &value) noexcept {
    auto &self = *static_cast<CodeReader *>(owner);
    if (space != sharc::MemorySpace::program || view != sharc::MemoryView::instruction ||
        address < fixture::code_address) {
      return false;
    }
    const auto offset = std::size_t{address - fixture::code_address} * instruction_bytes;
    if (offset > self.bytes.size() || self.bytes.size() - offset < instruction_bytes) {
      return false;
    }
    ++self.reads;
    try {
      (void)self.core->run(1);
    } catch (const std::logic_error &) { ++self.reentry_refusals; }
    value = 0;
    for (unsigned byte = 0; byte < instruction_bytes; ++byte) {
      value |= std::uint64_t{std::to_integer<std::uint8_t>(self.bytes[offset + byte])}
               << (byte * byte_shift);
    }
    return !self.refuse;
  }
};

void installation_callbacks() {
  fixture::Program program(sample_words);
  program.code.variants[0].layout.regions.clear();
  const auto native = sharc::Core::compile(program.code);
  CodeReader reader{
      .core = nullptr, .bytes = program.bytes, .reads = 0, .reentry_refusals = 0, .refuse = false};
  sharc::Core core(
      {.regions = {}, .io = {.owner = &reader, .read = CodeReader::read, .write = nullptr}});
  reader.core = &core;
  core.set_registers(initial_registers());
  const auto before = core.state();
  (void)core.install(native, variant_id);
  check(core.state() == before && reader.reads == 5 && reader.reentry_refusals == 5,
        "installation callback changed state or re-entered execution");
  check(core.run(5).progress == sharc::Progress{.instructions = 5, .base_cycles = 5},
        "callback-validated image did not execute");
  core.set_registers(initial_registers());
  const auto before_failure = core.state();
  reader.refuse = true;
  fixture::throws<sharc::PreparationError>([&] { (void)core.install(native, variant_id); },
                                           "failed code-read callback was accepted");
  check(core.state() == before_failure, "failed code-read callback changed state");
  reader.refuse = false;
  (void)core.install(native, variant_id);
  check(core.state() == before_failure && core.run(1).progress.instructions == 1,
        "failed validation retained the owner lock");
}

sharc::NativeSymbols consumer_compile(const sharc::SemanticImage &image) {
  return fixture::link(image,
                       std::make_shared<fixture::Provider>(),
                       false,
                       {.probe = consumer_probe, .wrap_exports = true});
}

sharc::NativeImage consumer_roundtrip(const sharc::CodeImage &code,
                                      const sharc::CompileOptions &options = {}) {
  const auto semantic = sharc::Core::lower(code, options);
  const auto loaded = sharc::Core::load(code, options, semantic.artifact());
  return sharc::Core::bind(loaded, consumer_compile(loaded));
}

void cached_loop_handoff(bool compatible) {
  constexpr auto start_pc = fixture::code_address;
  constexpr auto rewritten_pc = start_pc + 2;
  constexpr auto data_address = 0x90000;
  constexpr unsigned target_variant = 8;
  constexpr unsigned loop_turns = 3;
  constexpr unsigned loop_length = 2;
  constexpr unsigned count_shift = 24;
  constexpr unsigned old_value = 4;
  constexpr unsigned new_value = 7;
  constexpr std::uint64_t do_counted =
      0x0c0000000000 | (std::uint64_t{loop_turns} << count_shift) | loop_length;
  constexpr std::uint64_t store_r0 = 0xa10000000000;
  const std::array words{
      do_counted, store_r0, fixture::immediate(1, old_value), fixture::immediate(2, 1)};
  fixture::Program program(words);
  auto alias = fixture::normal_region(data_address, 1, fixture::Program::storage_id);
  alias.byte_offset = std::uint64_t{rewritten_pc - start_pc} * instruction_bytes;
  program.code.variants.front().layout.regions.push_back(alias);
  auto target = program.code.variants.front();
  target.id = target_variant;
  target.entries.push_back(rewritten_pc);
  target.spans.front().parcels[8] = new_value;
  if (!compatible) { target.spans.front().parcels[2] = 1; }
  program.code.variants.push_back(target);
  program.code.transitions.push_back(
      {.from_variant = variant_id, .address = rewritten_pc, .to_variants = {target_variant}});
  const auto native = consumer_roundtrip(program.code);
  sharc::Core core(program.memory());
  auto initial = initial_registers();
  initial.dag[0][0].index = data_address;
  initial.pe[0].data[0][0] = new_value << byte_shift;
  fixture::install(core, native, initial);
  const auto prefix = core.run(10);
  check(prefix.progress.instructions == 2 && core.registers().pc == rewritten_pc &&
            core.registers().pe[0].data[0][1] == 0,
        "a loop code store executed the stale instruction after the handoff boundary");
  if (!compatible) {
    check(prefix.has_fault && prefix.fault.reason == sharc::FaultReason::unsupported_feature &&
              prefix.fault.detail.contains("hardware-loop continuation"),
          "active loop accepted an incompatible prepared loop shape");
    return;
  }
  check(prefix.execution == sharc::ExecutionState::code_transfer && !prefix.has_fault,
        "compatible active loop did not hand off through the cached artifact");
  sharc::Core replay(program.memory());
  replay.restore(core.save());
  (void)replay.install(native, target_variant);
  constexpr unsigned total = 1 + (loop_turns * loop_length) + 1;
  const auto completion = replay.run(total - prefix.progress.instructions);
  sharc::Core reference(program.memory());
  reference.set_registers(initial);
  (void)reference.install(native, target_variant);
  const auto whole = reference.run(total);
  check(!completion.has_fault && !whole.has_fault && whole.progress.instructions == total &&
            replay.state() == reference.state() && replay.registers().loop_counter_depth == 0 &&
            replay.registers().pe[0].data[0][1] == (new_value << byte_shift),
        "resumed hardware-loop state or progress differs from uninterrupted preparation");
}

void lifecycle() {
  fixture::Program program(sample_words);
  const auto semantic = sharc::Core::lower(program.code);
  check(!semantic.bitcode().empty() && !semantic.exports().empty(),
        "lower returned an empty image");
  const auto artifact = semantic.artifact();
  const auto loaded = sharc::Core::load(program.code, {}, artifact);
  check(loaded.identity() == semantic.identity() &&
            std::ranges::equal(loaded.bitcode(), semantic.bitcode()) &&
            std::ranges::equal(loaded.exports(), semantic.exports()),
        "artifact round trip changed image");
  fixture::throws<sharc::PreparationError>([&] { (void)sharc::Core::bind(loaded, {}); },
                                           "bind accepted missing native symbols");
  const auto native = sharc::Core::compile(program.code);
  check(native.report().native_bytes != 0 && native.report().seconds > 0,
        "stand-alone preparation did not measure native code/time");
  sharc::Core core(program.memory());
  core.set_registers(initial_registers());
  const auto original = core.state();
  (void)core.install(native, variant_id);
  check(core.state() == original, "installation changed state or time");
  const auto prefix = core.run(2);
  check(fixture::completed(prefix, 2, 2), "immediate prefix failed");
  check(core.registers().pe[0].data[0][0] == negative_one_raw &&
            core.registers().pe[0].data[0][1] == 0x100,
        "immediate moves have wrong raw views");
  const auto compute = core.run(1);
  check(fixture::completed(compute, 1, 1) && core.registers().pe[0].data[0][2] == 0 &&
            core.registers().pe[0].astat == 9,
        "native ADD lost result, flags or precise prefix");
  const auto saved = core.save();
  const auto saved_state = core.state();
  const auto finish = core.run_until(5);
  check(fixture::completed(finish, 2, 2) && core.registers().pe[0].data[0][3] == 0x100 &&
            core.registers().pe[0].astat == 0,
        "deadline did not finish the remaining native prefix");
  check(core.state().progress == sharc::Progress{.instructions = 5, .base_cycles = 5} &&
            core.registers().pc == fixture::code_address + 5 &&
            core.registers().faddr == fixture::code_address + 9 &&
            core.registers().daddr == fixture::code_address + 7 &&
            core.registers().emuclk == initial_clock + 5,
        "architectural progress or pipeline counters differ");
  check(core.registers().pe[1].data[0] == initial_registers().pe[1].data[0] &&
            core.registers().pe[0].data[1] == initial_registers().pe[0].data[1],
        "SISD changed inactive PEy or alternate raw storage");
  const auto refused = core.run(1);
  check(refused.has_fault && refused.progress == sharc::Progress{} &&
            refused.fault.reason == sharc::FaultReason::undeclared_entry &&
            refused.fault.address == fixture::code_address + 5,
        "undeclared continuation did not fault precisely");
  core.restore(saved);
  check(core.state() == saved_state, "restore retained state after the snapshot");
  sharc::Core second(program.memory());
  (void)second.install(native, variant_id);
  second.restore(saved);
  check(second.run_until(5).progress == sharc::Progress{.instructions = 2, .base_cycles = 2},
        "continuation depends on original Core address");
  const auto bound = sharc::Core::bind(loaded, consumer_compile(loaded));
  sharc::Core consumer(program.memory());
  consumer.set_registers(initial_registers());
  (void)consumer.install(bound, variant_id);
  check(consumer.run(5).progress == sharc::Progress{.instructions = 5, .base_cycles = 5} &&
            consumer_calls == 1 && consumer.registers().pe[0].data[0][3] == 0x100,
        "consumer-linked IR did not execute through bind");
  for (const auto position : {std::size_t{0}, artifact.size() / 2, artifact.size() - 1}) {
    auto changed = artifact;
    changed[position] ^= std::byte{1};
    fixture::throws<sharc::PreparationError>(
        [&] { (void)sharc::Core::load(program.code, {}, changed); }, "corrupt artifact accepted");
  }
  auto changed_code = program.code;
  changed_code.variants[0].spans[0].parcels.back() ^= 1;
  fixture::throws<sharc::PreparationError>(
      [&] { (void)sharc::Core::load(changed_code, {}, artifact); }, "cache accepted changed code");
  fixture::throws<sharc::PreparationError>(
      [&] {
        (void)sharc::Core::load(
            program.code, {.mode = sharc::ExecutionMode::fast_fp32, .callbacks = {}}, artifact);
      },
      "cache accepted a different numerical mode");
  core.restore(saved);
  const auto before_rebind = core.state();
  core.set_memory(program.memory());
  check(core.state() == before_rebind && core.run(1).fault.reason == sharc::FaultReason::unprepared,
        "memory rebinding did not revoke the native binding");
  core.restore(saved);
  (void)core.install(native, variant_id);
  const auto before_install = core.state();
  program.bytes[0] ^= std::byte{1};
  fixture::throws<sharc::PreparationError>([&] { (void)core.install(native, variant_id); },
                                           "install accepted changed physical code");
  check(core.state() == before_install, "failed install changed architectural state");
  check(core.run(1).fault.reason == sharc::FaultReason::unprepared,
        "failed code validation retained a stale native binding");
}

void connected_entries() {
  constexpr std::uint64_t absolute_jump = 0x063e00000000;
  constexpr std::uint32_t alternate_marker = 99;
  constexpr auto base = fixture::code_address;
  const std::array words{fixture::immediate(0, 3),
                         absolute_jump | (base + 4),
                         fixture::immediate(8, alternate_marker),
                         absolute_jump | (base + 7),
                         fixture::add(2, 0, 1),
                         fixture::immediate(3, 7),
                         absolute_jump | (base + 9),
                         fixture::immediate(9, 11),
                         std::uint64_t{0}};
  fixture::Program program(words);
  program.code.variants[0].entries = {base + 7, base, base + 2};
  const auto linked = consumer_roundtrip(program.code);
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  auto initial = initial_registers();
  initial.pe[0].data[0][1] = 2U << byte_shift;
  fixture::install(core, native, initial);
  const auto first = core.run(2);
  check(fixture::completed(first, 2, 5) && core.registers().pc == base + 4,
        "connected branch did not stop at its exact boundary");
  const auto saved = core.save();
  sharc::Core replay(program.memory());
  (void)replay.install(linked, fixture::Program::variant_id);
  replay.restore(saved);
  const auto resumed = replay.run_until(7);
  auto expected = initial;
  expected.pe[0].data[0][0] = 3U << byte_shift;
  expected.pe[0].data[0][2] = 5U << byte_shift;
  expected.pe[0].data[0][3] = 7U << byte_shift;
  fixture::boundary(expected, {.pc = base + 6, .clock = expected.emuclk + 7});
  check(fixture::completed(resumed, 2, 2) && replay.registers() == expected,
        "artifact-bound replay lost its connected region or full state");
  for (const auto entry : {base + 2, base + 7}) {
    auto registers = initial;
    registers.pc = entry;
    core.set_registers(registers);
    const bool first_path = entry == base + 2;
    const auto result = core.run(first_path ? 3 : 2);
    if (first_path) { registers.pe[0].data[0][8] = alternate_marker << byte_shift; }
    registers.pe[0].data[0][9] = 11U << byte_shift;
    fixture::boundary(
        registers,
        {.pc = first_path ? base + 8 : base + 9, .clock = registers.emuclk + (first_path ? 6 : 2)});
    check(fixture::completed(result, first_path ? 3U : 2U, first_path ? 6U : 2U) &&
              core.registers() == registers,
          "declared entry selected another region or changed unreachable state");
  }
  replay.restore(saved);
  auto registers = replay.registers();
  registers.pc = base + 2;
  replay.set_registers(registers);
  check(!replay.run(1).has_fault &&
            replay.registers().pe[0].data[0][8] == (alternate_marker << byte_shift),
        "owner PC replacement retained the previous region continuation");
}

void cached_call(bool indirect, sharc::ExecutionMode mode) {
  constexpr std::uint64_t direct_call = 0x06be00000000;
  constexpr std::uint64_t call_m8_i8 = 0x08be00000000;
  constexpr std::uint64_t rts = 0x0a3e00000000;
  constexpr std::uint32_t call_tag = 1U << 24;
  constexpr std::uint32_t empty = 1U << 22;
  const auto base = fixture::code_address;
  const std::array words{indirect ? call_m8_i8 : direct_call | (base + 2), std::uint64_t{0}, rts};
  fixture::Program program(words);
  if (indirect) { program.code.variants[0].entries.push_back(base + 2); }
  const sharc::CompileOptions options{.mode = mode, .callbacks = {}};
  const auto native = consumer_roundtrip(program.code, options);
  sharc::Core core(program.memory(), mode);
  auto expected = initial_registers();
  if (indirect) { expected.dag[0][8].index = base + 2; }
  fixture::install(core, native, expected);
  const auto result = core.run(3);
  expected.pc = base + 2;
  fixture::fetch_addresses(expected);
  expected.pc_stack[0] = (base + 1) | call_tag;
  expected.pe[0].stky = empty;
  expected.emuclk += 9;
  check(fixture::completed(result, 3, 9) && core.registers() == expected,
        indirect ? "cached consumer-bound indirect CALL lost its private return continuation"
                 : "cached consumer-bound CALL/RTS lost architectural stack or bound state");
}

void cached_memory() {
  constexpr std::uint64_t load = 0xa20000000000;
  constexpr std::uint32_t ram_address = 0x90000;
  constexpr std::uint32_t ram_id = 11;
  constexpr std::uint32_t broadcast = 1U << 23;
  constexpr std::uint32_t dag_alternate = 1U << 4;
  constexpr std::uint32_t data_alternate = 1U << 10;
  constexpr std::uint64_t raw = 0x1234567800;
  constexpr std::array initial_bytes{
      std::byte{0xee}, std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}};
  auto ram = initial_bytes;
  const std::array words{load};
  fixture::Program program(words);
  auto region = fixture::normal_region(ram_address, 1, ram_id);
  region.writable = false;
  region.byte_offset = 1;
  program.code.variants[0].layout.regions.push_back(region);
  auto memory = program.memory(ram_id, ram);
  const auto native = consumer_roundtrip(program.code);
  sharc::Core core(memory);
  auto expected = initial_registers();
  expected.mode1 = broadcast | dag_alternate | data_alternate;
  expected.dag[1][1].index = ram_address;
  fixture::install(core, native, expected);
  const auto result = core.run(1);
  expected.pe[0].data[1][0] = raw;
  expected.pe[1].data[1][0] = raw;
  fixture::boundary(expected, {.pc = expected.pc + 1, .clock = expected.emuclk + 1});
  check(fixture::completed(result, 1, 1) && core.registers() == expected && ram == initial_bytes,
        "cached consumer-bound memory lost storage, broadcast or bank context");
}

void cached_dag() {
  constexpr std::uint64_t modify_i7 = 0x160700000001;
  constexpr unsigned circular_bit = 24;
  constexpr unsigned bank_bit = 3;
  constexpr unsigned sticky_bit = 17;
  constexpr unsigned interrupt_bit = 20;
  constexpr std::uint32_t base = 0x2340;
  constexpr std::uint32_t length = 17;
  constexpr unsigned i7_code = 0x17;
  constexpr unsigned l7_code = 0x37;
  constexpr unsigned b7_code = 0x47;
  const std::array words{fixture::immediate(b7_code, base),
                         fixture::immediate(l7_code, length),
                         fixture::immediate(i7_code, base + length - 1),
                         modify_i7};
  fixture::Program program(words);
  const auto native = consumer_roundtrip(program.code);
  sharc::Core core(program.memory());
  auto expected = initial_registers();
  expected.mode1 = (1U << circular_bit) | (1U << bank_bit);
  expected.dag[1][7] = {};
  fixture::install(core, native, expected);
  const auto result = core.run(4);
  expected.dag[1][7] = {.index = base, .modifier = 0, .length = length, .base = base};
  expected.pe[0].stky |= 1U << sticky_bit;
  expected.irptl |= 1U << interrupt_bit;
  fixture::boundary(expected, {.pc = expected.pc + 4, .clock = expected.emuclk + 6});
  check(
      fixture::completed(result, 4, 6) && core.registers() == expected,
      "cached consumer-bound DAG load/update lost dependency cycles, mode, flags or bank metadata");
}

} // namespace

int main() {
  return fixture::run_test([] {
    lifecycle();
    installation_callbacks();
    connected_entries();
    cached_call(false, sharc::ExecutionMode::strict);
    cached_call(true, sharc::ExecutionMode::strict);
    cached_call(true, sharc::ExecutionMode::fast_fp32);
    cached_memory();
    cached_dag();
    cached_loop_handoff(true);
    cached_loop_handoff(false);
  });
}
