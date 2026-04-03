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
    ${PROJECT_BINARY_DIR}/third-party/folly/
    ${PROJECT_SOURCE_DIR}/third-party/folly/
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
    folly
)

add_executable(pfsd_cachetest
    dcache_test_rw_ro_sync.cc
)

target_link_libraries(pfsd_cachetest
    pfs
    folly
)

add_executable(pfsd_perftest
    dcache_test_performance.cc
)

target_link_libraries(pfsd_perftest
    pfs
    folly
)

add_executable(pfs_lease_hold
    pfs_lease_hold.cc
)

target_link_libraries(pfs_lease_hold
    pfs
    folly
)

add_executable(pfs_hole_bench
    pfs_hole_bench.cc
)

target_link_libraries(pfs_hole_bench
    pfs
    folly
)

add_executable(pfs_inode_lru_test
    pfs_inode_lru_test.cc
)

target_link_libraries(pfs_inode_lru_test
    pfs
    folly
)
