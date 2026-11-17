// SPDX-License-Identifier: GPL-3.0-only
#include "decode.hpp"
#include "native.hpp"
#include "sharc/core.hpp"
#include "sharc/memory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/DebugInfo/DIContext.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/DebugInfo/DWARF/DWARFDebugLine.h>
#include <llvm/DebugInfo/DWARF/DWARFUnit.h>
#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/DebugObjectManagerPlugin.h>
#include <llvm/ExecutionEngine/Orc/EPCDebugObjectRegistrar.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/ExecutionEngine/Orc/Shared/MemoryFlags.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/RuntimeDyld.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace sharc::detail {
namespace {
constexpr std::string_view artifact_magic = "sharc-llvm-image-v3";
constexpr unsigned byte_bits = 8;
constexpr unsigned word_bytes = 6;

std::array<std::uint8_t, 32> checksum(std::span<const std::byte> bytes) {
  return llvm::SHA256::hash({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
}

class Writer {
public:
  void number(std::uint64_t value) {
    for (unsigned i = 0; i < sizeof(value); ++i) {
      bytes.push_back(static_cast<std::byte>(value));
      value >>= byte_bits;
    }
  }

  void data(std::span<const std::byte> value) {
    number(value.size());
    bytes.insert(bytes.end(), value.begin(), value.end());
  }

  void text(std::string_view value) { data(std::as_bytes(std::span(value.data(), value.size()))); }

  std::vector<std::byte> bytes;
};

class Reader {
public:
  Reader(std::span<const std::byte> input, std::string_view identity)
      : bytes_(input), identity_(identity) {}

  [[noreturn]] void invalid(std::string message) const {
    refuse(FaultReason::invalid_image, identity_, 0, 0, std::move(message));
  }

  std::uint64_t number() {
    constexpr auto size = sizeof(std::uint64_t);
    if (bytes_.size() < size) { invalid("truncated artifact number"); }
    std::uint64_t value{};
    for (unsigned i = 0; i < size; ++i) {
      value |= std::uint64_t{std::to_integer<std::uint8_t>(bytes_[i])} << (i * byte_bits);
    }
    bytes_ = bytes_.subspan(size);
    return value;
  }

  std::uint32_t word() {
    const auto value = number();
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      invalid("artifact word exceeds 32 bits");
    }
    return static_cast<std::uint32_t>(value);
  }

  std::span<const std::byte> data() {
    const auto count = number();
    if (count > bytes_.size()) { invalid("truncated artifact data"); }
    const auto result = bytes_.first(static_cast<std::size_t>(count));
    bytes_ = bytes_.subspan(result.size());
    return result;
  }

  std::string text() {
    const auto value = data();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
  }

  [[nodiscard]] std::size_t remaining() const { return bytes_.size(); }

private:
  std::span<const std::byte> bytes_;
  std::string_view identity_;
};

llvm::MemoryBufferRef bitcode_buffer(const PreparedImage &image) {
  return {
      llvm::StringRef(reinterpret_cast<const char *>(image.bitcode.data()), image.bitcode.size()),
      image.identity};
}

std::unique_ptr<llvm::Module> parse_module(const PreparedImage &image, llvm::LLVMContext &context) {
  auto parsed = llvm::parseBitcodeFile(bitcode_buffer(image), context);
  if (!parsed) {
    refuse(FaultReason::invalid_image,
           image.identity,
           0,
           0,
           "invalid LLVM bitcode: " + llvm::toString(parsed.takeError()));
  }
  std::string error;
  llvm::raw_string_ostream output(error);
  if (llvm::verifyModule(**parsed, &output)) {
    refuse(FaultReason::invalid_image, image.identity, 0, 0, "invalid LLVM module: " + error);
  }
  for (const auto &name : image.exports) {
    const auto *function = (*parsed)->getFunction(name);
    if (function == nullptr || function->isDeclaration() || function->arg_size() != 2 ||
        !function->getReturnType()->isVoidTy() || !function->getArg(0)->getType()->isPointerTy() ||
        !function->getArg(1)->getType()->isPointerTy()) {
      refuse(FaultReason::invalid_image,
             image.identity,
             0,
             0,
             "missing or incompatible export " + name);
    }
  }
  return std::move(*parsed);
}

[[noreturn]] void compiler_error(const PreparedImage &image, llvm::Error error) {
  refuse(FaultReason::unsupported_feature,
         image.identity,
         0,
         0,
         "LLVM preparation failed: " + llvm::toString(std::move(error)));
}

struct LinkedMap {
  std::vector<NativeSymbols::TextRange> text_ranges;
  std::vector<NativeSymbols::Site> sites;
  std::size_t bytes{};
  std::string error;

  void loaded(const llvm::object::ObjectFile &object,
              const llvm::RuntimeDyld::LoadedObjectInfo &addresses) {
    for (const auto &section : object.sections()) {
      if (!section.isText() || section.getSize() == 0) { continue; }
      text_ranges.push_back(
          {.address = addresses.getSectionLoadAddress(section), .size = section.getSize()});
      bytes += static_cast<std::size_t>(section.getSize());
    }
    lines(object, &addresses);
  }

  void lines(const llvm::object::ObjectFile &object, const llvm::LoadedObjectInfo *addresses) {
    const auto diagnostic = [&](llvm::Error failure) {
      error += llvm::toString(std::move(failure));
    };
    auto debug = llvm::DWARFContext::create(object,
                                            llvm::DWARFContext::ProcessDebugRelocations::Process,
                                            addresses,
                                            "",
                                            diagnostic,
                                            diagnostic);
    for (const auto &unit : debug->compile_units()) {
      const auto *lines = debug->getLineTableForUnit(unit.get());
      if (lines == nullptr) { continue; }
      for (std::size_t i = 0; i + 1 < lines->Rows.size(); ++i) {
        const auto &row = lines->Rows[i];
        const auto &next = lines->Rows[i + 1];
        if (row.EndSequence || row.Line == 0 ||
            row.Address.SectionIndex != next.Address.SectionIndex ||
            row.Address.Address >= next.Address.Address) {
          continue;
        }
        std::string filename;
        if (!lines->getFileNameByIndex(
                row.File, "", llvm::DILineInfoSpecifier::FileLineInfoKind::RawValue, filename)) {
          continue;
        }
        llvm::StringRef name(filename);
        std::uint64_t variant{};
        if (!name.consume_front("sharc-variant-") || name.getAsInteger(10, variant)) { continue; }
        sites.push_back({.address = row.Address.Address,
                         .size = next.Address.Address - row.Address.Address,
                         .variant = variant,
                         .guest_pc = row.Line - 1});
      }
    }
  }
};

class LineRegistrar final : public llvm::orc::DebugObjectRegistrar {
public:
  explicit LineRegistrar(std::shared_ptr<LinkedMap> map) : map_(std::move(map)) {}

  llvm::Error registerDebugObject(llvm::orc::ExecutorAddrRange memory,
                                  bool /*auto_register_code*/) override {
    auto object = llvm::object::ObjectFile::createObjectFile(
        {llvm::StringRef(memory.Start.toPtr<const char *>(), memory.size()), "sharc.debug"});
    if (!object) { return object.takeError(); }
    map_->lines(**object, nullptr);
    return llvm::Error::success();
  }

private:
  std::shared_ptr<LinkedMap> map_;
};

class LinkMapPlugin final : public llvm::orc::DebugObjectManagerPlugin {
public:
  LinkMapPlugin(llvm::orc::ExecutionSession &session, std::shared_ptr<LinkedMap> map)
      : llvm::orc::DebugObjectManagerPlugin(
            session, std::make_unique<LineRegistrar>(map), true, false),
        map_(std::move(map)) {}

  void modifyPassConfig(llvm::orc::MaterializationResponsibility &responsibility,
                        llvm::jitlink::LinkGraph &graph,
                        llvm::jitlink::PassConfiguration &passes) override {
    llvm::orc::DebugObjectManagerPlugin::modifyPassConfig(responsibility, graph, passes);
    passes.PostAllocationPasses.emplace_back([map = map_](llvm::jitlink::LinkGraph &linked) {
      for (const auto &section : linked.sections()) {
        if ((static_cast<unsigned>(section.getMemProt()) &
             static_cast<unsigned>(llvm::orc::MemProt::Exec)) == 0) {
          continue;
        }
        const llvm::jitlink::SectionRange range(section);
        if (range.getSize() == 0) { continue; }
        map->text_ranges.push_back(
            {.address = range.getStart().getValue(), .size = range.getSize()});
        map->bytes += range.getSize();
      }
      return llvm::Error::success();
    });
  }

private:
  std::shared_ptr<LinkedMap> map_;
};

std::uint64_t code_word(const CodeVariant &variant,
                        const Memory &memory,
                        std::uint32_t address,
                        const PreparedImage &image) {
  for (const auto &layout : variant.layout.regions) {
    if (layout.space != MemorySpace::program || layout.view != MemoryView::instruction ||
        address < layout.first || std::uint64_t{address} - layout.first >= layout.count) {
      continue;
    }
    if (!layout.readable) {
      refuse(
          FaultReason::invalid_image, image.identity, variant.id, address, "code is not readable");
    }
    if (!layout.initialized) { break; }
    const auto region =
        std::ranges::find(memory.regions, layout.storage_id, &MemoryRegion::storage_id);
    if (region == memory.regions.end() || layout.byte_stride < word_bytes) {
      refuse(FaultReason::invalid_image,
             image.identity,
             variant.id,
             address,
             "instruction storage is missing or its stride is too small");
    }
    const auto size = region->bytes.size();
    const auto index = std::uint64_t{address} - layout.first;
    if (layout.byte_offset > size || index > (size - layout.byte_offset) / layout.byte_stride) {
      refuse(FaultReason::invalid_image,
             image.identity,
             variant.id,
             address,
             "code storage is too short");
    }
    const auto offset = static_cast<std::size_t>(layout.byte_offset + (index * layout.byte_stride));
    if (size - offset < word_bytes) {
      refuse(FaultReason::invalid_image,
             image.identity,
             variant.id,
             address,
             "code word is truncated");
    }
    std::uint64_t value{};
    for (unsigned byte = 0; byte < word_bytes; ++byte) {
      value |= std::uint64_t{std::to_integer<std::uint8_t>(region->bytes[offset + byte])}
               << (byte * byte_bits);
    }
    return value;
  }
  std::uint64_t value{};
  if (memory.io.read != nullptr &&
      memory.io.read(
          memory.io.owner, MemorySpace::program, MemoryView::instruction, address, value)) {
    return value;
  }
  refuse(FaultReason::invalid_image,
         image.identity,
         variant.id,
         address,
         "actual instruction storage cannot be validated");
}
} // namespace

std::vector<std::byte> serialize_image(const PreparedImage &image) {
  Writer writer;
  writer.text(artifact_magic);
  writer.text(Core::compiler_identity());
  writer.text(image.identity);
  writer.number(static_cast<std::uint8_t>(image.options.mode));
  writer.number(static_cast<std::uint8_t>(image.options.memory_mode));
  writer.number(image.units.size());
  for (const auto &unit : image.units) {
    writer.number(unit.variant);
    writer.number(unit.entry);
    writer.number(unit.mask);
    writer.number(unit.context);
    writer.text(unit.symbol);
  }
  writer.data(image.bitcode);
  for (const auto byte : checksum(writer.bytes)) {
    writer.bytes.push_back(static_cast<std::byte>(byte));
  }
  return std::move(writer.bytes);
}

std::shared_ptr<const PreparedImage> load_image(const CodeImage &code,
                                                const CompileOptions &options,
                                                std::span<const std::byte> artifact) {
  const auto identity = input_identity(code, options);
  validate_options(options, identity);
  constexpr std::size_t checksum_bytes = 32;
  if (artifact.size() < checksum_bytes) {
    refuse(FaultReason::invalid_image, identity, 0, 0, "truncated semantic artifact");
  }
  const auto body = artifact.first(artifact.size() - checksum_bytes);
  const auto expected = checksum(body);
  if (!std::ranges::equal(artifact.last(checksum_bytes), expected, {}, [](std::byte value) {
        return std::to_integer<std::uint8_t>(value);
      })) {
    refuse(FaultReason::invalid_image, identity, 0, 0, "semantic artifact checksum mismatch");
  }
  Reader reader(body, identity);
  if (reader.text() != artifact_magic || reader.text() != Core::compiler_identity() ||
      reader.text() != identity || reader.number() != static_cast<std::uint8_t>(options.mode) ||
      reader.number() != static_cast<std::uint8_t>(options.memory_mode)) {
    reader.invalid("artifact format, compiler, code or execution options mismatch");
  }
  auto result = std::make_shared<PreparedImage>();
  result->code = code;
  result->options = options;
  result->identity = identity;
  constexpr unsigned minimum_unit_bytes = 40;
  const auto count = reader.number();
  if (count == 0 || count > reader.remaining() / minimum_unit_bytes) {
    reader.invalid("invalid native unit count");
  }
  std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> contexts;
  std::set<std::string> names;
  constexpr auto allowed_modes = circular_mode | high_bank_mode | low_bank_mode | saturation_mode |
                                 truncation_mode | simd_mode | dag1_high_mode | dag1_low_mode |
                                 dag2_high_mode | dag2_low_mode | broadcast1_mode | broadcast9_mode;
  for (std::uint64_t index = 0; index < count; ++index) {
    NativeUnit unit{.variant = reader.word(),
                    .entry = reader.word(),
                    .mask = reader.word(),
                    .context = reader.word(),
                    .symbol = reader.text()};
    const auto variant = std::ranges::find(code.variants, unit.variant, &CodeVariant::id);
    const bool declared =
        variant != code.variants.end() &&
        (std::ranges::find(variant->entries, unit.entry) != variant->entries.end() ||
         std::ranges::any_of(variant->interrupt_vectors,
                             [&](const auto &request) { return request.vector == unit.entry; }));
    if (!declared || (unit.mask & ~allowed_modes) != 0 || (unit.context & ~unit.mask) != 0 ||
        unit.symbol.empty() || !contexts.emplace(unit.variant, unit.entry, unit.context).second ||
        !names.insert(unit.symbol).second) {
      reader.invalid("invalid or duplicated native unit metadata");
    }
    result->exports.push_back(unit.symbol);
    result->units.push_back(std::move(unit));
  }
  const auto bitcode = reader.data();
  result->bitcode.assign(bitcode.begin(), bitcode.end());
  if (reader.remaining() != 0) { reader.invalid("trailing semantic artifact data"); }
  llvm::LLVMContext context;
  (void)parse_module(*result, context);
  return result;
}

CompiledSymbols compile_image(const PreparedImage &image) {
  static const bool initialized =
      !llvm::InitializeNativeTarget() && !llvm::InitializeNativeTargetAsmPrinter();
  if (!initialized) {
    refuse(FaultReason::unsupported_feature,
           image.identity,
           0,
           0,
           "LLVM native target is unavailable");
  }
  auto target = llvm::orc::JITTargetMachineBuilder::detectHost();
  if (!target) { compiler_error(image, target.takeError()); }
  target->setCodeGenOptLevel(llvm::CodeGenOptLevel::Aggressive);
  auto machine = target->createTargetMachine();
  if (!machine) { compiler_error(image, machine.takeError()); }
  auto linked = std::make_shared<LinkedMap>();
  auto created = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*target)).create();
  if (!created) { compiler_error(image, created.takeError()); }
  auto jit = std::shared_ptr<llvm::orc::LLJIT>(std::move(*created));
  auto &layer = jit->getObjLinkingLayer();
  if (auto *jitlink = llvm::dyn_cast<llvm::orc::ObjectLinkingLayer>(&layer)) {
    jitlink->addPlugin(std::make_shared<LinkMapPlugin>(jit->getExecutionSession(), linked));
  } else if (auto *rtdyld = llvm::dyn_cast<llvm::orc::RTDyldObjectLinkingLayer>(&layer)) {
    rtdyld->setNotifyLoaded([linked](llvm::orc::MaterializationResponsibility &,
                                     const llvm::object::ObjectFile &object,
                                     const llvm::RuntimeDyld::LoadedObjectInfo &addresses) {
      linked->loaded(object, addresses);
    });
  } else {
    refuse(FaultReason::unsupported_feature,
           image.identity,
           0,
           0,
           "native linker does not expose linked sections");
  }
  auto context = std::make_unique<llvm::LLVMContext>();
  auto module = parse_module(image, *context);
  module->setDataLayout(jit->getDataLayout());
  module->setTargetTriple(jit->getTargetTriple().str());
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
  auto pipeline = passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
  pipeline.run(*module, modules);
  if (auto error =
          jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context)))) {
    compiler_error(image, std::move(error));
  }
  CompiledSymbols result;
  result.symbols.lifetime = jit;
  for (const auto &name : image.exports) {
    auto address = jit->lookup(name);
    if (!address) { compiler_error(image, address.takeError()); }
    result.symbols.entries.push_back({name, static_cast<std::uintptr_t>(address->getValue())});
  }
  if (!linked->error.empty()) {
    refuse(FaultReason::invalid_image,
           image.identity,
           0,
           0,
           "invalid native line table: " + linked->error);
  }
  result.native_bytes = linked->bytes;
  result.text_ranges = std::move(linked->text_ranges);
  result.sites = std::move(linked->sites);
  result.symbols.fault_map = {.text_ranges = result.text_ranges, .sites = result.sites};
  return result;
}

