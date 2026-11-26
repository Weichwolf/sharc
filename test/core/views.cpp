// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "../support/signal_handler.hpp"
#include "../support/signals.h"
#include "../support/word_space.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/types.h>

namespace {
using fixture::check;
using MemoryMode = sharc::CompileOptions::MemoryMode;
constexpr std::uint64_t word_count = std::uint64_t{1} << 32, view_bytes = word_count * 4;
constexpr std::uint32_t ram_id = 11, ram_address = 0x90000;
constexpr unsigned raw_shift = 8;
constexpr std::uint64_t raw_value = 0x12345678ab;
constexpr std::uint64_t lane_difference = 0x1111111100, extension_mask = 0xff;
constexpr std::uint32_t simd = 1U << 21;
constexpr std::uint64_t load_r1 = 0xa00100000000, store_r0 = 0xa10000000000;
constexpr auto last_word = std::numeric_limits<std::uint32_t>::max();

enum class Access : std::uint8_t { read = PROT_READ, write = PROT_READ | PROT_WRITE };

class Window {
public:
  explicit Window(std::size_t page_bytes)
      : page_bytes_(page_bytes), mapping_(view_bytes + page_bytes, PROT_NONE) {}

  void map(const fixture::Backing &backing,
           std::uint64_t offset,
           Access access,
           std::size_t file_page) const {
    auto *target = bytes().data() + offset;
    check(mmap(target,
               page_bytes_,
               static_cast<int>(access),
               MAP_SHARED | MAP_FIXED,
               backing.descriptor(),
               static_cast<off_t>(file_page * page_bytes_)) == target,
          "could not alias view page");
  }

  void protect(std::uint32_t address) const {
    const auto offset = (std::uint64_t{address} * 4 / page_bytes_) * page_bytes_;
    mapping_.protect(offset, page_bytes_, PROT_NONE);
  }

  [[nodiscard]] std::span<std::byte> bytes() const { return mapping_.bytes().first(view_bytes); }

  [[nodiscard]] sharc::Memory::View loan() const {
    return {.data = bytes(), .mirrored_tail = {bytes().data() + view_bytes, page_bytes_}};
  }

  [[nodiscard]] std::uint32_t get(std::uint32_t address) const {
    return fixture::get_word(bytes().data() + (std::uint64_t{address} * 4));
  }

  [[nodiscard]] std::uint32_t tail_word() const {
    return fixture::get_word(mapping_.bytes().subspan(view_bytes).data());
  }

private:
  std::size_t page_bytes_;
  fixture::Mapping mapping_;
};

struct Aliases {
  const std::size_t page{fixture::page_size()};
  const fixture::Backing backing{page * 2};
  const Window read{page};
  const Window write{page};

  Aliases() {
    const auto ram_page = (std::uint64_t{ram_address} * 4 / page) * page;
    const auto top_page = view_bytes - page;
    for (const auto *window : {&read, &write}) {
      const auto access = window == &read ? Access::read : Access::write;
      window->map(backing, ram_page, access, 0);
      for (const auto offset : {std::size_t{0}, top_page, view_bytes}) {
        window->map(backing, offset, access, 1);
      }
    }
  }

