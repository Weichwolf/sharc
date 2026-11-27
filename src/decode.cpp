// SPDX-License-Identifier: GPL-3.0-only
#include "decode.hpp"

#include "sharc/core.hpp"
#include "sharc/memory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/SHA256.h>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sharc::detail {
namespace {
class Digest {
public:
  void number(std::uint64_t value) {
    std::array<std::uint8_t, 8> bytes{};
    for (auto &byte : bytes) {
      byte = static_cast<std::uint8_t>(value);
      value >>= 8;
    }
    hash_.update(bytes);
  }

  void text(std::string_view value) {
    number(value.size());
    hash_.update(llvm::StringRef(value));
  }

  [[nodiscard]] std::string finish() {
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result;
    for (const auto byte : hash_.final()) {
      result += hex[byte >> 4];
      result += hex[byte & 15];
    }
    return result;
  }

private:
  llvm::SHA256 hash_;
};

// Structural format recognition from Programming Reference 2.4, chapter 10.
// Operand legality and compute opcodes are deliberately not claimed here.
struct Format {
  std::uint16_t mask, value;
  std::uint8_t type, bits;
};

constexpr std::array classic_formats{
    Format{.mask = 0xe000, .value = 0x2000, .type = 1, .bits = 48},
    Format{.mask = 0xe000, .value = 0x4000, .type = 3, .bits = 48},
    Format{.mask = 0xf000, .value = 0x6000, .type = 4, .bits = 48},
    Format{.mask = 0xf000, .value = 0x7000, .type = 5, .bits = 48},
    Format{.mask = 0xf000, .value = 0x8000, .type = 6, .bits = 48},
    Format{.mask = 0xf000, .value = 0x9000, .type = 16, .bits = 48},
    Format{.mask = 0xe000, .value = 0xa000, .type = 15, .bits = 48},
    Format{.mask = 0xe000, .value = 0xc000, .type = 10, .bits = 48},
    Format{.mask = 0xe000, .value = 0xe000, .type = 10, .bits = 48},
    Format{.mask = 0xfc00, .value = 0x1000, .type = 14, .bits = 48},
    Format{.mask = 0xffc0, .value = 0x0000, .type = 21, .bits = 48},
    Format{.mask = 0xffc0, .value = 0x0040, .type = 22, .bits = 48},
    Format{.mask = 0xffc0, .value = 0x0080, .type = 22, .bits = 48},
    Format{.mask = 0xff00, .value = 0x0100, .type = 2, .bits = 48},
    Format{.mask = 0xff00, .value = 0x0200, .type = 6, .bits = 48},
    Format{.mask = 0xff00, .value = 0x0400, .type = 7, .bits = 48},
    Format{.mask = 0xff00, .value = 0x0600, .type = 8, .bits = 48},
    Format{.mask = 0xff00, .value = 0x0700, .type = 8, .bits = 48},
    Format{.mask = 0xff00, .value = 0x0800, .type = 9, .bits = 48},
    Format{.mask = 0xff00, .value = 0x0900, .type = 9, .bits = 48},
    Format{.mask = 0xff00, .value = 0x0a00, .type = 11, .bits = 48},
    Format{.mask = 0xff00, .value = 0x0b00, .type = 11, .bits = 48},
    Format{.mask = 0xff00, .value = 0x0c00, .type = 12, .bits = 48},
    Format{.mask = 0xff00, .value = 0x0d00, .type = 12, .bits = 48},
    Format{.mask = 0xff00, .value = 0x0e00, .type = 13, .bits = 48},
    Format{.mask = 0xff00, .value = 0x0f00, .type = 17, .bits = 48},
    Format{.mask = 0xff00, .value = 0x1400, .type = 18, .bits = 48},
    Format{.mask = 0xff00, .value = 0x1600, .type = 19, .bits = 48},
    Format{.mask = 0xff00, .value = 0x1700, .type = 20, .bits = 48},
    Format{.mask = 0xff00, .value = 0x1800, .type = 25, .bits = 48},
    Format{.mask = 0xff00, .value = 0x1900, .type = 25, .bits = 48}};

constexpr std::array compact_formats{
    Format{.mask = 0xf000, .value = 0xc000, .type = 2, .bits = 16},
    Format{.mask = 0xf010, .value = 0x9010, .type = 3, .bits = 16},
    Format{.mask = 0xf01c, .value = 0x9004, .type = 16, .bits = 32},
    Format{.mask = 0xf01c, .value = 0x9008, .type = 15, .bits = 32},
    Format{.mask = 0xff80, .value = 0x0180, .type = 2, .bits = 32},
    Format{.mask = 0xff80, .value = 0x0f80, .type = 17, .bits = 32},
    Format{.mask = 0xff80, .value = 0x0a80, .type = 11, .bits = 16},
    Format{.mask = 0xff80, .value = 0x0b80, .type = 11, .bits = 16},
    Format{.mask = 0xffc1, .value = 0x0001, .type = 21, .bits = 16},
    Format{.mask = 0xffc1, .value = 0x0041, .type = 22, .bits = 16},
    Format{.mask = 0xffc1, .value = 0x0081, .type = 22, .bits = 16},
    Format{.mask = 0xffff, .value = 0x1901, .type = 25, .bits = 16}};

std::optional<Format> match(std::span<const Format> formats, std::uint16_t head) {
  const auto found = std::ranges::find_if(
      formats, [head](const Format &format) { return (head & format.mask) == format.value; });
  if (found == formats.end()) { return {}; }
  return *found;
}

Instruction instruction(std::span<const std::uint16_t> parcels,
                        InstructionSet set,
                        std::string_view identity,
                        std::uint32_t variant,
                        std::uint32_t pc) {
  const auto head = parcels.front();
  const bool visa = set == InstructionSet::visa;
  auto format = visa ? match(compact_formats, head) : std::optional<Format>{};
  if (!format) { format = match(classic_formats, head); }
  // 1001/011 is reserved; 1101/111x have only the ISA type-10 meaning.
  constexpr std::uint16_t reserved_mask = 0xf01c;
  constexpr std::uint16_t reserved_value = 0x900c;
  if (!format || (visa && ((head & reserved_mask) == reserved_value || format->type == 10))) {
    refuse(FaultReason::invalid_encoding,
           identity,
           variant,
           pc,
           "reserved instruction prefix or subformat",
           std::uint64_t{head} << 32);
  }
  Instruction result{.variant = variant,
                     .address = pc,
                     .word = std::uint64_t{head} << 32,
                     .type = format->type,
                     .bits = format->bits,
                     .compute = {},
                     .instruction_set = set};
  const auto need = [&](std::size_t count) {
    if (parcels.size() < count) {
      refuse(FaultReason::truncated_instruction,
             identity,
             variant,
             pc,
             "instruction crosses the declared span end",
             result.word,
             result.bits);
    }
  };
  constexpr std::array optional_compute_types{1, 3, 4, 5, 7, 9};
  constexpr std::uint16_t compute_prefix_mask = 0x7f;
  constexpr std::uint16_t no_compute_prefix = 0x3f;
  if (visa && result.bits == full_instruction_bits &&
      std::ranges::find(optional_compute_types, result.type) != optional_compute_types.end()) {
    need(2);
    if ((parcels[1] & compute_prefix_mask) == no_compute_prefix) {
      result.bits = wide_instruction_bits;
    }
  }
  const auto count = static_cast<std::size_t>(result.bits / short_instruction_bits);
  need(count);
  if (count >= 2) { result.word |= std::uint64_t{parcels[1]} << 16; }
  if (count == 3) { result.word |= parcels[2]; }
  result.compute = decode_compute(result, identity);
  return result;
}
} // namespace

