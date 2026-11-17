// SPDX-License-Identifier: GPL-3.0-only
#include "decode.hpp"
#include "native.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <sharc/core.hpp>
#include <sharc/memory.hpp>
#include <string_view>
#include <vector>

namespace sharc::detail {
namespace {
constexpr unsigned normal_bytes = 4, code_bytes = 6;
constexpr auto address_count = std::uint64_t{1} << 32;

bool overlaps(const RegionLayout &a, const RegionLayout &b) {
  return a.count != 0 && b.count != 0 && a.space == b.space &&
         std::uint64_t{a.first} < std::uint64_t{b.first} + b.count &&
         std::uint64_t{b.first} < std::uint64_t{a.first} + a.count;
}

std::uint64_t end_offset(const RegionLayout &layout, unsigned width) {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (layout.count == 0) { return layout.byte_offset; }
  if (layout.byte_offset > maximum - width || layout.byte_stride == 0 ||
      layout.count - 1 > (maximum - layout.byte_offset - width) / layout.byte_stride) {
    return maximum;
  }
  return layout.byte_offset + ((layout.count - 1) * layout.byte_stride) + width;
}

void validate_storage(const Memory &memory, const PreparedImage &image, std::uint32_t variant) {
  std::set<std::uint32_t> ids;
  for (std::size_t i = 0; i < memory.regions.size(); ++i) {
    const auto &a = memory.regions[i];
    if (!ids.insert(a.storage_id).second) {
      refuse(FaultReason::invalid_image, image.identity, variant, 0, "duplicate storage id");
    }
    if (a.bytes.empty()) { continue; }
    const auto a_start = reinterpret_cast<std::uintptr_t>(a.bytes.data());
    for (std::size_t j = i + 1; j < memory.regions.size(); ++j) {
      const auto &b = memory.regions[j];
      if (b.bytes.empty()) { continue; }
      const auto b_start = reinterpret_cast<std::uintptr_t>(b.bytes.data());
      const bool overlap = a_start <= b_start ? b_start - a_start < a.bytes.size()
                                              : a_start - b_start < b.bytes.size();
      if (overlap) {
        refuse(FaultReason::invalid_image,
               image.identity,
               variant,
               0,
               "overlapping host storage requires one canonical storage id");
      }
    }
  }
}

bool code_storage_declared(const CodeVariant &variant) {
  for (const auto &span : variant.spans) {
    if (span.instruction_set != InstructionSet::isa) { return false; }
    auto cursor = std::uint64_t{span.address};
    const auto end = cursor + (span.parcels.size() / 3);
    while (cursor < end) {
      auto next = cursor;
      for (const auto &layout : variant.layout.regions) {
        if (layout.space == MemorySpace::program && layout.view == MemoryView::instruction &&
            layout.first <= cursor) {
          next = std::max(next, std::uint64_t{layout.first} + layout.count);
        }
      }
      if (next == cursor) { return false; }
      cursor = std::min(next, end);
    }
  }
  return true;
}
} // namespace

void validate_memory_layout(const CodeVariant &variant, std::string_view identity) {
  const auto &regions = variant.layout.regions;
  for (std::size_t i = 0; i < regions.size(); ++i) {
    const auto &a = regions[i];
    if (a.count > address_count - a.first) {
      refuse(FaultReason::invalid_image,
             identity,
             variant.id,
             a.first,
             "memory layout exceeds address space");
    }
    if (a.view == MemoryView::instruction || a.count == 0) { continue; }
    for (std::size_t j = i + 1; j < regions.size(); ++j) {
      const auto &b = regions[j];
      if (b.view != MemoryView::instruction && overlaps(a, b)) {
        refuse(FaultReason::invalid_image,
               identity,
               variant.id,
               a.first,
               "ambiguous data memory views");
      }
    }
  }
}

std::vector<void *>
direct_memory(const PreparedImage &image, std::uint32_t variant_id, const Memory &memory) {
  const auto variant = std::ranges::find(image.code.variants, variant_id, &CodeVariant::id);
  if (variant == image.code.variants.end()) {
    refuse(FaultReason::invalid_image, image.identity, variant_id, 0, "unknown memory variant");
  }
  validate_memory_layout(*variant, image.identity);
  validate_storage(memory, image, variant_id);
  std::vector<void *> result(variant->layout.regions.size());
  for (std::size_t i = 0; i < result.size(); ++i) {
    const auto &layout = variant->layout.regions[i];
    if (!layout.initialized || layout.count == 0 || layout.view != MemoryView::normal_word) {
      continue;
    }
    const auto region =
        std::ranges::find(memory.regions, layout.storage_id, &MemoryRegion::storage_id);
    if (region == memory.regions.end() || layout.byte_stride < normal_bytes ||
        end_offset(layout, normal_bytes) > region->bytes.size()) {
      refuse(FaultReason::invalid_image,
             image.identity,
             variant_id,
             layout.first,
             "direct normal-word storage is missing, truncated or has an invalid stride");
    }
    result[i] = region->bytes.data() + static_cast<std::size_t>(layout.byte_offset);
  }
  return result;
}

bool aliases_code(const CodeVariant &variant, std::size_t region) {
  if (!code_storage_declared(variant)) { return true; }
  const auto &data = variant.layout.regions[region];
  const auto data_end = end_offset(data, normal_bytes);
  return std::ranges::any_of(variant.layout.regions, [&](const auto &code) {
    return code.view == MemoryView::instruction && code.count != 0 &&
           code.storage_id == data.storage_id && data.byte_offset < end_offset(code, code_bytes) &&
           code.byte_offset < data_end;
  });
}
} // namespace sharc::detail
