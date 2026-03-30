#!/bin/bash
# tc-a-mutual-exclusion.sh — Group A: basic mutual exclusion tests.
#
# TC-A1: Single RW mount acquires and releases lease cleanly.
# TC-A2: A live RW holder blocks a second RW mount (same pbdname, diff host).
# TC-A3: After holder cleanly unmounts, second host mounts immediately.
# TC-A4: RO mount succeeds even while an RW holder is active.

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group A: Mutual exclusion ==="

# ---------------------------------------------------------------------------
# TC-A1: Single RW mount acquires and releases lease cleanly
# ---------------------------------------------------------------------------
echo ""
echo "TC-A1: single RW mount lifecycle"

if start_rw_holder 1; then
    stop_holder
    pass "TC-A1: RW mount as host 1 succeeds"
else
    fail "TC-A1: RW mount as host 1 failed"
fi

# ---------------------------------------------------------------------------
# TC-A2: Live RW lease blocks a second RW mount from a different host
# ---------------------------------------------------------------------------
echo ""
echo "TC-A2: live lease blocks second RW mount"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-A2: setup: could not mount as host 1"
else
    # Host 1 holds the lease.  Host 2 must be rejected immediately.
    rv=0
    try_rw_mount 2 || rv=$?
    stop_holder

    # pfs_mount returns -EBUSY which propagates as non-zero exit from pfs CLI
    if [[ $rv -ne 0 ]]; then
        pass "TC-A2: host 2 RW mount rejected while host 1 holds live lease"
    else
        fail "TC-A2: host 2 RW mount should have been rejected"
    fi
fi

# ---------------------------------------------------------------------------
# TC-A3: Clean release lets second host mount without waiting
# ---------------------------------------------------------------------------
echo ""
echo "TC-A3: clean release followed by immediate second mount"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-A3: setup: could not mount as host 1"
else
    stop_holder  # clean unmount → sector zeroed

    # Host 2 should now mount immediately (no 5s wait)
    before=$SECONDS
    if start_rw_holder 2; then
        elapsed=$(( SECONDS - before ))
        stop_holder
        if [[ $elapsed -lt $((LEASE_TEST_DURATION - 1)) ]]; then
            pass "TC-A3: host 2 mounted immediately after clean release (${elapsed}s)"
        else
            fail "TC-A3: host 2 took too long after clean release (${elapsed}s)"
        fi
    else
        fail "TC-A3: host 2 failed to mount after clean release"
    fi
fi

# ---------------------------------------------------------------------------
# TC-A4: RO mount succeeds while RW holder is active
# ---------------------------------------------------------------------------
echo ""
echo "TC-A4: RO mount succeeds alongside active RW holder"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-A4: setup: could not mount as host 1"
else
    rv=0
    try_ro_mount 2 || rv=$?
    stop_holder

    if [[ $rv -eq 0 ]]; then
        pass "TC-A4: host 2 RO mount succeeded while host 1 holds RW lease"
    else
        fail "TC-A4: host 2 RO mount failed unexpectedly (exit $rv)"
    fi
fi

lease_test_summary
