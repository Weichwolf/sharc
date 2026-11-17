// SPDX-License-Identifier: GPL-3.0-only
#include "escape.hpp"

#include "landing.h"
#include "native.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <sharc/core.hpp>
#include <span>

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