[[noreturn]] void refuse(FaultReason reason,
                         std::string_view identity,
                         std::uint32_t variant,
                         std::uint32_t address,
                         std::string message,
                         std::uint64_t word,
                         std::uint8_t bits) {
  throw PreparationError({.reason = reason,
                          .identity = std::string(identity),
                          .variant = variant,
                          .address = address,
                          .instruction = word,
                          .instruction_bits = bits,
                          .detail = std::move(message)});
}

MemoryMove memory_move(const Instruction &instruction) {
  constexpr unsigned dag_shift = 44;
  constexpr unsigned index_shift = 41;
  constexpr unsigned register_shift = 32;
  constexpr std::uint64_t memory_write = std::uint64_t{1} << 40;
  const bool program = ((instruction.word >> dag_shift) & 1) != 0;
  const auto ureg = static_cast<unsigned>((instruction.word >> register_shift) & 0x7f);
  return {.index =
              static_cast<unsigned>((instruction.word >> index_shift) & 7) + (program ? 8U : 0U),
          .reg = ureg & 15,
          .lane = ureg < 16 ? 0U : 1U,
          .space = program ? MemorySpace::program : MemorySpace::data,
          .write = (instruction.word & memory_write) != 0};
}

MemoryMoves memory_moves(const Instruction &instruction) {
  if (instruction.type == memory_move_type) {
    return {.transfers = {memory_move(instruction), {}}, .count = 1};
  }
  if (instruction.type != parallel_memory_type) { return {}; }
  constexpr unsigned dm_direction = 44;
  constexpr unsigned dm_index = 41;
  constexpr unsigned dm_modifier = 38;
  constexpr unsigned dm_register = 33;
  constexpr unsigned pm_direction = 37;
  constexpr unsigned pm_index = 30;
  constexpr unsigned pm_modifier = 27;
  constexpr unsigned pm_register = 23;
  const auto field = [&](unsigned shift, unsigned mask) {
    return static_cast<unsigned>((instruction.word >> shift) & mask);
  };
  const MemoryMove data{.index = field(dm_index, 7),
                        .reg = field(dm_register, 15),
                        .modifier = field(dm_modifier, 7),
                        .space = MemorySpace::data,
                        .write = field(dm_direction, 1) != 0};
  const MemoryMove program{.index = 8 + field(pm_index, 7),
                           .reg = field(pm_register, 15),
                           .modifier = 8 + field(pm_modifier, 7),
                           .space = MemorySpace::program,
                           .write = field(pm_direction, 1) != 0};
  return {.transfers = {data, program}, .count = 2};
}

