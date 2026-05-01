#!/usr/bin/env bash

set -euo pipefail

# shellcheck source=../common.sh
source "$(dirname "$0")/../common.sh"

usage() {
    cat <<EOF
Usage:

    $0 [RELEASE]

Build the source code and package it into polarfs-VERSION-RELEASE.rpm, where the
VERSION string is composed of variables defined in the POLARFS_VERSION file in the root
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

    local -r version=$(get_version)
    local -r release=${1:-"1"}

    build_and_install
    build_package rpm "$version" "$release"
}

main "$@"
