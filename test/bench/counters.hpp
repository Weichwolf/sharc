// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cerrno>
#include <cstdint>
#include <linux/perf_event.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>

namespace bench {
struct Counts {
  std::uint64_t instructions{}, cycles{}, enabled{}, running{};
};

class Counters {
public:
  Counters() {
    constexpr std::array events{PERF_COUNT_HW_INSTRUCTIONS, PERF_COUNT_HW_CPU_CYCLES};
    for (std::size_t i = 0; i < events.size(); ++i) {
      perf_event_attr event{};
      event.size = sizeof(event);
      event.type = PERF_TYPE_HARDWARE;
      event.config = events[i];
      event.disabled = i == 0;
      event.pinned = i == 0;
      event.exclude_kernel = 1;
      event.exclude_hv = 1;
      event.read_format =
          PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
      const auto fd = syscall(SYS_perf_event_open, &event, 0, -1, fds_[0], PERF_FLAG_FD_CLOEXEC);
      if (fd < 0) {
        const int error = errno;
        close_all();
        throw std::system_error(error, std::generic_category(), "perf_event_open");
      }
      fds_[i] = static_cast<int>(fd);
    }
  }

  ~Counters() { close_all(); }

  Counters(const Counters &) = delete;
  Counters &operator=(const Counters &) = delete;
  Counters(Counters &&) = delete;
  Counters &operator=(Counters &&) = delete;

  void start() {
    control(PERF_EVENT_IOC_RESET);
    baseline_ = read(); // RESET does not reset the lifetime enabled/running times.
    control(PERF_EVENT_IOC_ENABLE);
  }

  Counts stop() {
    control(PERF_EVENT_IOC_DISABLE);
    const auto end = read();
    Counts result{.instructions = end.values[0],
                  .cycles = end.values[1],
                  .enabled = end.enabled - baseline_.enabled,
                  .running = end.running - baseline_.running};
    if (result.running == 0 || result.enabled != result.running || result.instructions == 0 ||
        result.cycles == 0) {
      throw std::runtime_error("hardware counter group did not run completely and unscaled");
    }
    return result;
  }

private:
  struct Group {
    std::uint64_t count{}, enabled{}, running{};
    std::array<std::uint64_t, 2> values{};
  };

  void close_all() noexcept {
    for (const auto fd : fds_) {
      if (fd >= 0) { (void)::close(fd); }
    }
  }

  void control(unsigned long operation) {
    if (ioctl(fds_[0], operation, PERF_IOC_FLAG_GROUP) != 0) {
      throw std::system_error(errno, std::generic_category(), "perf group control");
    }
  }

  [[nodiscard]] Group read() const {
    Group result;
    if (::read(fds_[0], &result, sizeof(result)) != sizeof(result) || result.count != fds_.size()) {
      throw std::runtime_error("incomplete hardware counter group read");
    }
    return result;
  }

  std::array<int, 2> fds_{-1, -1};
  Group baseline_;
};
} // namespace bench
