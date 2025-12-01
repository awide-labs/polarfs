#!/usr/bin/env bash

set -euo pipefail

SRC_ROOT="$(cd "$(dirname "$0")"/../../; pwd)"

usage() {
    cat <<EOF
Usage:

    $0 [RELEASE]

Build the source code and package it into polarfs-VERSION-RELEASE.rpm, where the
VERSION string is composed of variables defined in the VERSION file in the root
source directory and the optional RELEASE argument specifies the RPM release
number.

When not specified, RELEASE defaults to 1.
EOF
}

check_env() {
    command -v fpm >/dev/null 2>&1 || {
        cat >&2 <<EOF
Error: 'fpm' was not found in PATH. You may want to install it with:

    sudo dnf -y install rubygems
    gem install --user-install fpm
EOF
        exit 1
    }

    command -v rpmbuild >/dev/null 2>&1 || {
        cat >&2 <<EOF
Error: 'rpmbuild' was not found in PATH. You may want to install it with:

    sudo dnf -y install rpm-build
EOF
        exit 1
    }
}

main() {
    if [[ $# -gt 1 ]]; then
           usage
           exit 1
    fi

    check_env

    cd "$SRC_ROOT"

    # shellcheck disable=SC1091
    source VERSION

    local -r version="$VERSION_MAJOR.$VERSION_MINOR.$VERSION_PATCH${VERSION_EXTRA:-}"

    local -r release=${1:-"1"}

    ./autobuild.sh

    rm -rf install
    INSTALL_ROOT=install ./install.sh

    fpm -s dir -t rpm -n polarfs -v "$version" --iteration "$release" \
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
        --description "Alibaba PolarFS distributed file system" \
        -C install etc/ usr/ var/
}

main "$@"
