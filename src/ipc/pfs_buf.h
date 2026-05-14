/*
 * Tiny replacements for folly::io::Cursor / folly::io::Appender. Operate on
 * a raw, contiguous byte range; throw std::out_of_range on overrun. Wire
 * format is little-endian regardless of host endianness.
 */
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace pfsutil {

namespace detail {

template <typename T>
constexpr T host_to_le(T v) noexcept {
  static_assert(std::is_trivially_copyable_v<T>);
  if constexpr (std::endian::native == std::endian::little) {
    return v;
  } else {
    if constexpr (sizeof(T) == 1) {
      return v;
    } else if constexpr (sizeof(T) == 2) {
      return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(v)));
    } else if constexpr (sizeof(T) == 4) {
      return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(v)));
    } else if constexpr (sizeof(T) == 8) {
      return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(v)));
    } else {
      static_assert(sizeof(T) == 0, "unsupported width");
    }
  }
}

template <typename T>
constexpr T le_to_host(T v) noexcept {
  return host_to_le(v); // bswap is its own inverse
}

} // namespace detail

class BufReader {
public:
  BufReader(const uint8_t* data, size_t len) noexcept
      : begin_(data), pos_(data), end_(data + len) {}

  size_t position() const noexcept {
    return static_cast<size_t>(pos_ - begin_);
  }
  size_t remaining() const noexcept {
    return static_cast<size_t>(end_ - pos_);
  }

  template <typename T>
  T readLE() {
    static_assert(std::is_trivially_copyable_v<T>);
    if (remaining() < sizeof(T)) {
      throw std::out_of_range("BufReader::readLE: short read");
    }
    T raw;
    std::memcpy(&raw, pos_, sizeof(T));
    pos_ += sizeof(T);
    return detail::le_to_host(raw);
  }

  std::string readFixedString(size_t n) {
    if (remaining() < n) {
      throw std::out_of_range("BufReader::readFixedString: short read");
    }
    std::string s(reinterpret_cast<const char*>(pos_), n);
    pos_ += n;
    return s;
  }

  void pull(void* dst, size_t n) {
    if (remaining() < n) {
      throw std::out_of_range("BufReader::pull: short read");
    }
    std::memcpy(dst, pos_, n);
    pos_ += n;
  }

private:
  const uint8_t* begin_;
  const uint8_t* pos_;
  const uint8_t* end_;
};

class BufWriter {
public:
  BufWriter(uint8_t* dst, size_t cap) noexcept
      : begin_(dst), pos_(dst), end_(dst + cap) {}

  size_t bytesWritten() const noexcept {
    return static_cast<size_t>(pos_ - begin_);
  }
  size_t remaining() const noexcept {
    return static_cast<size_t>(end_ - pos_);
  }

  template <typename T>
  void writeLE(T v) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (remaining() < sizeof(T)) {
      throw std::out_of_range("BufWriter::writeLE: overflow");
    }
    T raw = detail::host_to_le(v);
    std::memcpy(pos_, &raw, sizeof(T));
    pos_ += sizeof(T);
  }

  void push(const void* src, size_t n) {
    if (remaining() < n) {
      throw std::out_of_range("BufWriter::push: overflow");
    }
    std::memcpy(pos_, src, n);
    pos_ += n;
  }

private:
  uint8_t* begin_;
  uint8_t* pos_;
  uint8_t* end_;
};

} // namespace pfsutil
