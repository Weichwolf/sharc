// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "native.hpp"

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace sharc::detail {
struct HostView {
  std::uintptr_t address{};
  std::uint64_t size{};
  MemorySpace space{};
  std::uint32_t stride{};
  bool writing{};
};

[[nodiscard]] std::vector<HostView> host_views(std::span<const RegionLayout> layout,
                                               std::span<void *const> storage,
                                               const Memory::Views &views);

struct HostFault {
  std::uintptr_t address{}, pc{};
  bool writing{};
};

class NativeCall {
public:
  void bind(std::span<const NativeSymbols::TextRange> text,
            std::span<const HostView> views) noexcept;

  [[nodiscard]] bool invoke(NativeEntry entry, void **pointers, std::uint64_t *control) noexcept;
  [[nodiscard]] HostFault fault() const noexcept;
  [[nodiscard]] static bool
  claim(std::uintptr_t address, bool writing, std::uintptr_t native_pc) noexcept;

private:
  std::span<const NativeSymbols::TextRange> text_;
  std::span<const HostView> views_;
  void *landing_{};
  std::atomic<std::uintptr_t> address_;
  std::atomic<std::uintptr_t> pc_;
  std::atomic<bool> writing_;
};
} // namespace sharc::detail
