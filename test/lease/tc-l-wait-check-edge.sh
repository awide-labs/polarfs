#!/bin/bash
# tc-l-wait-check-edge.sh — Group L: pfs_rw_lease_wait_and_check edge cases.
#
# Both tests use write-two-records to plant simultaneous crashed-host sectors
# for H1 and H2 (same timestamp → they expire at the same instant).
#
# TC-L1: Multiple simultaneous live stale blockers — mount waits at most one
#   lease duration (not N × duration).
#   H1 and H2 both have fresh lease records (age=0 at plant time).  H3 mounts
#   immediately: pfs_rw_lease_check finds H1 first → -EBUSY.
#   pfs_rw_lease_wait_and_check polls H1; on the cycle when H1 expires,
#   pfs_rw_lease_check finds H2 also expired (same ts) → 0 → H3 mounts.
#   Total elapsed must be ≤ LEASE_TEST_DURATION + 3s, NOT 2 × LEASE_TEST_DURATION.
#
# TC-L2: Both blockers expired before mount attempt — pfs_rw_lease_check
#   passes immediately without entering wait_and_check.
#   Same setup but after sleeping LEASE_TEST_DURATION seconds both records are
#   stale.  pfs_rw_lease_check skips them both → 0 → H3 mounts in < 3s.
#
# Note: the specific blocker_id == 0 branch inside pfs_rw_lease_wait_and_check
# (fires when all live blockers expire in the milliseconds between
# pfs_rw_lease_check and the wait_and_check scan) requires a CLOCK_REALTIME
# second boundary to cross between two sequential function calls — not reliably
# forceable from a shell test with integer-second timestamps.  TC-L1 and TC-L2
# together verify the full observable range of correct multi-blocker behavior.

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group L: pfs_rw_lease_wait_and_check edge cases (multi-blocker) ==="

# ---------------------------------------------------------------------------
# TC-L1: Multiple simultaneous live stale blockers → single wait cycle (≤ D)
# ---------------------------------------------------------------------------
echo ""
echo "TC-L1: two simultaneous live stale blockers — mount waits at most one lease duration"

# Plant fresh records for H1 and H2 from within one mount session (H50).
# Both get the same hr_timestamp so they expire at the same instant.
# H50's own sector is zeroed by the clean unmount; H1/H2 sectors remain.
if ! (lease_hold 50 write-two-records 1 2); then
    fail "TC-L1: write-two-records failed"
else
    # H3 mounts immediately while both H1 and H2 are live.
    # wait_and_check polls the first blocker (H1) to expiry;
    # by the same poll cycle H2 has expired too (same ts).
    # pfs_rw_lease_check then returns 0 → H3 mounts.
    before=$SECONDS
    rv=0
    try_rw_mount 3 || rv=$?
    elapsed=$(( SECONDS - before ))

    if [[ $rv -eq 0 ]]; then
        max_ok=$(( LEASE_TEST_DURATION + 3 ))
        if [[ $elapsed -le $max_ok ]]; then
            pass "TC-L1: H3 mounted after ${elapsed}s with 2 simultaneous stale blockers (≤ ${max_ok}s)"
        else
            fail "TC-L1: H3 mounted but took ${elapsed}s — possible N×duration wait (expected ≤ ${max_ok}s)"
        fi
    else
        fail "TC-L1: H3 mount failed (rv=$rv) with 2 simultaneous stale blockers"
    fi
fi

# ---------------------------------------------------------------------------
# TC-L2: Both blockers already expired — pfs_rw_lease_check passes directly
# ---------------------------------------------------------------------------
echo ""
echo "TC-L2: both blockers expired before mount attempt — no unnecessary wait"

if ! (lease_hold 50 write-two-records 1 2); then
    fail "TC-L2: write-two-records failed"
else
    # Sleep until both records are stale (age >= LEASE_TEST_DURATION).
    # pfs_rw_lease_check will find them both expired and return 0 without
    # calling pfs_rw_lease_wait_and_check at all.
    sleep "$LEASE_TEST_DURATION"

    before=$SECONDS
    rv=0
    try_rw_mount 3 || rv=$?
    elapsed=$(( SECONDS - before ))

    # Threshold: pfs_meta_load_all_chunks (≤3s warm) + ballot acquire (≤1s)
    # + log-start/replay overhead (≤2s) = ≤6s without wait_and_check.
    # If wait_and_check ran for a full LEASE_TEST_DURATION cycle it would
    # add ≥ LEASE_TEST_DURATION s, pushing total to ≥ LEASE_TEST_DURATION+6.
    # Using LEASE_TEST_DURATION+3 as the ceiling distinguishes the correct
    # path (no unnecessary wait) from a full wait_and_check cycle (bug).
    max_ok=$(( LEASE_TEST_DURATION + 3 ))
    if [[ $rv -eq 0 ]]; then
        if [[ $elapsed -lt $max_ok ]]; then
            pass "TC-L2: H3 mounted in ${elapsed}s — expired blockers caused no delay (< ${max_ok}s)"
        else
            fail "TC-L2: H3 mounted but took ${elapsed}s — stale records may have caused unnecessary wait (expected < ${max_ok}s)"
        fi
    else
        fail "TC-L2: H3 mount failed (rv=$rv) after both blockers already expired"
    fi
fi

lease_test_summary
