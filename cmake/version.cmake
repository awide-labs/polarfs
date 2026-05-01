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

function(parse_version_file version_file)
    if(EXISTS "${version_file}")
        file(STRINGS "${version_file}" version_lines)

        foreach(line IN LISTS version_lines)
            if(line MATCHES "^VERSION_MAJOR=[0-9]+$")
                string(REGEX REPLACE "^VERSION_MAJOR=([0-9]+)$" "\\1" VERSION_MAJOR "${line}")
                set(VERSION_MAJOR "${VERSION_MAJOR}" PARENT_SCOPE)
            elseif(line MATCHES "^VERSION_MINOR=[0-9]+$")
                string(REGEX REPLACE "^VERSION_MINOR=([0-9]+)$" "\\1" VERSION_MINOR "${line}")
                set(VERSION_MINOR "${VERSION_MINOR}" PARENT_SCOPE)
            elseif(line MATCHES "^VERSION_PATCH=[0-9]+$")
                string(REGEX REPLACE "^VERSION_PATCH=([0-9]+)$" "\\1" VERSION_PATCH "${line}")
                set(VERSION_PATCH "${VERSION_PATCH}" PARENT_SCOPE)
            elseif(line MATCHES "^VERSION_EXTRA=.*$")
                string(REGEX REPLACE "^VERSION_EXTRA=(.*)$" "\\1" VERSION_EXTRA "${line}")
                set(VERSION_EXTRA "${VERSION_EXTRA}" PARENT_SCOPE)
            endif()
        endforeach()
    else()
        message(FATAL_ERROR "Version file ${version_file} not found")
    endif()
endfunction()

# Named POLARFS_VERSION (not VERSION) so -I${CMAKE_SOURCE_DIR} does not shadow
# the C++20 standard header <version> (e.g. Docker Desktop bind mounts + Folly).
parse_version_file("${CMAKE_SOURCE_DIR}/POLARFS_VERSION")

foreach(var IN ITEMS VERSION_MAJOR VERSION_MINOR VERSION_PATCH)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "Missing required version component: ${var}")
    endif()
endforeach()

if(NOT DEFINED VERSION_EXTRA)
    set(VERSION_EXTRA "")
endif()

set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}${VERSION_EXTRA}")

execute_process(COMMAND git describe --long --always OUTPUT_VARIABLE RAW_GIT_DESC)
if (RAW_GIT_DESC STREQUAL "")
    set(RAW_GIT_DESC "_")
endif()
string(REPLACE "\n" "" RAW_GIT_DESC ${RAW_GIT_DESC})
message("build git version: (\"${RAW_GIT_DESC} \")")

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    execute_process(COMMAND date OUTPUT_VARIABLE RAW_DATE)
    string(REPLACE "\n" "" RAW_DATE ${RAW_DATE})
    message("build date: (\"${RAW_DATE} \")")

else()
    set (RAW_DATE "debug")
endif()

set (VERSION_DETAIL "(\"pfsd-build-desc-${VERSION_FULL}-${RAW_GIT_DESC}-${RAW_DATE}\")")

configure_file(
    ${CMAKE_SOURCE_DIR}/config.h.in
    ${CMAKE_BINARY_DIR}/config.h
    @ONLY
)
