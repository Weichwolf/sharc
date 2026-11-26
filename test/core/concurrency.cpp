// SPDX-License-Identifier: GPL-3.0-only
#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "../support/native_provider.hpp"

#include <algorithm>
#include <array>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <thread>
#include <utility>
#include <vector>

namespace {
using fixture::check;
using MemoryMode = sharc::CompileOptions::MemoryMode;
constexpr unsigned workers = 4, rounds = 2, byte_bits = 8;
constexpr std::uint32_t ram_id = 11, ram_address = 0x90000;
constexpr auto other_variant = std::numeric_limits<std::uint32_t>::max();

struct Result {
  std::vector<std::byte> artifact;
  std::vector<char> text;
  sharc::State state;
  std::array<std::byte, 4> memory;

  bool operator==(const Result &) const = default;
};

void synchronize(std::barrier<> *gate) {
  if (gate != nullptr) { gate->arrive_and_wait(); }
}

Result
prepare(unsigned seed, const sharc::CompileOptions &options, std::barrier<> *gate = nullptr) {
  constexpr std::uint32_t first = 0x1234;
  constexpr std::uint32_t second = 0x5678;
  constexpr std::uint32_t variant_delta = 0x1000;
  constexpr std::uint32_t other_storage = 2;
  constexpr std::uint64_t store_r2 = 0xa10200000000;
  const bool use_other = ((seed / workers) & 1) != 0;
  const auto expected = first + seed + second + (2 * seed) + (use_other ? variant_delta : 0);
  const std::array words{fixture::immediate(0, first + seed),
                         fixture::immediate(1, second + (2 * seed)),
                         fixture::add(2, 0, 1),
                         store_r2};
  fixture::Program program(words);
  const std::array other_words{
      fixture::immediate(0, first + seed + variant_delta), words[1], words[2], words[3]};
  fixture::Program other_program(other_words);
  program.code.variants[0].layout.regions.push_back(fixture::normal_region(ram_address, 1, ram_id));
  auto variant = other_program.code.variants[0];
  variant.id = other_variant;
  variant.layout.regions[0].storage_id = other_storage;
  variant.layout.regions.push_back(program.code.variants[0].layout.regions.back());
  program.code.variants.push_back(std::move(variant));
  synchronize(gate);
  const auto semantic = sharc::Core::lower(program.code, options);
  auto artifact = semantic.artifact();
  synchronize(gate);
  const auto loaded = sharc::Core::load(program.code, options, artifact);
  check(loaded.identity() == semantic.identity() &&
            std::ranges::equal(loaded.bitcode(), semantic.bitcode()) &&
            std::ranges::equal(loaded.artifact(), artifact),
        "loaded parallel artifact differs from lowering");
  auto provider = std::make_shared<fixture::Provider>();
  const auto symbols = fixture::link(loaded, provider, false);
  synchronize(gate);
  const auto native = sharc::Core::bind(loaded, symbols);
  std::array<std::byte, 4> ram{};
  auto memory = program.memory();
  memory.regions.push_back(
      {.storage_id = other_storage, .bytes = other_program.bytes, .generation = 1});
  memory.regions.push_back({.storage_id = ram_id, .bytes = ram, .generation = 1});
  sharc::Core core(memory, options.mode);
  sharc::Registers registers;
  registers.pc = fixture::code_address;
  registers.dag[0][0].index = ram_address;
  fixture::install(
      core, native, registers, use_other ? other_variant : fixture::Program::variant_id);
  const auto run = core.run(words.size());
  check(fixture::completed(run, words.size(), 4) &&
            core.registers().pe[0].data[0][2] == (std::uint64_t{expected} << byte_bits),
        "parallel installed image lost independently expected arithmetic or progress");
  for (unsigned byte = 0; byte < ram.size(); ++byte) {
    check(ram[byte] == static_cast<std::byte>(expected >> (byte * byte_bits)),
          "parallel installed image lost expected memory bytes");
  }
  return {.artifact = std::move(artifact),
          .text = std::move(provider->text),
          .state = core.state(),
          .memory = ram};
}

sharc::CompileOptions options(unsigned worker) {
  return {.mode =
              (worker & 1) == 0 ? sharc::ExecutionMode::strict : sharc::ExecutionMode::fast_fp32,
          .memory_mode = (worker & 2) == 0 ? MemoryMode::exact : MemoryMode::direct,
          .callbacks = {}};
}

void qualify() {
  std::array<Result, std::size_t{workers} * rounds> parallel;
  std::barrier gate(workers);
  std::array<std::exception_ptr, workers> failures;
  std::vector<std::jthread> pool;
  try {
    for (unsigned worker = 0; worker < workers; ++worker) {
      pool.emplace_back([&, worker] {
        try {
          for (unsigned round = 0; round < rounds; ++round) {
            const auto index = (round * workers) + worker;
            parallel[index] = prepare(index + 1, options(worker), &gate);
          }
        } catch (...) {
          failures[worker] = std::current_exception();
          gate.arrive_and_drop();
        }
      });
    }
  } catch (...) {
    for (auto missing = pool.size(); missing < workers; ++missing) { gate.arrive_and_drop(); }
    throw;
  }
  pool.clear(); // Join before examining each worker's private result slot.
  for (const auto &failure : failures) {
    if (failure) { std::rethrow_exception(failure); }
  }
  for (unsigned round = 0; round < rounds; ++round) {
    for (unsigned worker = 0; worker < workers; ++worker) {
      const auto index = (round * workers) + worker;
      const auto serial = prepare(index + 1, options(worker));
      check(parallel[index] == serial,
            "concurrent preparation changed artifact, native text, state or memory");
    }
  }
}
} // namespace

int main() {
  return fixture::run_test([] {
    qualify();
    std::cout << "concurrent preparation passed\n";
  });
}