void validate_options(const CompileOptions &options, std::string_view identity) {
  if (options.mode != ExecutionMode::strict && options.mode != ExecutionMode::fast_fp32) {
    refuse(FaultReason::invalid_image, identity, 0, 0, "unknown numerical mode");
  }
  if (options.memory_mode != CompileOptions::MemoryMode::exact &&
      options.memory_mode != CompileOptions::MemoryMode::direct) {
    refuse(FaultReason::invalid_image, identity, 0, 0, "unknown memory mode");
  }
  std::set<std::string_view> roles;
  for (const auto &binding : options.callbacks) {
    if ((binding.role != io_read_role && binding.role != io_write_role) ||
        !roles.insert(binding.role).second || binding.symbol.empty() ||
        binding.symbol.contains('\0') || binding.symbol.starts_with("llvm.")) {
      refuse(
          FaultReason::invalid_image, identity, 0, 0, "invalid or duplicated I/O symbol binding");
    }
  }
}

std::string input_identity(const CodeImage &image, const CompileOptions &options) {
  Digest digest;
  digest.text(Core::compiler_identity());
  digest.number(static_cast<std::uint8_t>(options.mode));
  digest.number(static_cast<std::uint8_t>(options.memory_mode));
  digest.number(options.callbacks.size());
  for (const auto &callback : options.callbacks) {
    digest.text(callback.role);
    digest.text(callback.symbol);
  }
  digest.number(image.variants.size());
  for (const auto &variant : image.variants) {
    digest.number(variant.id);
    digest.number(variant.spans.size());
    for (const auto &span : variant.spans) {
      digest.number(span.address);
      digest.number(static_cast<std::uint8_t>(span.instruction_set));
      digest.number(span.parcels.size());
      for (const auto parcel : span.parcels) { digest.number(parcel); }
    }
    digest.number(variant.entries.size());
    for (const auto entry : variant.entries) { digest.number(entry); }
    digest.number(variant.interrupt_vectors.size());
    for (const auto &request : variant.interrupt_vectors) {
      digest.number(request.source);
      digest.number(request.vector);
    }
    digest.number(static_cast<std::uint64_t>(variant.layout.ordinary_regions));
    digest.number(variant.layout.regions.size());
    for (const auto &region : variant.layout.regions) {
      digest.number(static_cast<std::uint8_t>(region.space));
      digest.number(static_cast<std::uint8_t>(region.view));
      digest.number(region.first);
      digest.number(region.count);
      digest.number(region.storage_id);
      digest.number(region.byte_offset);
      digest.number(region.byte_stride);
      digest.number(region.readable ? 1 : 0);
      digest.number(region.writable ? 1 : 0);
      digest.number(region.initialized ? 1 : 0);
    }
  }
  digest.number(image.transitions.size());
  for (const auto &transition : image.transitions) {
    digest.number(transition.from_variant);
    digest.number(transition.address);
    digest.number(transition.to_variants.size());
    for (const auto target : transition.to_variants) { digest.number(target); }
  }
  return digest.finish();
}

