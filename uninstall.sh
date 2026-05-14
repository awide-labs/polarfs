#!/bin/bash

# Reverses install.sh — removes every file the installer placed and tears
# down the empty directory trees. Runtime dirs (/var/run/pfsd, /var/run/pfs)
# are only removed if empty so a running daemon's state is preserved.

set -euo pipefail

INSTALL_ROOT=${INSTALL_ROOT:-/}
INSTALL_BASE_DIR="${INSTALL_ROOT}/usr/local/polarstore"

# files under /usr/local/polarstore
rm -f "${INSTALL_BASE_DIR}"/pfsd/include/pfsd_sdk.h
rm -f "${INSTALL_BASE_DIR}"/pfsd/lib/libpfsd.a
rm -f "${INSTALL_BASE_DIR}"/pfsd/lib/libpfsd_test.so
rm -f "${INSTALL_BASE_DIR}"/pfs_core/include/pfs_api.h
rm -f "${INSTALL_BASE_DIR}"/pfs_core/lib/libpfs.a
rm -f "${INSTALL_BASE_DIR}"/pfsd/bin/pfsdaemon
rm -f "${INSTALL_BASE_DIR}"/pfsd/bin/pfs-fuse
rm -f "${INSTALL_BASE_DIR}"/pfsd/conf/pfsd_logger.conf
rm -f "${INSTALL_BASE_DIR}"/pfsd/bin/start_pfsd.sh
rm -f "${INSTALL_BASE_DIR}"/pfsd/bin/stop_pfsd.sh
rm -f "${INSTALL_BASE_DIR}"/pfsd/bin/mount_pfs_fuse.sh
rm -f "${INSTALL_BASE_DIR}"/pfsd/bin/umount_pfs_fuse.sh
rm -f "${INSTALL_BASE_DIR}"/pfsd/bin/clean_pfsd.sh

# files under /etc and /usr/local/bin
rm -f "${INSTALL_ROOT}"/etc/init.d/pfsd_env
rm -f "${INSTALL_ROOT}"/etc/polarfs.conf
rm -f "${INSTALL_ROOT}"/usr/local/bin/pfs
rm -f "${INSTALL_ROOT}"/usr/local/bin/pfsadm

# tear down the polarstore tree leaf-first; rmdir leaves non-empty dirs alone
rmdir --ignore-fail-on-non-empty \
	"${INSTALL_BASE_DIR}"/pfsd/bin \
	"${INSTALL_BASE_DIR}"/pfsd/conf \
	"${INSTALL_BASE_DIR}"/pfsd/include \
	"${INSTALL_BASE_DIR}"/pfsd/lib \
	"${INSTALL_BASE_DIR}"/pfsd \
	"${INSTALL_BASE_DIR}"/pfs_core/include \
	"${INSTALL_BASE_DIR}"/pfs_core/lib \
	"${INSTALL_BASE_DIR}"/pfs_core \
	"${INSTALL_BASE_DIR}" 2>/dev/null || true

# runtime dirs: drop the marker, rmdir only if empty
rm -f "${INSTALL_ROOT}"/var/run/pfsd/.pfsd
rmdir --ignore-fail-on-non-empty \
	"${INSTALL_ROOT}"/var/run/pfsd \
	"${INSTALL_ROOT}"/var/run/pfs 2>/dev/null || true

echo "uninstall pfsd success!"
