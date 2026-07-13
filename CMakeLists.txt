# Copyright (c) 2017-2021, Alibaba Group Holding Limited
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.16)
project(pfsd CXX C ASM)

message("compile pfsd for PostgreSQL")

set(CMAKE_BUILD_TYPE "Release")
#set(CMAKE_BUILD_TYPE "Debug")

include(${CMAKE_SOURCE_DIR}/cmake/version.cmake)

set(COMMON_FLAGS
  -fno-strict-aliasing
  #-fstack-protector
  #-fsanitize=address
  #-DEV_STANDALONE
  -Wall
  -Werror
  -Wno-deprecated-declarations
  -Wno-unused-result
  -Wno-format-overflow
  -Wno-misleading-indentation
  -Wno-format-truncation
  -Wno-stringop-truncation
  -Wno-int-in-bool-context
  -Wno-stringop-overflow
  -Wno-error=attributes
  -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
  -Wno-missing-declarations -fno-strict-aliasing
  -Wformat -Wformat-security -Wformat-nonliteral
  -fno-omit-frame-pointer
  -gdwarf-5
  -fPIC
  )

if(${CMAKE_HOST_SYSTEM_PROCESSOR} STREQUAL "x86_64")
    set(COMMON_FLAGS ${COMMON_FLAGS}  -march=x86-64-v3 -mtune=haswell -m64)
endif()

set(CMAKE_EXE_LINKER_FLAGS
  # Enable full RELRO - no lazy relocation (resolve everything at load time).
  # This allows the GOT to be made read-only early in the loading process.
  -Wl,-z,relro,-z,now,-z,noexecstack
  # Make the stack non-executable.
  # This is the default in most environments, but it doesn't hurt to set it explicitly.
  #-Wl,-z,noexecstack
)

set(COMMON_FLAGS ${COMMON_FLAGS}  -DPFS_DISK_IO_ONLY)

option(ENABLE_SANITIZERS "Enable sanitizers (address + UB)" OFF)

if(ENABLE_SANITIZERS)
    set(COMMON_FLAGS "${COMMON_FLAGS}" -fsanitize=address,undefined)
endif()

set(CXX_FLAGS
  ${COMMON_FLAGS}
  -std=c++20
  -Wno-deprecated
  -Wno-unused-local-typedefs
  -Wno-sign-compare
  -fpermissive
  -Wno-unused-variable
  -Wno-unused-function
)

set(C_FLAGS
  ${COMMON_FLAGS}
)

string(REPLACE ";" " " CMAKE_CXX_FLAGS "${CXX_FLAGS}")
string(REPLACE ";" " " CMAKE_C_FLAGS "${C_FLAGS}")

set(DEBUG_LEVEL 20)
set(RELEASE_LEVEL 40)

if(DEFINED ENV{POLAR_LOG_DEBUG_LEVEL})
	set(DEBUG_LEVEL $ENV{POLAR_LOG_DEBUG_LEVEL})
endif()
if(DEFINED ENV{POLAR_LOG_RELEASE_LEVEL})
	set(RELEASE_LEVEL $ENV{POLAR_LOG_RELEASE_LEVEL})
endif()

