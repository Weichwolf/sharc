// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "check.hpp"

#include <cstdint>
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
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
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
#include <stdexcept>
#include <utility>
#include <vector>

namespace fixture {
struct ConsumerEffects {
  void (*probe)() noexcept {};
  std::uintptr_t fault_address{};
  bool wrap_exports{};
  void (*link_ir)(llvm::Module &){};
};

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

inline void optimize(llvm::Module &module) {
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

inline void inject_effects(llvm::Module &module,
                           llvm::orc::LLJIT &engine,
                           const sharc::SemanticImage &image,
                           ConsumerEffects effects) {
  llvm::Function *probe{};
  if (effects.probe != nullptr) {
    probe = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(module.getContext()), false),
        llvm::GlobalValue::ExternalLinkage,
        "consumer_probe",
        module);
    llvm::orc::SymbolMap external;
    external[engine.mangleAndIntern("consumer_probe")] = {
        llvm::orc::ExecutorAddr::fromPtr(effects.probe), llvm::JITSymbolFlags::Exported};
    if (auto error =
            engine.getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(external)))) {
      throw std::runtime_error(llvm::toString(std::move(error)));
    }
  }
  for (const auto &name : image.exports()) {
    auto *function = module.getFunction(name);
    check(function != nullptr, "consumer did not find semantic export");
    if (effects.wrap_exports) {
      function->setName(name + "_guest");
      auto *wrapper = llvm::Function::Create(
          function->getFunctionType(), llvm::GlobalValue::ExternalLinkage, name, module);
      llvm::IRBuilder<> body(llvm::BasicBlock::Create(module.getContext(), "entry", wrapper));
      std::vector<llvm::Value *> arguments;
      for (auto &argument : wrapper->args()) { arguments.push_back(&argument); }
      body.CreateCall(function, arguments);
      body.CreateRetVoid();
      function = wrapper;
    }
    llvm::IRBuilder<> builder(&*function->getEntryBlock().getFirstInsertionPt());
    if (auto *scope = function->getSubprogram()) {
      builder.SetCurrentDebugLocation(llvm::DILocation::get(module.getContext(), 0, 0, scope));
    }
    if (probe != nullptr) { builder.CreateCall(probe); }
    if (effects.fault_address != 0) {
      auto *pointer =
          builder.CreateIntToPtr(builder.getInt64(effects.fault_address), builder.getPtrTy());
      builder.CreateLoad(builder.getInt32Ty(), pointer)->setVolatile(true);
    }
  }
}

inline sharc::NativeSymbols link(const sharc::SemanticImage &image,
                                 const std::shared_ptr<Provider> &provider,
                                 bool strip,
                                 ConsumerEffects effects = {}) {
  static const bool initialized =
      !llvm::InitializeNativeTarget() && !llvm::InitializeNativeTargetAsmPrinter();
  check(initialized, "LLVM unavailable");
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
  if (effects.probe != nullptr || effects.fault_address != 0 || effects.wrap_exports) {
    inject_effects(*module, *engine, image, effects);
  }
  if (effects.link_ir != nullptr) { effects.link_ir(*module); }
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

} // namespace fixture
