#!/usr/bin/env bash

set -euo pipefail

# Common functions for building packages

SRC_ROOT="$(cd "$(dirname "$0")"/../../; pwd)"

build_and_install() {
    cd "$SRC_ROOT"

    ./autobuild.sh

    rm -rf install
    INSTALL_ROOT=install ./install.sh
}

get_version() {
    # shellcheck source=../POLARFS_VERSION
    source "$SRC_ROOT"/POLARFS_VERSION
    echo "$VERSION_MAJOR.$VERSION_MINOR.$VERSION_PATCH${VERSION_EXTRA:-}"
}

build_package() {
    local package_type=$1
    local version=$2
    local release=$3

    fpm -s dir -t "$package_type" -n polarfs -v "$version" --iteration "$release" \
        --log info \
        -d boost-context \
        -d boost-filesystem \
        -d boost-regex \
        -d double-conversion \
        -d fmt \
        -d gflags \
        -d glibc \
        -d glog \
        -d libaio \
        -d libevent \
        -d libgcc \
        -d libicu \
        -d libstdc++ \
        -d libunwind \
        -d python3 \
        --force \
        --description "PolarFS distributed file system" \
        -C install etc/ usr/ var/
}
