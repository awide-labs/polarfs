#!/bin/bash

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

set -e

BASE_DIR=$(cd "$(dirname "$0")"; pwd)
cd "$BASE_DIR"

CMAKE_EXTRA_FLAGS=""
if [[ "${ENABLE_SANITIZERS:-}" == "1" ]]; then
    CMAKE_EXTRA_FLAGS="-DENABLE_SANITIZERS=ON"
    echo -e "\033[33m Sanitizers enabled (address + undefined behavior) \033[0m"
fi

echo -e "\033[33m begin compile pfsdaemon|pfs|libpfs.a|libpfsd.a \033[0m"
mkdir -p build
pushd build
cmake ../ $CMAKE_EXTRA_FLAGS
make -j$(nproc)
popd

echo -e "\033[33m end compile, binary's in ./bin, library's in ./lib \033[0m"

