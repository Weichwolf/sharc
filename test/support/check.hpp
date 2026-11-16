// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace fixture {
inline void check(bool condition, std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

template <class Exception, class Function>
void throws(Function function, std::string_view message) {
  try {
    function();
  } catch (const Exception &) { return; }
  check(false, message);
}
} // namespace fixture
