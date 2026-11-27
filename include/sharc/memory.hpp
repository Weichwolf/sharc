// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sharc {
enum class MemorySpace : std::uint8_t { program, data };
enum class MemoryView : std::uint8_t {
  short_word,
  normal_word,
  long_word,
  instruction,
  extended,
  packed_48 // Combined PX normal-address access, with its own six-byte layout.
};

// Callbacks execute synchronously on the Core owner. False means an access fault.
struct Io {
  void *owner{};
  bool (*read)(void *, MemorySpace, MemoryView, std::uint32_t, std::uint64_t &) noexcept {};
  bool (*write)(void *, MemorySpace, MemoryView, std::uint32_t, std::uint64_t) noexcept {};
};

// Several logical layouts may refer to the same physical storage id and bytes.
struct RegionLayout {
  MemorySpace space{MemorySpace::data};
  MemoryView view{MemoryView::normal_word};
  std::uint32_t first{};
  std::uint64_t count{};
  std::uint32_t storage_id{};
  std::uint64_t byte_offset{};
  std::uint32_t byte_stride{};
  bool readable{true};
  bool writable{true};
  bool initialized{};
};

// Overlapping physical bytes use one storage id; express logical aliases in layouts.
struct MemoryRegion {
  std::uint32_t storage_id{};
  std::span<std::byte> bytes;
  std::uint64_t generation{};
};

struct Memory {
  struct View {
    std::span<std::byte> data;
    std::span<std::byte> mirrored_tail;
  };

  // Indexed by MemorySpace. An empty pair leaves that space on ordinary regions.
  struct Views {
    std::array<View, 2> read, write;
  };

  [[nodiscard]] static Memory complete(std::span<const MemoryRegion> regions, const Views &views);

  [[nodiscard]] const Views &views() const noexcept { return complete_views; }

  std::vector<MemoryRegion> regions;
  Io io;
  Views complete_views{};
};
} // namespace sharc
