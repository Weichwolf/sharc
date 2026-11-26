// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "check.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace fixture {
inline constexpr auto word_count = std::uint64_t{1} << 32;
inline constexpr auto word_space_bytes = std::size_t{1} << 34;

inline std::size_t page_size() {
  const auto bytes = sysconf(_SC_PAGESIZE);
  check(bytes > 0, "invalid host page size");
  return static_cast<std::size_t>(bytes);
}

class Mapping {
public:
  explicit Mapping(std::size_t size, int protection, int fd = -1)
      : size_(size),
        data_(mmap(nullptr,
                   size,
                   protection,
                   MAP_PRIVATE | MAP_NORESERVE | (fd < 0 ? MAP_ANONYMOUS : 0),
                   fd,
                   0)) {
    check(data_ != MAP_FAILED, "could not reserve word space");
  }

  ~Mapping() { (void)munmap(data_, size_); }

  Mapping(const Mapping &) = delete;
  Mapping &operator=(const Mapping &) = delete;

  [[nodiscard]] std::span<std::byte> bytes() const {
    return {static_cast<std::byte *>(data_), size_};
  }

  void protect(std::size_t offset, std::size_t size, int protection) const {
    check(mprotect(bytes().data() + offset, size, protection) == 0, "could not protect word space");
  }

private:
  std::size_t size_;
  void *data_;
};

class Backing {
public:
  explicit Backing(std::size_t size) : fd_(memfd_create("sharc-test", MFD_CLOEXEC)) {
    if (fd_ < 0 || ftruncate(fd_, static_cast<off_t>(size)) != 0) {
      if (fd_ >= 0) { (void)close(fd_); }
      throw std::runtime_error("could not create word-space backing");
    }
  }

  ~Backing() { (void)close(fd_); }

  Backing(const Backing &) = delete;
  Backing &operator=(const Backing &) = delete;

  [[nodiscard]] int descriptor() const { return fd_; }

private:
  int fd_;
};

// Avoid LLVM 19 SLP truncating large pointer distances in the host test setup.
inline void put_word(volatile std::byte *data, std::uint32_t value) {
  for (unsigned byte = 0; byte < 4; ++byte) {
    data[byte] = static_cast<std::byte>(value >> (byte * 8));
  }
}

inline std::uint32_t get_word(const volatile std::byte *data) {
  std::uint32_t value{};
  for (unsigned byte = 0; byte < 4; ++byte) {
    value |= std::to_integer<std::uint32_t>(data[byte]) << (byte * 8);
  }
  return value;
}
} // namespace fixture