void validate_code_storage(const PreparedImage &image,
                           std::uint32_t variant_id,
                           const Memory &memory) {
  const auto variant = std::ranges::find(image.code.variants, variant_id, &CodeVariant::id);
  if (variant == image.code.variants.end()) {
    refuse(
        FaultReason::invalid_image, image.identity, variant_id, 0, "unknown installation variant");
  }
  constexpr unsigned parcel_bits = 16;
  for (const auto &span : variant->spans) {
    if (span.instruction_set != InstructionSet::isa || span.parcels.size() % 3 != 0) {
      refuse(FaultReason::invalid_image,
             image.identity,
             variant_id,
             span.address,
             "native installation requires complete classic ISA words");
    }
    for (std::size_t parcel = 0; parcel < span.parcels.size(); parcel += 3) {
      const auto address = span.address + static_cast<std::uint32_t>(parcel / 3);
      const auto expected = (std::uint64_t{span.parcels[parcel]} << (2 * parcel_bits)) |
                            (std::uint64_t{span.parcels[parcel + 1]} << parcel_bits) |
                            span.parcels[parcel + 2];
      const auto actual = code_word(*variant, memory, address, image);
      if (actual != expected) {
        refuse(FaultReason::invalid_image,
               image.identity,
               variant_id,
               address,
               "actual instruction word differs from the declared code",
               actual,
               full_instruction_bits);
      }
    }
  }
}
} // namespace sharc::detail
