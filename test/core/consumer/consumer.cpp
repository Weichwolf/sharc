// SPDX-License-Identifier: GPL-3.0-only
#include <exception>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>

int main() try {
  sharc::Core core{sharc::Memory{}};
  const auto snapshot = core.save();
  core.restore(snapshot);
  if (sharc::Core::compiler_identity().empty()) { return 1; }
  return core.state().progress.instructions == 0 ? 0 : 1;
} catch (const std::exception &) { return 1; }
