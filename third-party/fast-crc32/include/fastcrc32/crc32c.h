/*
 * SSE4.2 + PCLMUL CRC32C (Castagnoli polynomial), variant v8s3x3.
 * Backed by https://github.com/corsix/fast-crc32 (vendored at
 * third-party/fast-crc32/, generated at build time). MIT or zlib licensed.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace fastcrc32 {

// CRC-32C (Castagnoli, polynomial 0x1EDC6F41).
// Pass 0 for `crc` to start a new checksum.
// Requires x86_64 with SSE4.2 and PCLMUL (-march=x86-64-v2 + pclmul or higher).
uint32_t crc32c(uint32_t crc, const void* buf, std::size_t len);

} // namespace fastcrc32
