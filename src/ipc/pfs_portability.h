#pragma once

#include <algorithm>
#include <chrono>

#ifndef likely
#define likely(c) __builtin_expect(!!(c), 1)
#endif
#ifndef unlikely
#define unlikely(c) __builtin_expect(!!(c), 0)
#endif

namespace pfsutil {

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
#if defined(__ARM_ARCH) && __ARM_ARCH >= 9
  __asm__ __volatile__("sb" ::: "memory");
#else
  __asm__ __volatile__("isb" ::: "memory");
#endif
#elif defined(__arm__) && !(__ARM_ARCH < 7)
  __asm__ __volatile__("yield" ::: "memory");
#elif defined(__powerpc64__)
  __asm__ __volatile__("or 27,27,27" ::: "memory");
#endif
}

enum class spin_result {
  success, // condition passed
  timeout, // exceeded deadline
  advance, // exceeded spin_max
};

// Behaviour mirrors folly::detail::spin_pause_until exactly, including
// the spin_max==0 short-circuit, the deadline==min short-circuit, and the
// backward-clock-discontinuity guard. spin_max is taken directly as a
// duration rather than wrapped in a WaitOptions object.
template <typename Clock, typename Duration, typename F>
spin_result spin_pause_until(
    std::chrono::time_point<Clock, Duration> const& deadline,
    std::chrono::nanoseconds spin_max,
    F f) {
  if (spin_max <= std::chrono::nanoseconds::zero()) {
    return spin_result::advance;
  }
  if (f()) {
    return spin_result::success;
  }
  constexpr auto min = std::chrono::time_point<Clock, Duration>::min();
  if (deadline == min) {
    return spin_result::timeout;
  }
  auto tbegin = Clock::now();
  while (true) {
    if (f()) {
      return spin_result::success;
    }
    auto const tnow = Clock::now();
    if (tnow >= deadline) {
      return spin_result::timeout;
    }
    tbegin = std::min(tbegin, tnow);
    if (tnow >= tbegin + spin_max) {
      return spin_result::advance;
    }
    cpu_relax();
  }
}

// Mirrors folly::WaitOptions::Defaults::spin_max.
constexpr std::chrono::nanoseconds kDefaultSpinMax =
    std::chrono::microseconds(2);

} // namespace pfsutil
