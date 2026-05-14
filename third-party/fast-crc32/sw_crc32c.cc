// Portable software CRC-32C (Castagnoli, polynomial 0x1EDC6F41) — slice-by-8.
// Used as the dispatch fallback when no vectorized variant is available
// (currently: non-x86_64 builds; on x86_64 the AVX-512 / SSE4.2 variants
// always win the dispatch).
//
// Convention matches the corsix/fast-crc32 generator output: the running
// residual is inverted at entry and exit. Pass 0 for a fresh checksum.

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

// CRC-32C polynomial, reflected.
constexpr std::uint32_t kPoly = 0x82F63B78u;

using Table = std::array<std::array<std::uint32_t, 256>, 8>;

constexpr Table make_tables() {
  Table t{};
  for (int b = 0; b < 256; ++b) {
    std::uint32_t c = static_cast<std::uint32_t>(b);
    for (int k = 0; k < 8; ++k) {
      c = (c >> 1) ^ (kPoly & -(c & 1));
    }
    t[0][b] = c;
  }
  for (int b = 0; b < 256; ++b) {
    std::uint32_t c = t[0][b];
    for (int s = 1; s < 8; ++s) {
      c = t[0][c & 0xFFu] ^ (c >> 8);
      t[s][b] = c;
    }
  }
  return t;
}

constexpr Table kTab = make_tables();

}  // namespace

extern "C" std::uint32_t pfs_fastcrc32c_sw(std::uint32_t crc,
                                           const char* buf,
                                           std::size_t len) {
  crc = ~crc;
  const auto* p = reinterpret_cast<const std::uint8_t*>(buf);
  while (len >= 8) {
    std::uint32_t lo = static_cast<std::uint32_t>(p[0]) |
                       (static_cast<std::uint32_t>(p[1]) << 8) |
                       (static_cast<std::uint32_t>(p[2]) << 16) |
                       (static_cast<std::uint32_t>(p[3]) << 24);
    std::uint32_t hi = static_cast<std::uint32_t>(p[4]) |
                       (static_cast<std::uint32_t>(p[5]) << 8) |
                       (static_cast<std::uint32_t>(p[6]) << 16) |
                       (static_cast<std::uint32_t>(p[7]) << 24);
    lo ^= crc;
    crc = kTab[7][lo & 0xFFu] ^
          kTab[6][(lo >> 8) & 0xFFu] ^
          kTab[5][(lo >> 16) & 0xFFu] ^
          kTab[4][(lo >> 24) & 0xFFu] ^
          kTab[3][hi & 0xFFu] ^
          kTab[2][(hi >> 8) & 0xFFu] ^
          kTab[1][(hi >> 16) & 0xFFu] ^
          kTab[0][(hi >> 24) & 0xFFu];
    p += 8;
    len -= 8;
  }
  while (len--) {
    crc = kTab[0][(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
  }
  return ~crc;
}
