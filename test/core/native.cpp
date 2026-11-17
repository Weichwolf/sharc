// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "sharc/memory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <memory>
#include <sharc/core.hpp>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using fixture::check;
constexpr std::uint32_t variant_id = 7;
constexpr std::uint32_t storage_id = 9;
constexpr std::uint32_t instruction_bytes = 6;
constexpr unsigned byte_shift = 8;
constexpr std::uint64_t preserved_raw = 0x81234567ef;
constexpr std::uint64_t negative_one_raw = 0xffffffff00;
constexpr std::uint64_t initial_clock = 123;

struct Program {
  sharc::CodeImage image;
  std::vector<std::byte> bytes;

  Program() {
    const std::array words{fixture::immediate(0, 0xffffffff),
                           fixture::immediate(1, 1),
                           fixture::add(2, 0, 1),
                           fixture::add(3, 2, 1),
                           std::uint64_t{0}};
    sharc::CodeVariant variant;
    variant.id = variant_id;
    variant.entries = {fixture::code_address};
    sharc::CodeSpan span{.address = fixture::code_address,
                         .instruction_set = sharc::InstructionSet::isa,
                         .parcels = {}};
    for (const auto word : words) {
      fixture::append(span, word);
      for (unsigned byte = 0; byte < instruction_bytes; ++byte) {
        bytes.push_back(static_cast<std::byte>(word >> (byte * byte_shift)));
      }
    }
    variant.spans.push_back(std::move(span));
    variant.layout.regions.push_back({.space = sharc::MemorySpace::program,
                                      .view = sharc::MemoryView::instruction,
                                      .first = fixture::code_address,
                                      .count = words.size(),
                                      .storage_id = storage_id,
                                      .byte_offset = 0,
                                      .byte_stride = instruction_bytes,
                                      .readable = true,
                                      .writable = false,
                                      .initialized = true});
    image.variants.push_back(std::move(variant));
  }

  sharc::Memory memory() {
    return {.regions = {{.storage_id = storage_id, .bytes = bytes, .generation = 1}}, .io = {}};
  }
};

sharc::Registers initial_registers() {
  sharc::Registers result;
  result.pc = fixture::code_address;
  result.emuclk = initial_clock;
  result.pe[1].data[0].fill(preserved_raw);
  result.pe[0].data[1].fill(preserved_raw);
  result.pe[1].data[1].fill(preserved_raw);
  return result;
}

std::uint64_t consumer_calls{};

void consumer_probe() noexcept {
  ++consumer_calls;
}

void llvm_check(llvm::Error error) {
  if (error) { throw std::runtime_error(llvm::toString(std::move(error))); }
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
  Program program;
  program.image.variants[0].layout.regions.clear();
  const auto native = sharc::Core::compile(program.image);
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
  check(!llvm::InitializeNativeTarget() && !llvm::InitializeNativeTargetAsmPrinter(),
        "consumer could not initialize LLVM");
  auto created = llvm::orc::LLJITBuilder().create();
  if (!created) { throw std::runtime_error(llvm::toString(created.takeError())); }
  auto engine = std::shared_ptr<llvm::orc::LLJIT>(std::move(*created));
  auto context = std::make_unique<llvm::LLVMContext>();
  const auto bytes = image.bitcode();
  auto parsed = llvm::parseBitcodeFile(
      {llvm::StringRef(reinterpret_cast<const char *>(bytes.data()), bytes.size()), "consumer"},
      *context);
  if (!parsed) { throw std::runtime_error(llvm::toString(parsed.takeError())); }
  auto module = std::move(*parsed);
  module->setDataLayout(engine->getDataLayout());
  module->setTargetTriple(engine->getTargetTriple().str());
  auto *probe =
      llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(*context), false),
                             llvm::GlobalValue::ExternalLinkage,
                             "consumer_probe",
                             *module);
  for (const auto &name : image.exports()) {
    auto *guest = module->getFunction(name);
    check(guest != nullptr, "consumer did not find semantic export");
    guest->setName(name + "_guest");
    auto *wrapper = llvm::Function::Create(
        guest->getFunctionType(), llvm::GlobalValue::ExternalLinkage, name, *module);
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", wrapper));
    builder.CreateCall(probe);
    std::vector<llvm::Value *> arguments;
    for (auto &argument : wrapper->args()) { arguments.push_back(&argument); }
    builder.CreateCall(guest, arguments);
    builder.CreateRetVoid();
  }
  llvm::orc::SymbolMap external;
  external[engine->mangleAndIntern("consumer_probe")] = {
      llvm::orc::ExecutorAddr::fromPtr(&consumer_probe), llvm::JITSymbolFlags::Exported};
  llvm_check(engine->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(external))));
  llvm_check(
      engine->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));
  sharc::NativeSymbols symbols;
  symbols.lifetime = engine;
  for (const auto &name : image.exports()) {
    auto address = engine->lookup(name);
    if (!address) { throw std::runtime_error(llvm::toString(address.takeError())); }
    symbols.entries.push_back({name, static_cast<std::uintptr_t>(address->getValue())});
  }
  return symbols;
}

