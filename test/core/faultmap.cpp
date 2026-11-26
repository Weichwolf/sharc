// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "../support/native_provider.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <llvm/Support/TargetSelect.h>
#include <memory>
#include <sharc/core.hpp>
#include <span>
#include <utility>
#include <vector>

namespace {
using fixture::check;
using fixture::link;
using fixture::Provider;
constexpr std::uint32_t last_pc = 0xffffff;
constexpr std::uint64_t excessive_variant = std::uint64_t{1} << 32;

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
    fixture::install(core, native, registers, variant);
    const auto run = core.run(1);
    check(fixture::completed(run, 1, 1) &&
              core.registers().pe[0].data[0][0] == std::uint64_t{result} << 8,
          "bound image lost code lifetime after releasing fault-map input buffers");
  }
}
} // namespace

int main() {
  return fixture::run_test([] {
    check(!llvm::InitializeNativeTarget() && !llvm::InitializeNativeTargetAsmPrinter(),
          "LLVM unavailable");
    for (const auto mode : fixture::numeric_modes) { qualify(mode); }
    std::cout << "native fault-map binding passed\n";
  });
}
