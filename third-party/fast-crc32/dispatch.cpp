// Runtime dispatcher for fastcrc32::crc32c.
//
// Variants shipped:
//   x86_64:
//     - avx512_crc32c_v8s3x4 (AVX-512F + AVX-512VL + PCLMUL + SSE4.2)
//     - sse_crc32c_v8s3x3    (SSE4.2 + PCLMUL)
//   aarch64 (Linux):
//     - neon_eor3_crc32c_v8s2x4e_s2x1 (ARMv8.2 +crc +crypto +sha3)
//     - neon_crc32c_v3s4x2e_v2        (ARMv8   +crc +crypto)
//   any:
//     - portable slice-by-8 software fallback
//
// At first call we probe CPU features (__builtin_cpu_supports on x86_64,
// getauxval(AT_HWCAP) on Linux aarch64) and pick the best available impl.
// The selection is cached in a function-local static so subsequent calls
// pay only an already-initialized branch and an indirect call.

#include "fastcrc32/crc32c.h"

#include <cstddef>
#include <cstdint>

#if defined(__aarch64__) && defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

extern "C" {

// Each generated variant exports a function named `crc32_impl`; the build
// renames that symbol per variant via -Dcrc32_impl=<name> so they can all
// coexist in the same binary.

#if defined(__x86_64__)
std::uint32_t pfs_fastcrc32c_avx512(std::uint32_t crc,
                                    const char* buf,
                                    std::size_t len);
std::uint32_t pfs_fastcrc32c_sse(std::uint32_t crc,
                                 const char* buf,
                                 std::size_t len);
#endif

#if defined(__aarch64__)
std::uint32_t pfs_fastcrc32c_neon(std::uint32_t crc,
                                  const char* buf,
                                  std::size_t len);
std::uint32_t pfs_fastcrc32c_neon_eor3(std::uint32_t crc,
                                       const char* buf,
                                       std::size_t len);
#endif

std::uint32_t pfs_fastcrc32c_sw(std::uint32_t crc,
                                const char* buf,
                                std::size_t len);

}  // extern "C"

namespace fastcrc32 {

namespace {

using Fn = std::uint32_t (*)(std::uint32_t, const char*, std::size_t);

Fn select() {
#if defined(__x86_64__)
  __builtin_cpu_init();
  if (__builtin_cpu_supports("avx512f") &&
      __builtin_cpu_supports("avx512vl") &&
      __builtin_cpu_supports("pclmul")) {
    return &pfs_fastcrc32c_avx512;
  }
  if (__builtin_cpu_supports("sse4.2") &&
      __builtin_cpu_supports("pclmul")) {
    return &pfs_fastcrc32c_sse;
  }
#elif defined(__aarch64__) && defined(__linux__)
  // We use getauxval(AT_HWCAP) instead of __builtin_cpu_supports because
  // aarch64 support for the latter only landed in GCC 14 (2024) and recent
  // Clang — older toolchains in the wild won't compile it. getauxval has
  // been stable in glibc since 2.16 (2012) and the HWCAP bits are a kernel
  // ABI, so this works on every Linux/aarch64 toolchain we'd plausibly meet.
  const unsigned long hw = ::getauxval(AT_HWCAP);
  // Both NEON variants need the ARMv8 CRC32 + PMULL (crypto) extensions.
  // The eor3 variant additionally needs ARMv8.2-SHA3 for veor3q_u64.
  constexpr unsigned long kNeon = HWCAP_CRC32 | HWCAP_PMULL;
  if ((hw & (kNeon | HWCAP_SHA3)) == (kNeon | HWCAP_SHA3)) {
    return &pfs_fastcrc32c_neon_eor3;
  }
  if ((hw & kNeon) == kNeon) {
    return &pfs_fastcrc32c_neon;
  }
#endif
  return &pfs_fastcrc32c_sw;
}

}  // namespace

std::uint32_t crc32c(std::uint32_t crc, const void* buf, std::size_t len) {
  static const Fn fn = select();
  return fn(crc, static_cast<const char*>(buf), len);
}

}  // namespace fastcrc32
