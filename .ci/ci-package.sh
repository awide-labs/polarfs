#!/bin/bash
set -e

# Build PolarFS
./autobuild.sh

# Detect OS type and build appropriate package
if command -v dpkg >/dev/null 2>&1; then
  echo 'Detected DEB-based OS, building DEB package'
  ./package/deb/build-deb.sh
elif command -v rpm >/dev/null 2>&1; then
  echo 'Detected RPM-based OS, building RPM package'
  ./package/rpm/build-rpm.sh
else
  echo 'Error: Could not detect package manager (dpkg or rpm)'
  exit 1
fi
