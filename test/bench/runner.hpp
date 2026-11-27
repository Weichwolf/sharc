// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../support/check.hpp"
#include "../support/isa.hpp"
#include "../support/word_space.hpp"
#include "counters.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sched.h>
#include <sharc/core.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>

namespace bench {
using Clock = std::chrono::steady_clock;

class WordSpace {
public:
  void protect_read_only() const { mapping_.protect(0, fixture::word_space_bytes, PROT_READ); }

  [[nodiscard]] std::span<std::byte> all() const { return mapping_.bytes(); }

  [[nodiscard]] std::span<std::byte> at(std::uint32_t address, std::size_t size) const {
    return all().subspan(std::size_t{address} * 4, size);
  }

  void write_words(std::uint32_t address, std::span<const std::uint32_t> words) const {
    auto target = at(address, words.size_bytes());
    for (std::size_t i = 0; i < words.size(); ++i) {
      fixture::put_word(target.data() + (i * sizeof(std::uint32_t)), words[i]);
    }
  }

  void check_words(std::uint32_t address,
                   std::span<const std::uint32_t> words,
                   std::string_view message) const {
    const auto source = at(address, words.size_bytes());
    for (std::size_t i = 0; i < words.size(); ++i) {
      fixture::check(fixture::get_word(source.data() + (i * sizeof(std::uint32_t))) == words[i],
                     message);
    }
  }

