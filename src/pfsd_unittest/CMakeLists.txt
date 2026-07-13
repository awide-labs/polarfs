# Copyright (c) 2025, Tantor Labs
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

include_directories(
    ${PROJECT_SOURCE_DIR}/src/pfsd/
    ${PROJECT_SOURCE_DIR}/src/pfs_core/
    ${PROJECT_SOURCE_DIR}/src/pfs_sdk/
    ${PROJECT_SOURCE_DIR}/src/trace/include/
    ${PROJECT_SOURCE_DIR}/src/
    ${PROJECT_SOURCE_DIR}/
)

link_directories(
    ${LIBRARY_OUTPUT_PATH}
)

find_package(GTest REQUIRED)

add_executable(pfsd_filetest
    pfsd_testenv.cc
    pfsd_unittest.cc
    pfsd_filetest.cc
)

target_link_libraries(pfsd_filetest
    pfsd
    gtest
)

add_executable(pfsd_disconnect_test
    pfsd_disconnect_test.cc
)

target_link_libraries(pfsd_disconnect_test
    pfsd
    gtest
)

add_executable(pfsd_zc_test
    pfsd_testenv.cc
    pfsd_unittest.cc
    pfsd_zc_test.cc
)

target_link_libraries(pfsd_zc_test
    pfsd
    gtest
)

add_executable(pfsd_proto_test
    pfsd_proto_test.cc
)

# Build this single-TU test without LTO. Ubuntu's GCC 15 -flto link drops the
# weak COMDAT defs of some header-inline libstdc++/BufReader helpers (e.g.
# std::vector<uint8_t>::~vector, basic_string::_S_copy, BufReader::readLE<u64>)
# during link-time recompilation while keeping references to them from .cold
# clones inside libpfsd.a -- the objects define the symbols, but the -flto link
# loses them.
set_source_files_properties(pfsd_proto_test.cc
    PROPERTIES COMPILE_OPTIONS "-fno-lto"
)

target_link_libraries(pfsd_proto_test
    pfsd
    gtest
)

add_executable(pfsd_cachetest
    dcache_test_rw_ro_sync.cc
)

target_link_libraries(pfsd_cachetest
    pfs
)

add_executable(pfsd_perftest
    dcache_test_performance.cc
)

target_link_libraries(pfsd_perftest
    pfs
)

add_executable(pfs_lease_hold
    pfs_lease_hold.cc
)

target_link_libraries(pfs_lease_hold
    pfs
)

add_executable(pfs_hole_bench
    pfs_hole_bench.cc
)

target_link_libraries(pfs_hole_bench
    pfs
)

add_executable(pfsd_ebusy_test
    pfsd_ebusy_test.cc
)

target_link_libraries(pfsd_ebusy_test
    pfsd
)

add_executable(pfs_inode_lru_test
    pfs_inode_lru_test.cc
)

target_link_libraries(pfs_inode_lru_test
    pfs
)