namespace {
class ImageDecoder {
public:
  explicit ImageDecoder(std::string_view identity) : identity_(identity) {}

  [[noreturn]] void invalid(std::uint32_t variant, std::uint32_t pc, std::string message) const {
    refuse(FaultReason::invalid_image, identity_, variant, pc, std::move(message));
  }

  void span(const CodeSpan &code,
            std::uint32_t variant,
            std::vector<std::pair<std::uint64_t, std::uint64_t>> &ranges) {
    const auto set = code.instruction_set;
    if (set != InstructionSet::isa && set != InstructionSet::visa) {
      invalid(variant, code.address, "unknown instruction set");
    }
    if (code.parcels.empty()) { invalid(variant, code.address, "empty code span"); }
    const bool visa = set == InstructionSet::visa;
    const auto extent = visa ? code.parcels.size() : (code.parcels.size() + 2) / 3;
    const auto end = std::uint64_t{code.address} + extent;
    if (end > std::uint64_t{maximum_pc} + 1) {
      invalid(variant, code.address, "code span exceeds the 24-bit PC space");
    }
    for (const auto &[first, last] : ranges) {
      if (code.address < last && first < end) {
        invalid(variant, code.address, "overlapping executable spans");
      }
    }
    ranges.emplace_back(code.address, end);
    std::size_t offset = 0;
    while (offset < code.parcels.size()) {
      const auto pc = code.address + static_cast<std::uint32_t>(visa ? offset : offset / 3);
      const auto op =
          instruction(std::span(code.parcels).subspan(offset), set, identity_, variant, pc);
      boundaries_.emplace(variant, pc);
      instructions_.push_back(op);
      offset += op.bits / short_instruction_bits;
    }
  }

  void entry(std::uint32_t variant, std::uint32_t address) const {
    if (!boundaries_.contains({variant, address})) {
      refuse(FaultReason::undeclared_entry,
             identity_,
             variant,
             address,
             "entry does not name a declared instruction boundary");
    }
  }

  void variant(const CodeVariant &code) {
    if (!variants_.emplace(code.id, &code).second) {
      invalid(code.id, 0, "duplicate code variant id");
    }
    if (code.spans.empty() || code.entries.empty()) {
      invalid(code.id, 0, "variant needs code spans and entries");
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (const auto &code_span : code.spans) { span(code_span, code.id, ranges); }
    for (const auto address : code.entries) { entry(code.id, address); }
    std::set<std::uint8_t> sources;
    for (const auto &vector : code.interrupt_vectors) {
      if (vector.source >= interrupt_source_count || !sources.insert(vector.source).second) {
        invalid(code.id, vector.vector, "invalid or duplicate interrupt source");
      }
      entry(code.id, vector.vector);
    }
  }

  void transition(const CodeTransition &code) {
    if (!variants_.contains(code.from_variant) || code.address > maximum_pc ||
        code.to_variants.empty()) {
      invalid(code.from_variant, code.address, "invalid code transition source or targets");
    }
    if (!transitions_.emplace(code.from_variant, code.address).second) {
      invalid(code.from_variant, code.address, "duplicate code transition");
    }
    std::set<std::uint32_t> targets;
    for (const auto target : code.to_variants) {
      const auto destination = variants_.find(target);
      if (destination == variants_.end() || target == code.from_variant ||
          !targets.insert(target).second) {
        invalid(code.from_variant, code.address, "invalid or duplicate code transition target");
      }
      const auto &entries = destination->second->entries;
      if (std::ranges::find(entries, code.address) == entries.end()) {
        invalid(target, code.address, "code transition target is not an entry in its variant");
      }
    }
  }

  std::vector<Instruction> take_instructions() { return std::move(instructions_); }

private:
  std::vector<Instruction> instructions_;
  std::string_view identity_;
  std::map<std::uint32_t, const CodeVariant *> variants_;
  std::set<std::pair<std::uint32_t, std::uint32_t>> transitions_;
  std::set<std::pair<std::uint32_t, std::uint32_t>> boundaries_;
};
} // namespace

std::vector<Instruction> decode(const CodeImage &image, std::string_view identity) {
  ImageDecoder decoder(identity);
  if (image.variants.empty()) { decoder.invalid(0, 0, "no code variants declared"); }
  for (const auto &variant : image.variants) { decoder.variant(variant); }
  for (const auto &transition : image.transitions) { decoder.transition(transition); }
  return decoder.take_instructions();
}
} // namespace sharc::detail
