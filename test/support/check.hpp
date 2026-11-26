// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <exception>
#include <iostream>
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

template <class Function> int run_test(Function function) {
  try {
    function();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
} // namespace fixture