  void check_guards(std::uint32_t before,
                    std::uint32_t after,
                    std::byte guard,
                    std::string_view message) const {
    for (const auto address : {before, after}) {
      fixture::check(std::ranges::all_of(at(address, sizeof(std::uint32_t)),
                                         [guard](std::byte byte) { return byte == guard; }),
                     message);
    }
  }

private:
  fixture::Mapping mapping_{fixture::word_space_bytes, PROT_READ | PROT_WRITE};
};

inline int pin_cpu() {
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  fixture::check(sched_getaffinity(0, sizeof(allowed), &allowed) == 0, "cannot read CPU affinity");
  const auto current = sched_getcpu();
  fixture::check(current >= 0 && CPU_ISSET(current, &allowed),
                 "current CPU is outside the allowed set");
  cpu_set_t selected;
  CPU_ZERO(&selected);
  CPU_SET(current, &selected);
  fixture::check(sched_setaffinity(0, sizeof(selected), &selected) == 0,
                 "cannot pin benchmark thread");
  return current;
}

inline std::uint64_t nanoseconds(Clock::time_point start, Clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

struct Options {
  bool measure{};
  sharc::CompileOptions::MemoryMode memory_mode{sharc::CompileOptions::MemoryMode::exact};
};

inline Options start(int argc, char **argv) {
  fixture::check(argc == 1 ||
                     ((argc == 2 || argc == 3) && (std::string_view(argv[1]) == "--measure" ||
                                                   std::string_view(argv[1]) == "--check")),
                 "usage: benchmark [--check|--measure [--direct]]");
  fixture::check(argc != 3 || std::string_view(argv[2]) == "--direct",
                 "expected --direct as the second argument");
  std::cout << std::setprecision(10);
  std::cout << R"({"platform":"Linux","cpu":)" << pin_cpu()
            << ",\"compiler\":" << std::quoted(__clang_version__)
            << ",\"compiler_identity\":" << std::quoted(sharc::Core::compiler_identity()) << "}\n";
  return {.measure = argc >= 2 && std::string_view(argv[1]) == "--measure",
          .memory_mode = argc == 3 ? sharc::CompileOptions::MemoryMode::direct
                                   : sharc::CompileOptions::MemoryMode::exact};
}

template <class Run> int run_modes(int argc, char **argv, Run run) {
  return fixture::run_test([&] {
    const auto options = start(argc, argv);
    for (const auto mode : fixture::numeric_modes) { run(mode, options); }
  });
}

inline std::string counter_value(bool measured, std::uint64_t value) {
  return measured ? std::to_string(value) : "null";
}

struct Kernel {
  std::string_view name;
  sharc::ExecutionMode mode;
  sharc::CompileOptions::MemoryMode memory_mode;
  sharc::Progress progress;
  unsigned prepared_instructions;
};

struct OwnerStats {
  std::uint64_t calls{}, horizons{}, requested{}, accepted{}, max_delay{}, total_delay{};
};

struct SampleRun {
  sharc::RunResult result;
  OwnerStats owner;
};

template <class Reset, class Check, class Reference, class Execute>
void sample_execution(Kernel kernel,
                      const sharc::NativeImage &native,
                      bool measure,
                      Reset reset,
                      Check check,
                      Reference reference,
                      Execute execute,
                      std::string_view schedule = {}) {
  constexpr unsigned repetitions = 3;
  check(execute());
  (void)reference();
  const auto report = native.report();
  std::cout << R"({"kernel":)" << std::quoted(kernel.name) << R"(,"mode":)"
            << std::quoted(kernel.mode == sharc::ExecutionMode::strict ? "Strict" : "FastFP32")
            << R"(,"memory_mode":)"
            << std::quoted(kernel.memory_mode == sharc::CompileOptions::MemoryMode::direct
                               ? "direct"
                               : "exact")
            << R"(,"identity":)" << std::quoted(report.identity)
            << ",\"preparation_seconds\":" << report.seconds
            << ",\"native_bytes\":" << report.native_bytes
            << ",\"prepared_instructions\":" << kernel.prepared_instructions
            << ",\"native_bytes_per_prepared_instruction\":"
            << static_cast<double>(report.native_bytes) / kernel.prepared_instructions
            << ",\"guest_instructions\":" << kernel.progress.instructions
            << ",\"guest_base_cycles\":" << kernel.progress.base_cycles;
  if (!schedule.empty()) { std::cout << ",\"schedule\":" << std::quoted(schedule); }
  std::cout << ",\"runs\":[";
  std::unique_ptr<Counters> counters;
  if (measure) { counters = std::make_unique<Counters>(); }
  for (unsigned repeat = 0; repeat < (measure ? repetitions : 1); ++repeat) {
    const auto native_time = reference();
    reset();
    if (counters) { counters->start(); }
    const auto start = Clock::now();
    const auto result = execute();
    const auto end = Clock::now();
    const auto counts = counters ? counters->stop() : Counts{};
    check(result);
    if (repeat != 0) { std::cout << ','; }
    std::cout << "{\"nanoseconds\":" << nanoseconds(start, end)
              << ",\"reference_nanoseconds\":" << native_time
              << ",\"host_instructions\":" << counter_value(measure, counts.instructions)
              << ",\"host_cycles\":" << counter_value(measure, counts.cycles)
              << ",\"enabled\":" << counts.enabled << ",\"running\":" << counts.running;
    if (!schedule.empty()) {
      const auto &owner = result.owner;
      std::cout << ",\"owner_calls\":" << owner.calls << ",\"owner_horizons\":" << owner.horizons
                << ",\"irq_requested\":" << owner.requested
                << ",\"irq_accepted\":" << owner.accepted
                << ",\"irq_max_delay\":" << owner.max_delay
                << ",\"irq_total_delay\":" << owner.total_delay;
    }
    std::cout << '}';
  }
  std::cout << "]}\n";
}

template <class Reset, class Check, class Reference>
void sample(Kernel kernel,
            const sharc::NativeImage &native,
            sharc::Core &core,
            bool measure,
            Reset reset,
            Check check,
            Reference reference) {
  sample_execution(
      kernel,
      native,
      measure,
      reset,
      [&](const SampleRun &run) { check(run.result); },
      reference,
      [&] { return SampleRun{.result = core.run(kernel.progress.instructions), .owner = {}}; });
}
} // namespace bench
