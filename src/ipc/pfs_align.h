#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace pfsutil {

constexpr std::size_t kCachelineSize = 64;

struct alignas(kCachelineSize) cacheline_align_t {};

constexpr bool is_pow2(std::size_t v) {
  return v && !(v & (v - 1));
}

constexpr std::uintptr_t align_ceil(std::uintptr_t x, std::size_t a) {
  assert(is_pow2(a));
  return (x + a - 1) & ~(static_cast<std::uintptr_t>(a) - 1);
}

template <typename T>
inline T *align_ceil(T *p, std::size_t a) {
  return reinterpret_cast<T *>(
      align_ceil(reinterpret_cast<std::uintptr_t>(p), a));
}

constexpr std::uintptr_t align_floor(std::uintptr_t x, std::size_t a) {
  assert(is_pow2(a));
  return x & ~(static_cast<std::uintptr_t>(a) - 1);
}

template <typename T>
inline T *align_floor(T *p, std::size_t a) {
  return reinterpret_cast<T *>(
      align_floor(reinterpret_cast<std::uintptr_t>(p), a));
}

} // namespace pfsutil
