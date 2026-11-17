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
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
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
constexpr std::string_view artifact_magic = "sharc-llvm-image-v2";
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
      reader.text() != identity || reader.number() != static_cast<std::uint8_t>(options.mode)) {
    reader.invalid("artifact format, compiler, code or numerical mode mismatch");
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
  auto created = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*target)).create();
  if (!created) { compiler_error(image, created.takeError()); }
  auto jit = std::shared_ptr<llvm::orc::LLJIT>(std::move(*created));
  auto bytes = std::make_shared<std::size_t>(0);
  jit->getObjTransformLayer().setTransform(
      [bytes](std::unique_ptr<llvm::MemoryBuffer> object)
          -> llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> {
        auto parsed = llvm::object::ObjectFile::createObjectFile(object->getMemBufferRef());
        if (!parsed) { return parsed.takeError(); }
        for (const auto &section : (*parsed)->sections()) {
          if (section.isText()) { *bytes += static_cast<std::size_t>(section.getSize()); }
        }
        return object;
      });
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
  result.native_bytes = *bytes;
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
