// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "sharc/core.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <string>

namespace {
constexpr unsigned full_width = 48, wide_width = 32;
constexpr std::uint64_t classic_branch = 0xc00000001123;
constexpr std::uint16_t compact_immediate_head = 0x0f81;
constexpr std::uint16_t immediate_high = 0x1234, immediate_low = 0xabcd;
constexpr std::uint16_t conditional_compute_head = 0x013e, add_operands = 0x1123;
constexpr std::uint32_t next_classic_pc = 0x101, next_visa_pc = 0x103;
constexpr std::uint32_t last_pc = 0xffffff, immediate_value = 0xdeadbeef;
using fixture::check;
using sharc::Core;
using sharc::FaultReason;
using sharc::InstructionSet;

sharc::Fault refusal(const sharc::CodeImage &image, const sharc::CompileOptions &options = {}) {
  try {
    const auto semantic = Core::lower(image, options);
    (void)semantic;
  } catch (const sharc::PreparationError &error) {
    check(std::string(error.what()).contains(error.fault().identity), "diagnostic lacks identity");
    check(std::string(error.what()).contains(std::to_string(error.fault().address)),
          "diagnostic lacks PC");
    return error.fault();
  }
  check(false, "no instruction has lowering in this increment");
  return {};
}

void formats() {
  struct Format {
    std::uint64_t word;
    unsigned type, bits;
  };

  // Synthetic field combinations cover every documented top-level format.
  constexpr std::array cases{Format{.word = 0x200000001123, .type = 1, .bits = 48},
                             Format{.word = 0x2000003f0000, .type = 1, .bits = 32},
                             Format{.word = 0x013e00001123, .type = 2, .bits = 48},
                             Format{.word = 0x018011230000, .type = 2, .bits = 32},
                             Format{.word = 0xc01200000000, .type = 2, .bits = 16},
                             Format{.word = 0x400000001123, .type = 3, .bits = 48},
                             Format{.word = 0x4000003f0000, .type = 3, .bits = 32},
                             Format{.word = 0x901200000000, .type = 3, .bits = 16},
                             Format{.word = 0x600000001123, .type = 4, .bits = 48},
                             Format{.word = 0x6000003f0000, .type = 4, .bits = 32},
                             Format{.word = 0x700000001123, .type = 5, .bits = 48},
                             Format{.word = 0x7000003f0000, .type = 5, .bits = 32},
                             Format{.word = 0x800000001123, .type = 6, .bits = 48},
                             Format{.word = 0x040000001123, .type = 7, .bits = 48},
                             Format{.word = 0x0400003f0000, .type = 7, .bits = 32},
                             Format{.word = 0x063e00000100, .type = 8, .bits = 48},
                             Format{.word = 0x083e00001123, .type = 9, .bits = 48},
                             Format{.word = 0x083e003f0000, .type = 9, .bits = 32},
                             Format{.word = 0x0a3e00001123, .type = 11, .bits = 48},
                             Format{.word = 0x0abe00000000, .type = 11, .bits = 16},
                             Format{.word = 0x0c0004000003, .type = 12, .bits = 48},
                             Format{.word = 0x0e0000000003, .type = 13, .bits = 48},
                             Format{.word = 0x100000004321, .type = 14, .bits = 48},
                             Format{.word = 0xa00000004321, .type = 15, .bits = 48},
                             Format{.word = 0x900801230000, .type = 15, .bits = 32},
                             Format{.word = 0x900000004321, .type = 16, .bits = 48},
                             Format{.word = 0x900443210000, .type = 16, .bits = 32},
                             Format{.word = 0x0f0100004321, .type = 17, .bits = 48},
                             Format{.word = 0x0f8143210000, .type = 17, .bits = 32},
                             Format{.word = 0x140200000001, .type = 18, .bits = 48},
                             Format{.word = 0x160000000001, .type = 19, .bits = 48},
                             Format{.word = 0x170200000000, .type = 20, .bits = 48},
                             Format{.word = 0x000000000000, .type = 21, .bits = 48},
                             Format{.word = 0x000100000000, .type = 21, .bits = 16},
                             Format{.word = 0x008000000000, .type = 22, .bits = 48},
                             Format{.word = 0x008100000000, .type = 22, .bits = 16},
                             Format{.word = 0x180400000123, .type = 25, .bits = 48},
                             Format{.word = 0x190100000000, .type = 25, .bits = 16}};
  for (const auto &item : cases) {
    const auto fault = refusal(
        fixture::image(InstructionSet::visa, item.word, static_cast<fixture::Width>(item.bits)));
    check(fault.reason == FaultReason::unsupported_instruction, "format misclassified");
    check(fault.instruction_bits == item.bits && fault.instruction == item.word,
          "wrong width or MSB alignment");
    char suffix = 'c';
    if (item.bits == full_width) { suffix = 'a'; }
    if (item.bits == wide_width) { suffix = 'b'; }
    const auto name = "type " + std::to_string(item.type) + suffix;
    check(fault.detail.starts_with(name), "wrong instruction class");
    if (item.bits == full_width) {
      const auto classic = refusal(fixture::image(InstructionSet::isa, item.word));
      check(classic.reason == FaultReason::unsupported_instruction &&
                classic.instruction_bits == full_width && classic.detail.starts_with(name),
            "classic format disagrees with its full-width VISA form");
    }
  }
  auto fault = refusal(fixture::image(InstructionSet::isa, classic_branch));
  check(fault.reason == FaultReason::unsupported_instruction &&
            fault.detail.starts_with("type 10a"),
        "classic 1100 prefix must not become VISA short compute");
  fault = refusal(
      fixture::image(InstructionSet::isa, {compact_immediate_head, immediate_high, immediate_low}));
  check(fault.instruction_bits == full_width, "VISA marker must not shorten ISA");
}

void boundaries() {
  auto image = fixture::image(InstructionSet::visa,
                              {0x0001,
                               compact_immediate_head,
                               immediate_high,
                               conditional_compute_head,
                               0x0000,
                               add_operands});
  for (const auto entry : {0x100U, next_classic_pc, next_visa_pc}) {
    image.variants.front().entries = {entry};
    check(refusal(image).reason == FaultReason::unsupported_instruction, "valid VISA PC refused");
  }
  for (const auto entry : {0x102U, 0x104U, 0x105U, 0x106U}) {
    image.variants.front().entries = {entry};
    const auto fault = refusal(image);
    check(fault.reason == FaultReason::undeclared_entry && fault.address == entry,
          "entry inside instruction accepted");
  }
  for (const auto set : {InstructionSet::isa, InstructionSet::visa}) {
    image = fixture::image(set, {0, 0, 0, 0x0200, 0, 0});
    const auto fault = refusal(image);
    check(fault.reason == FaultReason::invalid_encoding, "later invalid prefix not decoded");
    check(fault.address == (set == InstructionSet::isa ? next_classic_pc : next_visa_pc),
          "wrong PC unit");
  }
  for (const auto parcels : {std::initializer_list<std::uint16_t>{0x0f01},
                             {0x0f01, immediate_high},
                             {compact_immediate_head},
                             {0x2000}}) {
    image = fixture::image(InstructionSet::visa, parcels);
    check(refusal(image).reason == FaultReason::truncated_instruction, "truncation accepted");
  }
  image = fixture::image(InstructionSet::visa, {0x0001, 0x0200});
  check(refusal(image).address == next_classic_pc, "short instruction consumed an extra parcel");
  image = fixture::image(InstructionSet::isa, {0, 0, 0}, last_pc);
  check(refusal(image).reason == FaultReason::unsupported_instruction, "last ISA PC refused");
  image.variants.front().spans.front().instruction_set = InstructionSet::visa;
  check(refusal(image).reason == FaultReason::invalid_image, "VISA PC range overflow accepted");
}

void declarations() {
  check(refusal({}).reason == FaultReason::invalid_image, "empty image accepted");
  auto image = fixture::image(InstructionSet::isa, fixture::immediate(1, immediate_value));
  image.variants.push_back(image.variants.front());
  check(refusal(image).reason == FaultReason::invalid_image, "duplicate variant accepted");
  image.variants.pop_back();
  auto &variant = image.variants.front();
  variant.spans.push_back(variant.spans.front());
  check(refusal(image).reason == FaultReason::invalid_image, "overlapping spans accepted");
  variant.spans.pop_back();
  variant.interrupt_vectors = {{.source = 8, .vector = next_classic_pc}};
  check(refusal(image).reason == FaultReason::undeclared_entry, "undeclared vector accepted");
  variant.interrupt_vectors = {{.source = 32, .vector = 0x100}};
  check(refusal(image).reason == FaultReason::invalid_image, "invalid interrupt source accepted");
  variant.interrupt_vectors.clear();
  image.transitions = {{.from_variant = 7, .address = 0x100, .to_variants = {9}}};
  check(refusal(image).reason == FaultReason::invalid_image, "unknown transition target accepted");
  for (const auto word : {0x020000000000ULL,
                          0x030000000000ULL,
                          0x150000000000ULL,
                          0x00c000000000ULL,
                          0x900c00000000ULL,
                          0xd00000000000ULL}) {
    const auto fault = refusal(fixture::image(InstructionSet::visa, word));
    check(fault.reason == FaultReason::invalid_encoding, "reserved VISA form accepted");
  }
}

void identities() {
  auto image = fixture::image(InstructionSet::isa, fixture::add(1, 2, 3));
  const auto original = refusal(image).identity;
  check(original.size() == 64 && original == refusal(image).identity, "unstable input identity");
  sharc::CompileOptions options;
  options.mode = sharc::ExecutionMode::fast_fp32;
  check(original != refusal(image, options).identity, "numerical modes share identity");
  options = {};
  options.callbacks = {{.role = "read", .symbol = "consumer_read"}};
  check(original != refusal(image, options).identity, "callback symbol absent from identity");
  image.variants.front().layout.regions.push_back({});
  check(original != refusal(image).identity, "memory layout absent from identity");
  image.variants.front().layout.regions.clear();
  image.variants.front().spans.front().parcels.back() ^= 1;
  check(original != refusal(image).identity, "code contents absent from identity");
  fixture::throws<sharc::PreparationError>([&] { (void)Core::compile(image); },
                                           "compile bypassed unsupported lowering");
  fixture::throws<sharc::PreparationError>([&] { (void)Core::load(image, {}, {}); },
                                           "empty artifact accepted");
}
} // namespace

int main() try {
  formats();
  boundaries();
  declarations();
  identities();
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
