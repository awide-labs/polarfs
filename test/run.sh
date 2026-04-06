#!/bin/bash
set -euo pipefail

# Initialize environment variables
export TEST_IMAGE_FILE="${TEST_IMAGE_FILE:-/tmp/polarfs_disk.img}"
export TEST_LOOP_DEVICE="${TEST_LOOP_DEVICE:-/dev/loop100}"
export TEST_LOOP_DEVICE_NAME="${TEST_LOOP_DEVICE_NAME:-loop100}"
export TEST_IMAGE_SIZE_GB="${TEST_IMAGE_SIZE_GB:-30}"

# Cleanup function to ensure daemon is stopped and resources are cleaned up even on failure
cleanup() {
    local exit_code=$?
    echo ""
    echo "=== CLEANUP (exit code: $exit_code) ==="

    # Stop daemon if it's running
    if [[ -n "${TEST_LOOP_DEVICE_NAME:-}" ]]; then
        echo "Stopping PFSD..."
        /usr/local/polarstore/pfsd/bin/stop_pfsd.sh "$TEST_LOOP_DEVICE_NAME" 2>/dev/null || true
        echo "Cleaning up PFSD..."
        /usr/local/polarstore/pfsd/bin/clean_pfsd.sh "$TEST_LOOP_DEVICE_NAME" 2>/dev/null || true
    fi

    # Cleanup loop device
    if [[ -n "${TEST_LOOP_DEVICE:-}" ]]; then
        losetup -d "$TEST_LOOP_DEVICE" 2>/dev/null || true
    fi

    # Cleanup test image file
    if [[ -n "${TEST_IMAGE_FILE:-}" ]]; then
        rm -f "$TEST_IMAGE_FILE" 2>/dev/null || true
    fi

    if [[ $exit_code -ne 0 ]]; then
        echo "=== TEST FAILED ==="
        exit $exit_code
    fi
}

# Register cleanup function to run on exit
trap cleanup EXIT

# === SETUP PHASE ===
echo "=== START SETUP ==="

echo "Creating ${TEST_IMAGE_SIZE_GB}GB test image file..."
fallocate -l ${TEST_IMAGE_SIZE_GB}G "$TEST_IMAGE_FILE"

echo "Attaching image to loop device..."
# Create loop device node if it doesn't exist
if [ ! -e "$TEST_LOOP_DEVICE" ]; then
    LOOP_NUM="${TEST_LOOP_DEVICE##/dev/loop}"
    mknod "$TEST_LOOP_DEVICE" b 7 "$LOOP_NUM" 2>/dev/null || true
fi
losetup -d "$TEST_LOOP_DEVICE" 2>/dev/null || true
if ! losetup "$TEST_LOOP_DEVICE" "$TEST_IMAGE_FILE"; then
    echo "Error: Failed to attach image to loop device"
    exit 1
fi

echo "Installing polarfs..."
./install.sh

echo "=== SETUP COMPLETE ==="

# === TEST PHASE ===
echo ""
echo "=== TEST ==="
echo "Block device: $TEST_LOOP_DEVICE"
echo ""

# 1. Format with PFS
echo "Formatting..."
./bin/pfs -C disk mkfs -f "$TEST_LOOP_DEVICE_NAME"

# 2. Start daemon
echo "Starting PFSD..."
/usr/local/polarstore/pfsd/bin/start_pfsd.sh -p "$TEST_LOOP_DEVICE_NAME"

# 3. Run tests with individual error reporting
echo "Running tests..."
TEST_FAILED=0

echo "Running pfs_inode_lru_test..."
if ! ./bin/pfs_inode_lru_test; then
    echo "ERROR: pfs_inode_lru_test failed"
    TEST_FAILED=1
fi

echo "Running pfsd_filetest..."
if ! ./bin/pfsd_filetest 0 "disk" "$TEST_LOOP_DEVICE_NAME"; then
    echo "ERROR: pfsd_filetest failed"
    TEST_FAILED=1
fi

echo "Running pfsd_cachetest..."
if ! ./bin/pfsd_cachetest "disk" "$TEST_LOOP_DEVICE_NAME"; then
    echo "ERROR: pfsd_cachetest failed"
    TEST_FAILED=1
fi

# Two stage test, create test files first, then run tests on them
echo "Running pfsd_perftest (initialization)..."
if ! ./bin/pfsd_perftest -C "disk" -D "$TEST_LOOP_DEVICE_NAME" -i; then
    echo "ERROR: pfsd_perftest initialization failed"
    TEST_FAILED=1
fi

echo "Running pfsd_perftest (performance test)..."
if ! ./bin/pfsd_perftest -C "disk" -D "$TEST_LOOP_DEVICE_NAME" -t 100000; then
    echo "ERROR: pfsd_perftest performance test failed"
    TEST_FAILED=1
fi

echo "Running pfsd_ebusy_test..."
if ! bash "$(dirname "$0")/pfsd-ebusy-test.sh"; then
    echo "ERROR: pfsd_ebusy_test failed"
    TEST_FAILED=1
fi

if [[ $TEST_FAILED -ne 0 ]]; then
    echo "=== ONE OR MORE TESTS FAILED ==="
    exit 1
fi

echo "=== TEST PASSED ==="

# Cleanup will be handled by the trap function on exit
