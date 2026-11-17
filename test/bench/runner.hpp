// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../support/check.hpp"
#include "counters.hpp"

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
#include <string_view>
#include <sys/mman.h>

namespace bench {
using Clock = std::chrono::steady_clock;

class WordSpace {
public:
  static constexpr auto bytes = std::size_t{1} << 34;
  static constexpr unsigned word_bytes = 4;

  WordSpace()
      : base_(mmap(nullptr,
                   bytes,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                   -1,
                   0)) {
    if (base_ == MAP_FAILED) {
      throw std::runtime_error("benchmark complete-space reservation failed");
    }
  }

  ~WordSpace() { (void)munmap(base_, bytes); }

  WordSpace(const WordSpace &) = delete;
  WordSpace &operator=(const WordSpace &) = delete;
  WordSpace(WordSpace &&) = delete;
  WordSpace &operator=(WordSpace &&) = delete;

  void protect_read_only() const {
    fixture::check(mprotect(base_, bytes, PROT_READ) == 0, "benchmark read protection failed");
  }

  [[nodiscard]] std::span<std::byte> all() const {
    return {static_cast<std::byte *>(base_), bytes};
  }

  [[nodiscard]] std::span<std::byte> at(std::uint32_t address, std::size_t size) const {
    return all().subspan(std::size_t{address} * word_bytes, size);
  }

private:
  void *base_;
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

struct Kernel {
  std::string_view name;
  sharc::ExecutionMode mode;
  sharc::CompileOptions::MemoryMode memory_mode;
  sharc::Progress progress;
  unsigned prepared_instructions;
};

template <class Reset, class Check, class Reference>
void sample(Kernel kernel,
            const sharc::NativeImage &native,
            sharc::Core &core,
            bool measure,
            Reset reset,
            Check check,
            Reference reference) {
  constexpr unsigned repetitions = 3;
  check(core.run(kernel.progress.instructions));
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
            << ",\"guest_base_cycles\":" << kernel.progress.base_cycles << ",\"runs\":[";
  std::unique_ptr<Counters> counters;
  if (measure) { counters = std::make_unique<Counters>(); }
  for (unsigned repeat = 0; repeat < (measure ? repetitions : 1); ++repeat) {
    const auto native_time = reference();
    reset();
    if (counters) { counters->start(); }
    const auto start = Clock::now();
    const auto result = core.run(kernel.progress.instructions);
    const auto end = Clock::now();
    const auto counts = counters ? counters->stop() : Counts{};
    check(result);
    if (repeat != 0) { std::cout << ','; }
    std::cout << "{\"nanoseconds\":" << nanoseconds(start, end)
              << ",\"reference_nanoseconds\":" << native_time << ",\"host_instructions\":";
    if (measure) {
      std::cout << counts.instructions;
    } else {
      std::cout << "null";
    }
    std::cout << ",\"host_cycles\":";
    if (measure) {
      std::cout << counts.cycles;
    } else {
      std::cout << "null";
    }
    std::cout << ",\"enabled\":" << counts.enabled << ",\"running\":" << counts.running << '}';
  }
  std::cout << "]}\n";
}
} // namespace bench