message("build log level release level ${RELEASE_LEVEL} debug level ${DEBUG_LEVEL}")
set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -ggdb -DDEBUG -DCHKSVR_LOG_LEVEL=${DEBUG_LEVEL}")
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O2 -finline-limit=100000 -DNDEBUG -DCHKSVR_LOG_LEVEL=${RELEASE_LEVEL}")
set(CMAKE_C_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -ggdb -DDEBUG -DCHKSVR_LOG_LEVEL=${DEBUG_LEVEL}")
set(CMAKE_C_FLAGS_RELEASE "$ENV{CXXFLAGS} -O2 -finline-limit=100000 -DNDEBUG -DCHKSVR_LOG_LEVEL=${RELEASE_LEVEL}")

# Link-time optimization (Release only): append -flto to Release compile and
# link lines. We intentionally do not use CMake IPO here: vendored Folly calls
# cmake_minimum_required(3.5), which leaves CMP0069 unset so IPO would warn
# and be ignored for Folly targets unless we patch upstream CMakeLists.
# Disable LTO with -DPFSD_ENABLE_LTO=OFF.
#
# We also pass -ffat-lto-objects (GCC only) so that each .o carries BOTH GIMPLE
# bytecode AND a regular optimized .text section. Consumers that link libpfsd
# without -flto then use the regular code path and skip the multi-second LTO
# codegen pass that the linker plugin would otherwise trigger for every consumer
# binary. Clang has no exact fat-LTO equivalent, so this only applies to GCC.
option(PFSD_ENABLE_LTO "Enable link-time optimization for Release builds" ON)
if(PFSD_ENABLE_LTO)
  set(_pfsd_lto_compile_extra "")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_pfsd_lto_flag "-flto=auto")
    set(_pfsd_lto_compile_extra "-ffat-lto-objects")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
    set(_pfsd_lto_flag "-flto=thin")
  else()
    set(_pfsd_lto_flag "")
  endif()
  if(_pfsd_lto_flag)
    include(CheckCXXCompilerFlag)
    include(CheckCCompilerFlag)
    check_cxx_compiler_flag("${_pfsd_lto_flag}" _pfsd_cxx_lto_ok)
    check_c_compiler_flag("${_pfsd_lto_flag}" _pfsd_c_lto_ok)
    if(_pfsd_cxx_lto_ok AND _pfsd_c_lto_ok)
      # Probe -ffat-lto-objects independently: very old GCCs accept -flto
      # but not -ffat-lto-objects. If unsupported, drop it silently.
      if(_pfsd_lto_compile_extra)
        check_cxx_compiler_flag("${_pfsd_lto_compile_extra}" _pfsd_cxx_fat_ok)
        check_c_compiler_flag("${_pfsd_lto_compile_extra}" _pfsd_c_fat_ok)
        if(NOT (_pfsd_cxx_fat_ok AND _pfsd_c_fat_ok))
          set(_pfsd_lto_compile_extra "")
        endif()
      endif()
      string(APPEND CMAKE_CXX_FLAGS_RELEASE " ${_pfsd_lto_flag}")
      string(APPEND CMAKE_C_FLAGS_RELEASE " ${_pfsd_lto_flag}")
      if(_pfsd_lto_compile_extra)
        string(APPEND CMAKE_CXX_FLAGS_RELEASE " ${_pfsd_lto_compile_extra}")
        string(APPEND CMAKE_C_FLAGS_RELEASE " ${_pfsd_lto_compile_extra}")
      endif()
      string(APPEND CMAKE_EXE_LINKER_FLAGS " ${_pfsd_lto_flag}")
      string(APPEND CMAKE_SHARED_LINKER_FLAGS " ${_pfsd_lto_flag}")
      # Do not append to CMAKE_STATIC_LINKER_FLAGS: that is passed to `ar`
      # when creating .a files; -flto is not an ar option and breaks EL9 with
      # "two different operation options". LTO comes from compile flags on .o.
      if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # g++ -print-prog-name=ar is often plain "ar" (not gcc-ar); LTO bytecode
        # objects need gcc-ar / gcc-ranlib. Prefer tools next to the compiler,
        # then /usr/bin (Rocky/Ubuntu layout).
        get_filename_component(_pfsd_cc_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set(_pfsd_gcc_ar "")
        foreach(_pfsd_ar_cand "${_pfsd_cc_dir}/gcc-ar" "/usr/bin/gcc-ar")
          if(EXISTS "${_pfsd_ar_cand}")
            set(_pfsd_gcc_ar "${_pfsd_ar_cand}")
            break()
          endif()
        endforeach()
        set(_pfsd_gcc_ranlib "")
        foreach(_pfsd_rl_cand "${_pfsd_cc_dir}/gcc-ranlib" "/usr/bin/gcc-ranlib")
          if(EXISTS "${_pfsd_rl_cand}")
            set(_pfsd_gcc_ranlib "${_pfsd_rl_cand}")
            break()
          endif()
        endforeach()
        if(_pfsd_gcc_ar AND _pfsd_gcc_ranlib)
          set(CMAKE_CXX_COMPILER_AR "${_pfsd_gcc_ar}" CACHE FILEPATH "" FORCE)
          set(CMAKE_CXX_COMPILER_RANLIB "${_pfsd_gcc_ranlib}" CACHE FILEPATH "" FORCE)
          set(CMAKE_C_COMPILER_AR "${_pfsd_gcc_ar}" CACHE FILEPATH "" FORCE)
          set(CMAKE_C_COMPILER_RANLIB "${_pfsd_gcc_ranlib}" CACHE FILEPATH "" FORCE)
        endif()
      endif()
      if(_pfsd_lto_compile_extra)
        message(STATUS "LTO enabled (${_pfsd_lto_flag} ${_pfsd_lto_compile_extra} on Release compile; ${_pfsd_lto_flag} on exe/shared link)")
      else()
        message(STATUS "LTO enabled (${_pfsd_lto_flag} on Release compile; exe/shared link)")
      endif()
    else()
      message(WARNING "LTO flag ${_pfsd_lto_flag} not accepted by C or C++ compiler")
    endif()
  else()
    message(WARNING "LTO is not configured for compiler ID ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)

string(TOUPPER ${CMAKE_BUILD_TYPE} BUILD_TYPE)
message(STATUS "CXX_FLAGS = " ${CMAKE_CXX_FLAGS} " " ${CMAKE_CXX_FLAGS_${BUILD_TYPE}})
message(STATUS "C_FLAGS = " ${CMAKE_C_FLAGS} " " ${CMAKE_C_FLAGS_${BUILD_TYPE}})

execute_process(COMMAND mkdir -p ${PROJECT_SOURCE_DIR}/include)
execute_process(COMMAND cp ${PROJECT_SOURCE_DIR}/src/pfs_sdk/pfsd_sdk.h ${PROJECT_SOURCE_DIR}/include/)
execute_process(COMMAND cp ${PROJECT_SOURCE_DIR}/src/pfs_core/pfs_api.h ${PROJECT_SOURCE_DIR}/include/)

include(${CMAKE_SOURCE_DIR}/cmake/deps.cmake)

add_subdirectory(third-party/fast-crc32)
add_subdirectory(third-party/zlog)
add_subdirectory(src)
