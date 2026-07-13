#!/bin/bash
# run-lease-tests.sh — Run all cross-host RW lease test groups.
#
# These tests must be run standalone (not while pfsd is running on the same
# loop device).  They reformat the device with mkfs on each test group.
#
# Usage:
#   sudo ./test/lease/run-lease-tests.sh
#
# Optional environment variables (see common.sh for full list):
#   LEASE_TEST_DURATION=5   — lease duration in seconds (default 5 for fast CI)
#   TEST_LOOP_DEVICE=...     — loop device to use (default /dev/loop100)
#   TEST_IMAGE_FILE=...    — backing image file
#   BINDIR=...              — directory containing pfs and pfs_lease_hold

set -euo pipefail

export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Verify required binaries exist
BINDIR="${BINDIR:-$(cd "${SCRIPT_DIR}/../../bin" && pwd)}"
if [[ ! -x "${BINDIR}/pfs" ]]; then
    echo "ERROR: ${BINDIR}/pfs not found.  Build the project first."
    exit 1
fi
if [[ ! -x "${BINDIR}/pfs_lease_hold" ]]; then
    echo "ERROR: ${BINDIR}/pfs_lease_hold not found.  Build the project first."
    exit 1
fi

echo "============================================"
echo " PolarDB-FS cross-host RW lease / promotion tests"
echo " bindir: $BINDIR"
echo " duration: ${LEASE_TEST_DURATION:-5}s"
echo "============================================"

OVERALL_FAIL=0

LEASE_TEST_FAIL_FAST="${LEASE_TEST_FAIL_FAST:-0}"

run_group() {
    local script="$1"
    local name="$2"
    echo ""
    echo "--- $name ---"
    if bash "$SCRIPT_DIR/$script"; then
        echo "$name: OK"
    else
        echo "$name: FAILED"
        OVERALL_FAIL=1
        if [[ "$LEASE_TEST_FAIL_FAST" -eq 1 ]]; then
            echo "LEASE_TEST_FAIL_FAST=1, stopping."
            exit 1
        fi
    fi
}

run_group tc-a-mutual-exclusion.sh  "Group A: Mutual exclusion"
run_group tc-b-crash-recovery.sh    "Group B: Crash recovery"
run_group tc-c-ro-isolation.sh      "Group C: RO isolation"
run_group tc-d-generation.sh        "Group D: Generation counter"
run_group tc-e-renewal.sh           "Group E: Lease renewal"
run_group tc-f-promotion.sh         "Group F: RO-to-RW promotion"
run_group tc-g-remount.sh           "Group G: pfs_remount live promotion"
run_group tc-h-config.sh            "Group H: configuration options"
run_group tc-i-corruption.sh        "Group I: corrupt paxos sector handling"
run_group tc-j-self-heal.sh         "Group J: pfs_rw_lease_renew self-heal"
run_group tc-k-wait-check.sh        "Group K: wait_and_check sub-paths"
run_group tc-l-wait-check-edge.sh   "Group L: wait_and_check edge cases (multi-blocker)"
run_group tc-m-ballot.sh            "Group M: ballot-based lease acquisition"
run_group tc-n-watchdog.sh          "Group N: timer-kill watchdog"
run_group tc-o-refcount.sh          "Group O: Multi-client refcount"

echo ""
echo "============================================"
if [[ $OVERALL_FAIL -eq 0 ]]; then
    echo " ALL LEASE TEST GROUPS PASSED"
else
    echo " ONE OR MORE LEASE TEST GROUPS FAILED"
fi
echo "============================================"
exit $OVERALL_FAIL
