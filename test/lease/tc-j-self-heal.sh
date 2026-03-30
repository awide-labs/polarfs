#!/bin/bash
# tc-j-self-heal.sh — Group J: pfs_rw_lease_renew self-heal.
#
# TC-J1: own paxos sector corrupted (bad checksum) while the RW lease is
#        held → the log thread's next renewal tick detects the corrupt record
#        (rv = PFS_HOST_ECHECKSUM ≠ PFS_OK), calls pfs_rw_lease_acquire to
#        re-write a valid record, and the lease continues normally.
#
# TC-J2: after self-heal the renewed lease is live — a second host cannot
#        mount RW (the healed record has a fresh timestamp and must block new
#        mounters until the holder exits).

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group J: pfs_rw_lease_renew self-heal ==="

# ---------------------------------------------------------------------------
# TC-J1: corrupt own sector → renewal self-heals, record reappears
# ---------------------------------------------------------------------------
echo ""
echo "TC-J1: corrupt own sector triggers renewal self-heal"

HOLDER_PID=""
if ! start_self_corrupt_holder 1; then
    fail "TC-J1: setup: could not start self-corrupt holder as host 1"
else
    if trigger_self_corrupt; then
        pass "TC-J1: renewal self-healed corrupt own sector (result: ${SELF_CORRUPT_RESULT})"
    else
        fail "TC-J1: self-heal did not occur (result: ${SELF_CORRUPT_RESULT})"
    fi
    stop_holder
fi

# ---------------------------------------------------------------------------
# TC-J2: after self-heal the lease is live — blocks another RW mount
# ---------------------------------------------------------------------------
echo ""
echo "TC-J2: self-healed lease blocks another RW mount"

HOLDER_PID=""
if ! start_self_corrupt_holder 1; then
    fail "TC-J2: setup: could not start self-corrupt holder as host 1"
else
    if ! trigger_self_corrupt; then
        stop_holder
        fail "TC-J2: self-heal failed (${SELF_CORRUPT_RESULT}), cannot test exclusion"
    else
        # Holder is still alive with a freshly self-healed lease record.
        # A second host should be blocked: pfs_rw_lease_check returns -EBUSY
        # → pfs_rw_lease_wait_and_check detects the timestamp advancing
        #   (ts-advance fast path, 1-2 poll cycles).
        before=$SECONDS
        rv=0
        try_rw_mount 2 || rv=$?
        elapsed=$(( SECONDS - before ))
        stop_holder

        # Budget: pfs_meta_load_all_chunks (≤8s under CI load) +
        # wait_and_check ts-advance fast path (1-2 poll cycles) +
        # overhead.  This threshold is intentionally generous because
        # TC-K1 already proves the ts-advance early-exit works precisely;
        # here we just verify H1's self-healed lease is live (rv != 0).
        max_ok=$(( LEASE_TEST_DURATION + 8 ))
        if [[ $rv -ne 0 ]]; then
            if [[ $elapsed -lt $max_ok ]]; then
                pass "TC-J2: self-healed lease blocked host 2 (${elapsed}s < ${max_ok}s)"
            else
                fail "TC-J2: host 2 was blocked but took ${elapsed}s (expected < ${max_ok}s)"
            fi
        else
            fail "TC-J2: host 2 mounted RW while host 1 held self-healed lease"
        fi
    fi
fi

lease_test_summary
