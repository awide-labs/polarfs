#!/bin/bash
# tc-k-wait-check.sh — Group K: pfs_rw_lease_wait_and_check sub-paths.
#
# TC-K1: live holder renewing → timestamp-advance early exit.
#   A second host enters wait_and_check while the first is alive and renewing.
#   After one poll cycle (sleep 1s), the blocker's timestamp has advanced →
#   pfs_rw_lease_wait_and_check returns -EBUSY immediately.  The total mount
#   attempt time must be < LEASE_TEST_DURATION - 1, not the full
#   LEASE_TEST_DURATION (5s) that a naive timeout-only loop would take.
#   Threshold is LEASE_TEST_DURATION - 1 rather than a hard-coded 3 to
#   tolerate CI scheduling jitter (1-3 poll cycles are normal under load).
#
# TC-K2: holder cleanly releases mid-wait → H2 proceeds without waiting
#   for expiry.
#   H2 enters wait_and_check while H1 holds the lease.  While H2 is inside
#   the polling sleep(1), H1 unmounts cleanly (pfs_rw_lease_release clears
#   the sector to zero).  On the next poll, H2 reads rv==0 → calls
#   pfs_rw_lease_check → no holders → H2 acquires the lease.
#   The mount must succeed within 3s of H1's clean release, not after the
#   full LEASE_TEST_DURATION.

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group K: pfs_rw_lease_wait_and_check sub-paths ==="

# ---------------------------------------------------------------------------
# TC-K1: live renewing holder → timestamp advance triggers fast -EBUSY
# ---------------------------------------------------------------------------
echo ""
echo "TC-K1: live holder renewing → timestamp advance exits wait_and_check fast"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-K1: setup: could not start RW holder as host 1"
else
    sleep 2   # let H1 complete at least one renewal so its timestamp is fresh

    before=$SECONDS
    rv=0
    try_rw_mount 2 || rv=$?
    elapsed=$(( SECONDS - before ))

    stop_holder

    # wait_and_check does: sleep(1) → read → ts changed → return -EBUSY.
    # Without the ts-advance path it would poll for LEASE_TEST_DURATION (${LEASE_TEST_DURATION}s).
    # elapsed < LEASE_TEST_DURATION-1 proves the early-exit fired (1-3 poll
    # cycles under load) rather than waiting for the full expiry timeout.
    max_ok=$(( LEASE_TEST_DURATION - 1 ))
    if [[ $rv -ne 0 ]]; then
        if [[ $elapsed -lt $max_ok ]]; then
            pass "TC-K1: mount failed in ${elapsed}s — timestamp-advance exit confirmed (< ${max_ok}s)"
        else
            fail "TC-K1: mount failed but took ${elapsed}s — ts-advance early-exit may be broken (expected < ${max_ok}s)"
        fi
    else
        fail "TC-K1: mount by host 2 succeeded while host 1 held a live lease"
    fi
fi

# ---------------------------------------------------------------------------
# TC-K2: clean release mid-wait → H2 mounts quickly without expiry wait
# ---------------------------------------------------------------------------
echo ""
echo "TC-K2: holder releases cleanly mid-wait → H2 acquires without expiry wait"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-K2: setup: could not start RW holder as host 1"
else
    H1_PID="$HOLDER_PID"
    sleep 1   # let H1 establish its lease

    # Start H2 in background.  pfs_mount will call pfs_rw_lease_check (fast
    # → -EBUSY) then pfs_rw_lease_wait_and_check, which immediately calls
    # sleep(1).  By the time 0.5s has elapsed H2 is reliably inside that
    # sleep(1) call.
    tmpout2=$(mktemp)
    lease_hold 2 rw >"$tmpout2" &
    H2_PID=$!

    # Give H2 time to enter the wait_and_check polling loop.
    sleep 0.5

    # Cleanly stop H1 (pfs_rw_lease_release zeros sector 1).
    # stop_holder blocks until H1 exits, so sector 1 is clear when it returns.
    stop_holder "$H1_PID"
    H1_PID=""
    T_stop=$SECONDS

    # H2's next poll (sleep finishes in ~0.5s) reads rv==0 → pfs_rw_lease_check
    # → no holders → proceeds through ballot acquire → prints MOUNTED.
    # Allow up to 5s: poll wake-up (≤1s) + ballot acquire (≤1s) + overhead (3s).
    deadline=$(( SECONDS + 5 ))
    h2_mounted=false
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -q "^MOUNTED$" "$tmpout2" 2>/dev/null; then
            h2_mounted=true
            break
        fi
        kill -0 "$H2_PID" 2>/dev/null || break
        sleep 0.2
    done

    T_done=$SECONDS
    elapsed=$(( T_done - T_stop ))

    HOLDER_PID="$H2_PID"
    stop_holder
    rm -f "$tmpout2"

    # Without the rv==0 clean-release path H2 would continue polling until
    # H1's stale sector ages out (LEASE_TEST_DURATION seconds).  With it, H2
    # detects the cleared sector on the next wake-up and mounts within 3s.
    if $h2_mounted; then
        if [[ $elapsed -lt 4 ]]; then
            pass "TC-K2: H2 mounted RW within ${elapsed}s of H1 clean release (< 4s)"
        else
            fail "TC-K2: H2 mounted but took ${elapsed}s after H1 released — clean-release mid-wait detection may be slow"
        fi
    else
        fail "TC-K2: H2 did not mount RW after H1 clean release (elapsed ${elapsed}s)"
    fi
fi

lease_test_summary
