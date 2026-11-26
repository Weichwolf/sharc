// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "check.hpp"
#include "signals.h"

#include <cstdint>
#include <sharc/core.hpp>

namespace fixture {
class ConsumerHandler {
public:
  ConsumerHandler() {
    check(sharc_test_signals_begin(claim) == 0, "could not install consumer fault handler");
  }

  ~ConsumerHandler() { sharc_test_signals_end(); }

  ConsumerHandler(const ConsumerHandler &) = delete;
  ConsumerHandler &operator=(const ConsumerHandler &) = delete;

private:
  static int claim(const void *address, int writing, unsigned long pc) noexcept {
    return static_cast<int>(
        sharc::Core::host_fault(reinterpret_cast<std::uintptr_t>(address), writing != 0, pc));
  }
};
} // namespace fixture
