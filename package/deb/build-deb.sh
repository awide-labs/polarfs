#!/usr/bin/env bash

set -euo pipefail

# shellcheck source=../common.sh
source "$(dirname "$0")/../common.sh"

usage() {
    cat <<EOF
Usage:

    $0 [RELEASE]

Build the source code and package it into polarfs-VERSION-RELEASE.deb, where the
VERSION string is composed of variables defined in the VERSION file in the root
source directory and the optional RELEASE argument specifies the DEB release
number.

When not specified, RELEASE defaults to 1.
EOF
}

check_env() {
    command -v fpm >/dev/null 2>&1 || {
        cat >&2 <<EOF
Error: 'fpm' was not found in PATH. You may want to install it with:

    sudo apt -y install ruby-dev
    gem install --user-install fpm
EOF
        exit 1
    }

    command -v dpkg-buildpackage >/dev/null 2>&1 || {
        cat >&2 <<EOF
Error: 'dpkg-buildpackage' was not found in PATH. You may want to install it
with:

    sudo apt -y install build-essential
EOF
        exit 1
    }
}

build_package() {
    local package_type=$1
    local version=$2
    local release=$3

    fpm -s dir -t "$package_type" -n polarfs -v "$version" --iteration "$release" \
        --log info \
        -d libboost-atomic1.74.0 \
        -d libboost-context1.74.0 \
        -d libboost-filesystem1.74.0 \
        -d libboost-program-options1.74.0 \
        -d libboost-regex1.74.0 \
        -d libboost-system1.74.0 \
        -d libboost-thread1.74.0 \
        -d libdouble-conversion3 \
        -d libfmt-dev \
        -d libgoogle-glog-dev \
        -d libgflags2.2 \
        -d libaio1 \
        -d python3 \
        --force \
        --description "Alibaba PolarFS distributed file system" \
        -C install etc/ usr/ var/
}

main() {
    if [[ $# -gt 1 ]]; then
           usage
           exit 1
    fi

    check_env

    local -r version=$(get_version)
    local -r release=${1:-"1"}

    build_and_install
    build_package deb "$version" "$release"
}

main "$@"