  [[nodiscard]] sharc::Memory::Views views(sharc::MemorySpace space) const {
    sharc::Memory::Views result;
    const auto index = static_cast<std::size_t>(space);
    result.read[index] = read.loan();
    result.write[index] = write.loan();
    return result;
  }
};

sharc::Memory borrow(fixture::Program &program, const Aliases &aliases, sharc::MemorySpace space) {
  const auto backing = program.memory(ram_id, aliases.read.bytes());
  return sharc::Memory::complete(backing.regions, aliases.views(space));
}

sharc::Memory lend(fixture::Program &program, const Aliases &aliases, sharc::MemorySpace space) {
  auto region = fixture::normal_region(0, word_count, ram_id);
  region.space = space;
  program.code.variants[0].layout.regions.push_back(region);
  return borrow(program, aliases, space);
}

void transfer(sharc::ExecutionMode mode,
              MemoryMode memory_mode,
              sharc::MemorySpace space,
              bool wrap) {
  const Aliases aliases;
  const auto dag = space == sharc::MemorySpace::program ? std::uint64_t{1} << 44 : 0;
  const std::array words{store_r0 | dag, load_r1 | dag};
  fixture::Program program(words);
  const auto memory = lend(program, aliases, space);
  const auto native = fixture::compile(program.code, mode, memory_mode);
  sharc::Core core(memory, mode);
  sharc::Registers registers;
  registers.pc = fixture::code_address;
  registers.mode1 = wrap ? simd : 0;
  registers.dag[0][space == sharc::MemorySpace::data ? 0U : 8U].index =
      wrap ? last_word : ram_address;
  registers.pe[0].data[0][0] = raw_value;
  registers.pe[1].data[0][0] = raw_value ^ lane_difference;
  fixture::install(core, native, registers);
  const auto snapshot = core.save();
  const auto result = core.run(words.size());
  check(fixture::completed(result, 2, 2) &&
            core.registers().pe[0].data[0][1] == (raw_value & ~extension_mask) &&
            aliases.read.get(wrap ? last_word : ram_address) == raw_value >> raw_shift,
        "split views lost the store/load alias or used the read-only read view for writing");
  if (wrap) {
    const auto expected = registers.pe[1].data[0][0] >> raw_shift;
    check(core.registers().pe[1].data[0][1] == (expected << raw_shift) &&
              aliases.read.get(0) == expected && aliases.read.tail_word() == expected,
          "paired transfer lost wrapped storage or complementary lane");
  }
  const Aliases replacement;
  core.set_memory(borrow(program, replacement, space));
  core.restore(snapshot);
  registers.pe[0].data[0][0] ^= lane_difference;
  fixture::install(core, native, registers);
  check(!core.run(words.size()).has_fault &&
            replacement.read.get(wrap ? last_word : ram_address) ==
                registers.pe[0].data[0][0] >> raw_shift &&
            core.registers().pe[0].data[0][1] == (registers.pe[0].data[0][0] & ~extension_mask),
        "rebinding or snapshot restore retained an obsolete view pointer");
}

void denied_write(sharc::ExecutionMode mode, MemoryMode memory_mode, sharc::MemorySpace space) {
  const Aliases aliases;
  aliases.write.protect(ram_address);
  const auto dag = space == sharc::MemorySpace::program ? std::uint64_t{1} << 44 : 0;
  const std::array words{store_r0 | dag};
  fixture::Program program(words);
  const auto memory = lend(program, aliases, space);
  program.code.variants[0].layout.regions.back().writable = false;
  const auto native = fixture::compile(program.code, mode, memory_mode);
  sharc::Core core(memory, mode);
  sharc::Registers registers;
  registers.pc = fixture::code_address;
  registers.dag[0][space == sharc::MemorySpace::data ? 0U : 8U].index = ram_address;
  fixture::install(core, native, registers);
  const auto result = core.run(1);
  check(result.has_fault && result.fault.reason == sharc::FaultReason::access_fault &&
            result.fault.address == fixture::code_address && result.progress == sharc::Progress{},
        "split write-view fault lost its PC or retired a denied instruction");
  if (memory_mode == MemoryMode::direct) {
    check(result.fault.detail.contains("host write") &&
              result.fault.detail.contains("word address " + std::to_string(ram_address)) &&
              sharc_test_mask_restored() != 0,
          "split write-view host fault lost attribution or mask");
  } else {
    check(core.registers() == registers && aliases.read.get(ram_address) == 0,
          "checked write fault changed registers or backing");
  }
}

void geometry() {
  const Aliases aliases;
  const auto good = aliases.views(sharc::MemorySpace::data);
  auto no_tail = good;
  no_tail.read[1].mirrored_tail = {};
  no_tail.write[1].mirrored_tail = {};
  const auto copied = sharc::Memory::complete({}, no_tail);
  no_tail.read[1] = {};
  check(copied.views().read[1].data.data() == aliases.read.bytes().data() &&
            copied.views().read[1].mirrored_tail.empty(),
        "factory retained the descriptor reference or required an unused mirrored tail");
  const auto reject = [&](const sharc::Memory::Views &views, std::string_view message) {
    fixture::throws<std::invalid_argument>([&] { (void)sharc::Memory::complete({}, views); },
                                           message);
  };
  auto changed = good;
  changed.write[1] = {};
  reject(changed, "missing write view accepted");
  changed = good;
  changed.read[1].data = changed.read[1].data.first(view_bytes - 1);
  reject(changed, "short complete view accepted");
  changed = good;
  changed.read[1].mirrored_tail = aliases.read.bytes().first(aliases.page);
  reject(changed, "noncontiguous mirrored tail accepted");
  changed.read[1].mirrored_tail = good.read[1].mirrored_tail.first(aliases.page - 1);
  reject(changed, "short mirrored tail accepted");
  const std::array words{store_r0};
  fixture::Program program(words);
  auto memory = lend(program, aliases, sharc::MemorySpace::data);
  memory.complete_views = changed;
  const auto native = sharc::Core::compile(program.code);
  sharc::Core core(memory);
  fixture::throws<sharc::PreparationError>(
      [&] { (void)core.install(native, fixture::Program::variant_id); },
      "install trusted corrupted view geometry");

  program.code.variants[0].layout.ordinary_regions = true;
  const auto ordinary = sharc::Core::compile(program.code);
  sharc::Core split_views(borrow(program, aliases, sharc::MemorySpace::data));
  fixture::throws<sharc::PreparationError>(
      [&] { (void)split_views.install(ordinary, fixture::Program::variant_id); },
      "ordinary native accesses accepted a complete-view binding");
}
} // namespace

int main() {
  return fixture::run_test([] {
    const fixture::ConsumerHandler handler;
    geometry();
    for (const auto mode : fixture::numeric_modes) {
      for (const auto space : {sharc::MemorySpace::data, sharc::MemorySpace::program}) {
        for (const auto memory : {MemoryMode::exact, MemoryMode::direct}) {
          denied_write(mode, memory, space);
          transfer(mode, memory, space, false);
          transfer(mode, memory, space, true);
        }
      }
    }
    std::cout << "complete memory views passed\n";
  });
}
