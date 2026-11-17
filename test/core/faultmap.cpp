// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Layer.h>
#include <llvm/ExecutionEngine/Orc/ObjectTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/RuntimeDyld.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>
#include <memory>
#include <sharc/core.hpp>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using fixture::check;
constexpr std::uint32_t last_pc = 0xffffff;
constexpr std::uint64_t excessive_variant = std::uint64_t{1} << 32;

struct Provider {
  std::vector<sharc::NativeSymbols::TextRange> ranges;
  std::vector<sharc::NativeSymbols::Site> sites;
  std::vector<char> text;

  void loaded(const llvm::object::ObjectFile &object,
              const llvm::RuntimeDyld::LoadedObjectInfo &loaded) {
    auto debug = llvm::DWARFContext::create(
        object, llvm::DWARFContext::ProcessDebugRelocations::Process, &loaded);
    for (const auto &section : object.sections()) {
      if (!section.isText() || section.getSize() == 0) { continue; }
      const auto address = loaded.getSectionLoadAddress(section);
      ranges.push_back({.address = address, .size = section.getSize()});
      // Independent provider: query every native byte instead of parsing row intervals.
      for (std::uint64_t offset = 0; offset < section.getSize(); ++offset) {
        const auto line = debug->getLineInfoForAddress(
            {.Address = address + offset, .SectionIndex = section.getIndex()});
        llvm::StringRef file(line.FileName);
        std::uint64_t variant{};
        if (line.Line == 0 || !file.consume_front("sharc-variant-") ||
            file.getAsInteger(10, variant)) {
          continue;
        }
        if (!sites.empty() && sites.back().address + sites.back().size == address + offset &&
            sites.back().variant == variant && sites.back().guest_pc == line.Line - 1) {
          ++sites.back().size;
        } else {
          sites.push_back({.address = address + offset,
                           .size = 1,
                           .variant = variant,
                           .guest_pc = line.Line - 1});
        }
      }
    }
  }
};

void optimize(llvm::Module &module) {
  auto target = llvm::orc::JITTargetMachineBuilder::detectHost();
  if (!target) { throw std::runtime_error(llvm::toString(target.takeError())); }
  auto machine = target->createTargetMachine();
  if (!machine) { throw std::runtime_error(llvm::toString(machine.takeError())); }
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager calls;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder passes(machine->get());
  passes.registerModuleAnalyses(modules);
  passes.registerCGSCCAnalyses(calls);
  passes.registerFunctionAnalyses(functions);
  passes.registerLoopAnalyses(loops);
  passes.crossRegisterProxies(loops, functions, calls, modules);
  passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3).run(module, modules);
}

sharc::NativeSymbols
link(const sharc::SemanticImage &image, const std::shared_ptr<Provider> &provider, bool strip) {
  auto target = llvm::orc::JITTargetMachineBuilder::detectHost();
  if (!target) { throw std::runtime_error(llvm::toString(target.takeError())); }
  target->setCodeGenOptLevel(llvm::CodeGenOptLevel::Aggressive);
  auto created =
      llvm::orc::LLJITBuilder()
          .setJITTargetMachineBuilder(std::move(*target))
          .setObjectLinkingLayerCreator(
              [provider](llvm::orc::ExecutionSession &session, const llvm::Triple &)
                  -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
                auto layer = std::make_unique<llvm::orc::RTDyldObjectLinkingLayer>(
                    session, [] { return std::make_unique<llvm::SectionMemoryManager>(); });
                layer->setNotifyLoaded(
                    [provider](llvm::orc::MaterializationResponsibility &,
                               const llvm::object::ObjectFile &object,
                               const llvm::RuntimeDyld::LoadedObjectInfo &loaded) {
                      provider->loaded(object, loaded);
                    });
                return layer;
              })
          .create();
  if (!created) { throw std::runtime_error(llvm::toString(created.takeError())); }
  auto engine = std::shared_ptr<llvm::orc::LLJIT>(std::move(*created));
  engine->getObjTransformLayer().setTransform(
      [provider](std::unique_ptr<llvm::MemoryBuffer> buffer)
          -> llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> {
        auto object = llvm::object::ObjectFile::createObjectFile(buffer->getMemBufferRef());
        if (!object) { return object.takeError(); }
        for (const auto &section : (*object)->sections()) {
          if (!section.isText()) { continue; }
          auto bytes = section.getContents();
          if (!bytes) { return bytes.takeError(); }
          provider->text.insert(provider->text.end(), bytes->begin(), bytes->end());
        }
        return buffer;
      });
  auto context = std::make_unique<llvm::LLVMContext>();
  const auto bytes = image.bitcode();
  auto parsed = llvm::parseBitcodeFile(
      {llvm::StringRef(reinterpret_cast<const char *>(bytes.data()), bytes.size()), "consumer"},
      *context);
  if (!parsed) { throw std::runtime_error(llvm::toString(parsed.takeError())); }
  auto module = std::move(*parsed);
  module->setDataLayout(engine->getDataLayout());
  module->setTargetTriple(engine->getTargetTriple().str());
  if (strip) { llvm::StripDebugInfo(*module); }
  optimize(*module);
  if (auto error =
          engine->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context)))) {
    throw std::runtime_error(llvm::toString(std::move(error)));
  }
  sharc::NativeSymbols symbols;
  symbols.lifetime = engine;
  for (const auto &name : image.exports()) {
    auto address = engine->lookup(name);
    if (!address) { throw std::runtime_error(llvm::toString(address.takeError())); }
    symbols.entries.push_back({name, static_cast<std::uintptr_t>(address->getValue())});
  }
  symbols.fault_map = {.text_ranges = provider->ranges, .sites = provider->sites};
  return symbols;
}

