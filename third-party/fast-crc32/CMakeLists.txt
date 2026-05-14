# CRC-32C runtime-dispatched library.
#
# We ship pre-generated corsix/fast-crc32 variants in generated/, plus a
# portable slice-by-8 software fallback and a C++ runtime dispatcher.
# Per-arch variants are picked at first call:
#   x86_64 : avx512_crc32c_v8s3x4 -> sse_crc32c_v8s3x3 -> SW
#   aarch64: neon_eor3_crc32c_v8s2x4e_s2x1 -> neon_crc32c_v3s4x2e_v2 -> SW
#
# Each generated TU exports a function literally named `crc32_impl`. We
# rename via -Dcrc32_impl=<unique-name> so all variants can coexist in the
# binary, and scope ISA flags per source so they don't leak.
#
# To regenerate the variants from upstream, run tools/regen-fastcrc32.sh
# from this directory.

set(_gen "${CMAKE_CURRENT_SOURCE_DIR}/generated")

set(_fastcrc_sources
  sw_crc32c.cc
  dispatch.cpp)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(_fc_sse_src    "${_gen}/sse_crc32c_v8s3x3.c")
  set(_fc_avx512_src "${_gen}/avx512_crc32c_v8s3x4.c")

  set_source_files_properties("${_fc_sse_src}" PROPERTIES
    COMPILE_OPTIONS "-msse4.2;-mpclmul"
    COMPILE_DEFINITIONS "crc32_impl=pfs_fastcrc32c_sse")

  set_source_files_properties("${_fc_avx512_src}" PROPERTIES
    COMPILE_OPTIONS "-mavx512f;-mavx512vl;-mpclmul;-msse4.2"
    COMPILE_DEFINITIONS "crc32_impl=pfs_fastcrc32c_avx512")

  list(APPEND _fastcrc_sources "${_fc_sse_src}" "${_fc_avx512_src}")

elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(_fc_neon_src      "${_gen}/neon_crc32c_v3s4x2e_v2.c")
  set(_fc_neon_eor3_src "${_gen}/neon_eor3_crc32c_v8s2x4e_s2x1.c")

  # Basic NEON variant: ARMv8 CRC32 + PMULL (crypto) intrinsics.
  set_source_files_properties("${_fc_neon_src}" PROPERTIES
    COMPILE_OPTIONS "-march=armv8-a+crc+crypto"
    COMPILE_DEFINITIONS "crc32_impl=pfs_fastcrc32c_neon")

  # EOR3 variant: additionally needs ARMv8.2-SHA3 (veor3q_u64).
  set_source_files_properties("${_fc_neon_eor3_src}" PROPERTIES
    COMPILE_OPTIONS "-march=armv8.2-a+crc+crypto+sha3"
    COMPILE_DEFINITIONS "crc32_impl=pfs_fastcrc32c_neon_eor3")

  list(APPEND _fastcrc_sources "${_fc_neon_src}" "${_fc_neon_eor3_src}")
endif()

add_library(fastcrc32 STATIC ${_fastcrc_sources})

target_include_directories(fastcrc32 PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

set_target_properties(fastcrc32 PROPERTIES POSITION_INDEPENDENT_CODE ON)