void lifecycle() {
  Program program;
  const auto semantic = sharc::Core::lower(program.image);
  check(!semantic.bitcode().empty() && !semantic.exports().empty(),
        "lower returned an empty image");
  const auto artifact = semantic.artifact();
  const auto loaded = sharc::Core::load(program.image, {}, artifact);
  check(loaded.identity() == semantic.identity() &&
            std::ranges::equal(loaded.bitcode(), semantic.bitcode()) &&
            std::ranges::equal(loaded.exports(), semantic.exports()),
        "artifact round trip changed image");
  fixture::throws<sharc::PreparationError>([&] { (void)sharc::Core::bind(loaded, {}); },
                                           "bind accepted missing native symbols");
  const auto native = sharc::Core::compile(program.image);
  check(native.report().native_bytes != 0 && native.report().seconds > 0,
        "stand-alone preparation did not measure native code/time");
  sharc::Core core(program.memory());
  core.set_registers(initial_registers());
  const auto original = core.state();
  (void)core.install(native, variant_id);
  check(core.state() == original, "installation changed state or time");
  const auto prefix = core.run(2);
  check(!prefix.has_fault &&
            prefix.progress == sharc::Progress{.instructions = 2, .base_cycles = 2},
        "immediate prefix failed");
  check(core.registers().pe[0].data[0][0] == negative_one_raw &&
            core.registers().pe[0].data[0][1] == 0x100,
        "immediate moves have wrong raw views");
  const auto compute = core.run(1);
  check(!compute.has_fault &&
            compute.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            core.registers().pe[0].data[0][2] == 0 && core.registers().pe[0].astat == 9,
        "native ADD lost result, flags or precise prefix");
  const auto saved = core.save();
  const auto saved_state = core.state();
  const auto finish = core.run_until(5);
  check(!finish.has_fault &&
            finish.progress == sharc::Progress{.instructions = 2, .base_cycles = 2} &&
            core.registers().pe[0].data[0][3] == 0x100 && core.registers().pe[0].astat == 0,
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
        [&] { (void)sharc::Core::load(program.image, {}, changed); }, "corrupt artifact accepted");
  }
  auto changed_code = program.image;
  changed_code.variants[0].spans[0].parcels.back() ^= 1;
  fixture::throws<sharc::PreparationError>(
      [&] { (void)sharc::Core::load(changed_code, {}, artifact); }, "cache accepted changed code");
  fixture::throws<sharc::PreparationError>(
      [&] {
        (void)sharc::Core::load(
            program.image, {.mode = sharc::ExecutionMode::fast_fp32, .callbacks = {}}, artifact);
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
  const auto semantic = sharc::Core::lower(program.code);
  const auto loaded = sharc::Core::load(program.code, {}, semantic.artifact());
  const auto linked = sharc::Core::bind(loaded, consumer_compile(loaded));
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(program.memory());
  auto initial = initial_registers();
  initial.pe[0].data[0][1] = 2U << byte_shift;
  core.set_registers(initial);
  (void)core.install(native, fixture::Program::variant_id);
  const auto first = core.run(2);
  check(!first.has_fault &&
            first.progress == sharc::Progress{.instructions = 2, .base_cycles = 5} &&
            core.registers().pc == base + 4,
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
  expected.pc = base + 6;
  expected.faddr = expected.pc + 4;
  expected.daddr = expected.pc + 2;
  expected.emuclk += 7;
  check(!resumed.has_fault &&
            resumed.progress == sharc::Progress{.instructions = 2, .base_cycles = 2} &&
            replay.registers() == expected,
        "artifact-bound replay lost its connected region or full state");
  for (const auto entry : {base + 2, base + 7}) {
    auto registers = initial;
    registers.pc = entry;
    core.set_registers(registers);
    const bool first_path = entry == base + 2;
    const auto result = core.run(first_path ? 3 : 2);
    if (first_path) { registers.pe[0].data[0][8] = alternate_marker << byte_shift; }
    registers.pe[0].data[0][9] = 11U << byte_shift;
    registers.pc = first_path ? base + 8 : base + 9;
    registers.faddr = registers.pc + 4;
    registers.daddr = registers.pc + 2;
    registers.emuclk += first_path ? 6 : 2;
    check(!result.has_fault &&
              result.progress == sharc::Progress{.instructions = first_path ? 3U : 2U,
                                                 .base_cycles = first_path ? 6U : 2U} &&
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

void cached_call() {
  constexpr std::uint64_t call = 0x06be00000000;
  constexpr std::uint64_t rts = 0x0a3e00000000;
  constexpr std::uint32_t call_tag = 1U << 24;
  constexpr std::uint32_t empty = 1U << 22;
  const auto base = fixture::code_address;
  const std::array words{call | (base + 2), std::uint64_t{0}, rts};
  fixture::Program program(words);
  const auto semantic = sharc::Core::lower(program.code);
  const auto loaded = sharc::Core::load(program.code, {}, semantic.artifact());
  const auto native = sharc::Core::bind(loaded, consumer_compile(loaded));
  sharc::Core core(program.memory());
  auto expected = initial_registers();
  core.set_registers(expected);
  (void)core.install(native, fixture::Program::variant_id);
  const auto result = core.run(3);
  expected.pc = base + 2;
  expected.faddr = expected.pc + 4;
  expected.daddr = expected.pc + 2;
  expected.pc_stack[0] = (base + 1) | call_tag;
  expected.pe[0].stky = empty;
  expected.emuclk += 9;
  check(!result.has_fault &&
            result.progress == sharc::Progress{.instructions = 3, .base_cycles = 9} &&
            core.registers() == expected,
        "cached consumer-bound CALL/RTS lost architectural stack or bound state");
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
  program.code.variants[0].layout.regions.push_back({.space = sharc::MemorySpace::data,
                                                     .view = sharc::MemoryView::normal_word,
                                                     .first = ram_address,
                                                     .count = 1,
                                                     .storage_id = ram_id,
                                                     .byte_offset = 1,
                                                     .byte_stride = 4,
                                                     .readable = true,
                                                     .writable = false,
                                                     .initialized = true});
  auto memory = program.memory();
  memory.regions.push_back({.storage_id = ram_id, .bytes = ram, .generation = 1});
  const auto semantic = sharc::Core::lower(program.code);
  const auto loaded = sharc::Core::load(program.code, {}, semantic.artifact());
  const auto native = sharc::Core::bind(loaded, consumer_compile(loaded));
  sharc::Core core(memory);
  auto expected = initial_registers();
  expected.mode1 = broadcast | dag_alternate | data_alternate;
  expected.dag[1][1].index = ram_address;
  core.set_registers(expected);
  (void)core.install(native, fixture::Program::variant_id);
  const auto result = core.run(1);
  expected.pe[0].data[1][0] = raw;
  expected.pe[1].data[1][0] = raw;
  ++expected.pc;
  expected.faddr = expected.pc + 4;
  expected.daddr = expected.pc + 2;
  ++expected.emuclk;
  check(!result.has_fault &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
            core.registers() == expected && ram == initial_bytes,
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
  const std::array words{modify_i7};
  fixture::Program program(words);
  const auto semantic = sharc::Core::lower(program.code);
  const auto loaded = sharc::Core::load(program.code, {}, semantic.artifact());
  const auto native = sharc::Core::bind(loaded, consumer_compile(loaded));
  sharc::Core core(program.memory());
  auto expected = initial_registers();
  expected.mode1 = (1U << circular_bit) | (1U << bank_bit);
  expected.dag[1][7] = {.index = base + length - 1, .modifier = 0, .length = length, .base = base};
  core.set_registers(expected);
  (void)core.install(native, fixture::Program::variant_id);
  const auto result = core.run(1);
  expected.dag[1][7].index = base;
  expected.pe[0].stky |= 1U << sticky_bit;
  expected.irptl |= 1U << interrupt_bit;
  ++expected.pc;
  expected.faddr = expected.pc + 4;
  expected.daddr = expected.pc + 2;
  ++expected.emuclk;
  check(!result.has_fault && core.registers() == expected &&
            result.progress == sharc::Progress{.instructions = 1, .base_cycles = 1},
        "cached consumer-bound DAG update lost MODE1, flags or register-bank metadata");
}

} // namespace

int main() try {
  lifecycle();
  installation_callbacks();
  connected_entries();
  cached_call();
  cached_memory();
  cached_dag();
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
