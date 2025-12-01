#!/usr/bin/env bash

set -euo pipefail

SRC_ROOT="$(cd "$(dirname "$0")"/../../; pwd)"

usage() {
    cat <<EOF
Usage:

    $0 VERSION [RELEASE]

Build the source code and package it into polarfs-VERSION-RELEASE.rpm
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
    if [[ $# -lt 1 || $# -gt 2 ]]; then
           usage
           exit 1
    fi

    check_env

    local -r version=$1
    local -r release=${2:-"1"}

    cd "$SRC_ROOT"

    ./autobuild.sh

    rm -rf install
    INSTALL_ROOT=install ./install.sh

    fpm -s dir -t rpm -n polarfs -v "$version" --iteration "$release" \
        --force \
        --description "Alibaba PolarFS distributed file system" \
        -C install etc/ usr/ var/
}

main "$@"