void invalid_maps(const sharc::SemanticImage &image, const sharc::NativeSymbols &valid) {
  const auto refuse = [&](std::vector<sharc::NativeSymbols::TextRange> ranges,
                          std::vector<sharc::NativeSymbols::Site> sites) {
    auto changed = valid;
    changed.fault_map = {.text_ranges = ranges, .sites = sites};
    fixture::throws<sharc::PreparationError>([&] { (void)sharc::Core::bind(image, changed); },
                                             "invalid native fault map accepted");
  };
  const std::vector<sharc::NativeSymbols::TextRange> ranges(valid.fault_map.text_ranges.begin(),
                                                            valid.fault_map.text_ranges.end());
  const std::vector<sharc::NativeSymbols::Site> sites(valid.fault_map.sites.begin(),
                                                      valid.fault_map.sites.end());
  for (const auto address : {std::uintptr_t{0}, std::numeric_limits<std::uintptr_t>::max()}) {
    auto changed = ranges;
    changed[0].address = address;
    refuse(std::move(changed), sites);
  }
  auto changed_ranges = ranges;
  changed_ranges[0].size = 0;
  refuse(changed_ranges, sites);
  changed_ranges = ranges;
  changed_ranges.push_back(ranges[0]);
  refuse(changed_ranges, sites);
  refuse({}, sites);
  for (const auto variant : {excessive_variant, excessive_variant + sites[0].variant}) {
    auto changed = sites;
    changed[0].variant = variant;
    refuse(ranges, std::move(changed));
  }
  auto changed = sites;
  changed[0].guest_pc = last_pc - 1;
  refuse(ranges, changed);
  changed = sites;
  changed[0].size = 0;
  refuse(ranges, changed);
  changed = sites;
  changed[0].address = std::numeric_limits<std::uintptr_t>::max();
  refuse(ranges, changed);
  changed = sites;
  changed.push_back(sites[0]);
  refuse(ranges, changed);
  std::vector<sharc::NativeSymbols::TextRange> split;
  for (const auto &range : ranges) {
    const auto half = range.size / 2;
    if (half != 0) { split.push_back({.address = range.address, .size = half}); }
    split.push_back({.address = range.address + half, .size = range.size - half});
  }
  std::ranges::reverse(split);
  auto independent = valid;
  independent.fault_map = {.text_ranges = split, .sites = sites};
  (void)sharc::Core::bind(image, independent);
}

void qualify(sharc::ExecutionMode mode) {
  constexpr std::uint32_t result = 0x12345678;
  const std::array words{fixture::immediate(0, result)};
  fixture::Program program(words, 0);
  auto high = program.code.variants[0];
  high.id = std::numeric_limits<std::uint32_t>::max();
  high.spans[0].address = last_pc;
  high.entries = {last_pc};
  high.layout.regions[0].first = last_pc;
  program.code.variants.push_back(std::move(high));
  const auto semantic = sharc::Core::lower(program.code, {.mode = mode, .callbacks = {}});
  const auto image =
      sharc::Core::load(program.code, {.mode = mode, .callbacks = {}}, semantic.artifact());
  auto provider = std::make_shared<Provider>();
  auto symbols = link(image, provider, false);
  check(!provider->ranges.empty() && !provider->sites.empty(), "missing linked fault attribution");
  for (const auto &[variant, pc] :
       {std::pair{fixture::Program::variant_id, 0U},
        std::pair{std::numeric_limits<std::uint32_t>::max(), last_pc}}) {
    check(std::ranges::any_of(
              provider->sites,
              [&](const auto &site) { return site.variant == variant && site.guest_pc == pc; }),
          "guest PC zero, maximum PC or variant lost in linked line table");
  }
  const auto stripped_provider = std::make_shared<Provider>();
  const auto stripped_symbols = link(image, stripped_provider, true);
  check(!provider->text.empty() && provider->text == stripped_provider->text &&
            stripped_provider->sites.empty(),
        "guest debug locations changed native executable bytes");
  invalid_maps(image, symbols);
  const auto native = sharc::Core::bind(image, symbols);
  symbols.fault_map = {};
  provider->ranges.clear();
  provider->sites.clear();
  provider.reset();
  for (const auto variant :
       {fixture::Program::variant_id, std::numeric_limits<std::uint32_t>::max()}) {
    sharc::Core core(program.memory(), mode);
    sharc::Registers registers;
    registers.pc = variant == fixture::Program::variant_id ? 0 : last_pc;
    core.set_registers(registers);
    (void)core.install(native, variant);
    const auto run = core.run(1);
    check(!run.has_fault && run.progress == sharc::Progress{.instructions = 1, .base_cycles = 1} &&
              core.registers().pe[0].data[0][0] == std::uint64_t{result} << 8,
          "bound image lost code lifetime after releasing fault-map input buffers");
  }
}
} // namespace

int main() try {
  check(!llvm::InitializeNativeTarget() && !llvm::InitializeNativeTargetAsmPrinter(),
        "LLVM unavailable");
  qualify(sharc::ExecutionMode::strict);
  qualify(sharc::ExecutionMode::fast_fp32);
  std::cout << "native fault-map binding passed\n";
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
