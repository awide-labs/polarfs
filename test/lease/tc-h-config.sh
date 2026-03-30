#!/bin/bash
# tc-h-config.sh — Group H: configuration option tests.
#
# TC-H1: Basic RW mount succeeds with the test config and the lease record
#        is written correctly on disk.
# TC-H2: A live RW holder still blocks a second RW mount.
#        Timing: wait_and_check polls every 1s and detects the live holder
#        within a few poll cycles; threshold is < LEASE_TEST_DURATION.

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group H: configuration options ==="

# ---------------------------------------------------------------------------
# TC-H1: Basic RW mount succeeds; lease sector is written
# ---------------------------------------------------------------------------
echo ""
echo "TC-H1: RW mount succeeds, lease record on disk"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-H1: RW mount failed"
else
    # Verify sector 1 has a non-zero lease record
    sector_offset=$(paxos_sector_offset 1)
    tmp=$(mktemp)
    pfs_cmd 50 read -o "$sector_offset" -l "$LEASE_TEST_SECTOR_SIZE" \
        "/${TEST_LOOP_DEVICE_NAME}/.pfs-paxos" >"$tmp" 2>/dev/null || {
        rm -f "$tmp"; stop_holder
        fail "TC-H1: could not read paxos sector 1"
        lease_test_summary; exit 1
    }
    nonzero=$(od -v -t x1 -A n "$tmp" | tr -s ' \n' '\n' \
        | grep -v '^00$' | grep -v '^$' | wc -l) || true
    rm -f "$tmp"
    stop_holder

    if [[ "$nonzero" -gt 0 ]]; then
        pass "TC-H1: RW mount succeeded, lease record on disk ($nonzero non-zero bytes)"
    else
        fail "TC-H1: RW mount reported success but sector 1 is still zeroed"
    fi
fi

# ---------------------------------------------------------------------------
# TC-H2: Live RW holder blocks second RW mount
# ---------------------------------------------------------------------------
echo ""
echo "TC-H2: live RW holder blocks second RW mount"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-H2: setup: could not start RW holder as host 1"
else
    H1_PID="$HOLDER_PID"
    sleep 2  # ensure H1 is actively renewing

    before=$SECONDS
    rv=0
    try_rw_mount 2 || rv=$?
    elapsed=$(( SECONDS - before ))

    stop_holder "$H1_PID"

    if [[ $rv -ne 0 ]]; then
        # wait_and_check polls every 1s; under CI load 2-3 poll cycles
        # are normal.  The threshold is LEASE_TEST_DURATION: a live
        # holder must be detected before the lease expires.
        max_ok=$LEASE_TEST_DURATION
        if [[ $elapsed -lt $max_ok ]]; then
            pass "TC-H2: RW mount blocked (${elapsed}s < ${max_ok}s) — pre-acquire check unaffected"
        else
            fail "TC-H2: RW mount was blocked but took ${elapsed}s (expected < ${max_ok}s with live holder)"
        fi
    else
        fail "TC-H2: second RW mount succeeded while host 1 held a live lease"
    fi
fi

lease_test_summary
