// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <sharc/core.hpp>
#include <string_view>

namespace {
struct Case {
  std::string_view name;
  std::uint32_t field;
  bool legal;
};

// Hand-authored examples use distinct destinations and sources. No production
// opcode table or decoding helper participates in these expectations.
constexpr std::array cases{
    Case{.name = "fixed add", .field = 0x001123, .legal = true},
    Case{.name = "floating add", .field = 0x081123, .legal = true},
    Case{.name = "fixed COMP", .field = 0x00a023, .legal = true},
    Case{.name = "FIX with scaling", .field = 0x0d9123, .legal = true},
    Case{.name = "ALU hole next to XOR/NOT", .field = 0x044123, .legal = false},
    Case{.name = "reserved third compute unit", .field = 0x301123, .legal = false},
    Case{.name = "fractional rounded multiply", .field = 0x179123, .legal = true},
    Case{.name = "integer rounded multiply", .field = 0x171123, .legal = false},
    Case{.name = "missing plain-multiply destination", .field = 0x172123, .legal = false},
    Case{.name = "integer multiply to MRF", .field = 0x174023, .legal = true},
    Case{.name = "floating multiply", .field = 0x130123, .legal = true},
    Case{.name = "multiplier hole next to floating multiply", .field = 0x131123, .legal = false},
    Case{.name = "SAT MRF integer unsigned", .field = 0x100100, .legal = true},
    Case{.name = "RND MRB", .field = 0x11b100, .legal = true},
    Case{.name = "FDEP shifter opcode colliding with ALU hole", .field = 0x244123, .legal = true},
    Case{.name = "FPACK", .field = 0x290120, .legal = true},
    Case{.name = "ADSP-214xx BITEXT", .field = 0x250120, .legal = true},
    Case{.name = "ASHIFT", .field = 0x204123, .legal = true},
    Case{.name = "unaligned shifter opcode", .field = 0x205123, .legal = false},
    Case{.name = "MR2F read", .field = 0x402100, .legal = true},
    Case{.name = "reserved MR part", .field = 0x403100, .legal = false},
    Case{.name = "MR2B write", .field = 0x416100, .legal = true},
    Case{.name = "multiplier with ALU average", .field = 0x4612e4, .legal = true},
    Case{.name = "multifunction opcode gap", .field = 0x4712e4, .legal = false},
    Case{.name = "floating multiply and FIX", .field = 0x5b12e4, .legal = true},
    Case{.name = "floating multiply and MIN", .field = 0x5f12e4, .legal = true},
    Case{.name = "multiply and dual ALU", .field = 0x6312e4, .legal = true},
    Case{.name = "floating multiply and dual ALU", .field = 0x7312e4, .legal = true},
    Case{.name = "fixed dual ALU high second destination", .field = 0x07f123, .legal = true},
    Case{.name = "floating dual ALU high second destination", .field = 0x0ff123, .legal = true}};

void verify(const sharc::CodeImage &image, bool legal, std::string_view name) {
  try {
    (void)sharc::Core::lower(image);
  } catch (const sharc::PreparationError &error) {
    const auto &fault = error.fault();
    const auto expected =
        legal ? sharc::FaultReason::unsupported_instruction : sharc::FaultReason::invalid_encoding;
    fixture::check(fault.reason == expected, name);
    fixture::check(fault.address == fixture::code_address && fault.variant == 7 &&
                       !fault.identity.empty(),
                   "COMPUTE refusal lost guest context");
    if (!legal) {
      fixture::check(fault.detail.contains("COMPUTE"), "missing compute refusal reason");
    }
    return;
  }
  fixture::check(false, "COMPUTE executed before native lowering exists");
}

void computations() {
  constexpr std::uint64_t full_header = 0x013e00000000;
  constexpr std::uint64_t wide_header = 0x018000000000;
  for (const auto &item : cases) {
    for (const auto set : {sharc::InstructionSet::isa, sharc::InstructionSet::visa}) {
      verify(fixture::image(set, full_header | item.field), item.legal, item.name);
    }
    verify(fixture::image(sharc::InstructionSet::visa,
                          wide_header | (std::uint64_t{item.field} << 16),
                          fixture::Width::wide_word),
           item.legal,
           item.name);
  }
  constexpr unsigned compact_prefix = 0xc000;
  constexpr unsigned compact_operands = 0x12;
  for (unsigned opcode = 0; opcode < 16; ++opcode) {
    const auto parcel =
        static_cast<std::uint16_t>(compact_prefix | (opcode << 8) | compact_operands);
    verify(fixture::image(sharc::InstructionSet::visa, {parcel}), true, "compact compute form");
  }
}

void shifts() {
  constexpr std::uint64_t immediate_shift = 0x800000000000;
  constexpr std::uint32_t deposit_immediate = 0x110c23;
  constexpr std::uint32_t reserved_immediate = 0x060c23;
  constexpr std::uint32_t reserved_high_bit = 0x400c23;
  verify(fixture::image(sharc::InstructionSet::isa, immediate_shift | deposit_immediate),
         true,
         "FDEP immediate");
  verify(fixture::image(sharc::InstructionSet::visa, immediate_shift | reserved_immediate),
         false,
         "reserved immediate shifter operation");
  verify(fixture::image(sharc::InstructionSet::isa, immediate_shift | reserved_high_bit),
         false,
         "reserved immediate-shift bit 22");
}
} // namespace

int main() try {
  computations();
  shifts();
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
