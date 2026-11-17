// SPDX-License-Identifier: GPL-3.0-only
#include "escape.hpp"

#include "landing.h"
#include "native.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <span>
#include <vector>

namespace sharc::detail {
namespace {
static_assert(std::atomic<NativeCall *>::is_always_lock_free &&
              std::atomic<std::uintptr_t>::is_always_lock_free &&
              std::atomic<bool>::is_always_lock_free);

// A handler on a thread which never ran a Core must not allocate dynamic TLS.
#if defined(__ELF__)
[[gnu::tls_model("initial-exec")]]
#endif
constinit thread_local std::atomic<NativeCall *> active{nullptr};

template <class Ranges> bool contains(const Ranges &ranges, std::uintptr_t address) noexcept {
  return std::ranges::any_of(ranges, [address](const auto &range) {
    return address >= range.address && address - range.address < range.size;
  });
}
} // namespace

std::vector<HostView> host_views(std::span<const RegionLayout> layout,
                                 std::span<void *const> storage,
                                 const Memory::Views &views) {
  std::vector<HostView> result;
  constexpr auto words = std::uint64_t{1} << 32;
  constexpr unsigned word_bytes = 4;
  for (std::size_t region = 0; region < layout.size(); ++region) {
    const auto &entry = layout[region];
    if (!entry.initialized || entry.view != MemoryView::normal_word || entry.count != words) {
      continue;
    }
    for (std::size_t direction = 0; direction < 2; ++direction) {
      const auto *base = storage[region + (direction * layout.size())];
      auto size = ((entry.count - 1) * entry.byte_stride) + word_bytes;
      const auto space = static_cast<std::size_t>(entry.space);
      const auto &loans = direction == 0 ? views.read : views.write;
      if (space < loans.size() && entry.byte_stride == word_bytes &&
          base == loans[space].data.data()) {
        size += loans[space].mirrored_tail.size();
      }
      result.push_back({.address = reinterpret_cast<std::uintptr_t>(base),
                        .size = size,
                        .space = entry.space,
                        .stride = entry.byte_stride,
                        .writing = direction != 0});
    }
  }
  return result;
}

void NativeCall::bind(std::span<const NativeSymbols::TextRange> text,
                      std::span<const HostView> views) noexcept {
  text_ = text;
  views_ = views;
}

[[gnu::noinline]] bool
NativeCall::invoke(NativeEntry entry, void **pointers, std::uint64_t *control) noexcept {
  auto *previous = active.load(std::memory_order_relaxed);
  // The C landing frame contains no C++ objects. A generated-code fault cannot
  // reach this owner until the adapter has established its landing point.
  active.store(this, std::memory_order_release);
  const bool completed = sharc_native_invoke(entry, pointers, control, &landing_) != 0;
  active.store(previous, std::memory_order_release);
  return completed;
}

HostFault NativeCall::fault() const noexcept {
  return {.address = address_.load(std::memory_order_relaxed),
          .pc = pc_.load(std::memory_order_relaxed),
          .writing = writing_.load(std::memory_order_relaxed)};
}

bool NativeCall::claim(std::uintptr_t address, bool writing, std::uintptr_t native_pc) noexcept {
  auto *call = active.load(std::memory_order_acquire);
  if (call == nullptr || !contains(call->text_, native_pc) || !contains(call->views_, address)) {
    return false;
  }
  call->address_.store(address, std::memory_order_relaxed);
  call->pc_.store(native_pc, std::memory_order_relaxed);
  call->writing_.store(writing, std::memory_order_relaxed);
  sharc_native_escape(call->landing_);
}
} // namespace sharc::detail
