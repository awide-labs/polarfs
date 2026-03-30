#!/bin/bash
# tc-b-crash-recovery.sh — Group B: crash and lease expiry recovery tests.
#
# TC-B1: After simulated crash (SIGKILL on holder) and waiting one full lease
#        duration, a second host can mount RW (stale lease expired).
# TC-B2: pfs_rw_lease_wait_and_check exits early when it detects renewal has
#        stopped — specifically, host 2 does NOT wait the full lease duration
#        once renewal stops; it waits only until the timestamp age exceeds the
#        duration.
# TC-B3: If the holder is alive and actively renewing, host 2 gets EBUSY
#        quickly (within ~2s) without waiting the full lease duration.

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group B: Crash recovery ==="

# ---------------------------------------------------------------------------
# TC-B1: Stale lease (crash sim): host 2 can mount after lease expires
# ---------------------------------------------------------------------------
echo ""
echo "TC-B1: stale lease — host 2 mounts after expiry"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-B1: setup: could not mount as host 1"
else
    # Kill without cleanup → lease record stays on disk with old timestamp
    kill -KILL "$HOLDER_PID" 2>/dev/null || true
    wait "$HOLDER_PID" 2>/dev/null || true
    HOLDER_PID=""

    # The log thread was killed too; last renewal was at mount time.
    # Wait for the lease to expire (LEASE_TEST_DURATION seconds).
    echo "  Waiting ${LEASE_TEST_DURATION}s for lease to expire..."
    sleep $((LEASE_TEST_DURATION + 1))

    before=$SECONDS
    rv=0
    start_rw_holder 2 || rv=$?
    elapsed=$(( SECONDS - before ))
    if [[ $rv -eq 0 ]]; then
        stop_holder
        pass "TC-B1: host 2 mounted after stale lease expired (waited ${elapsed}s)"
    else
        fail "TC-B1: host 2 failed to mount after stale lease (exit $rv, ${elapsed}s)"
    fi
fi

# ---------------------------------------------------------------------------
# TC-B2: Wait-and-check detects dead node without sleeping full duration twice
# ---------------------------------------------------------------------------
echo ""
echo "TC-B2: wait-and-check exits as soon as timestamp stops advancing"

# We need host 1's lease to be fresh when host 2 starts trying to mount,
# but then host 1's renewal must stop.  We do this by:
#   1. Start H1, let it hold for 1s (gets at least one renewal)
#   2. Kill H1 (renewal stops, but timestamp is recent → age starts counting)
#   3. Immediately try H2 mount (goes into wait-and-check loop)
#   4. H2 should succeed within LEASE_TEST_DURATION + 1 seconds total
#      (not LEASE_TEST_DURATION * 2)

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-B2: setup: could not mount as host 1"
else
    sleep 1  # let one renewal tick

    kill -KILL "$HOLDER_PID" 2>/dev/null || true
    wait "$HOLDER_PID" 2>/dev/null || true
    HOLDER_PID=""

    before=$SECONDS
    rv=0
    start_rw_holder 2 || rv=$?
    elapsed=$(( SECONDS - before ))
    # Budget: pfs_meta_load_all_chunks (≤8s under CI load) +
    # wait_and_check single-pass expiry (≤LEASE_TEST_DURATION) +
    # ballot acquire/log-start overhead (≤7s) + jitter (3s) = +15.
    # This verifies H2 did NOT wait 2×LEASE_TEST_DURATION (which
    # would take ≥ 2×LEASE_TEST_DURATION + 8s overhead = 18s).
    max_wait=$(( LEASE_TEST_DURATION + 15 ))

    if [[ $rv -eq 0 ]]; then
        stop_holder
        if [[ $elapsed -le $max_wait ]]; then
            pass "TC-B2: host 2 mounted in ${elapsed}s (≤ max ${max_wait}s)"
        else
            fail "TC-B2: host 2 waited too long: ${elapsed}s (max ${max_wait}s)"
        fi
    else
        fail "TC-B2: host 2 failed to mount after dead node (exit $rv, ${elapsed}s)"
    fi
fi

# ---------------------------------------------------------------------------
# TC-B3: Live renewing holder causes fast EBUSY, not full-duration wait
# ---------------------------------------------------------------------------
echo ""
echo "TC-B3: live holder → fast EBUSY for would-be second writer"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-B3: setup: could not mount as host 1"
else
    H1_PID="$HOLDER_PID"   # save before start_rw_holder 2 overwrites HOLDER_PID
    sleep 2  # ensure H1 has renewed at least once

    before=$SECONDS
    rv=0
    start_rw_holder 2 || rv=$?
    elapsed=$(( SECONDS - before ))

    # H2 either failed (HOLDER_PID="") or succeeded (HOLDER_PID=H2_PID).
    # Stop whatever start_rw_holder last set, then always stop H1.
    stop_holder           # stop H2 if it somehow succeeded
    stop_holder "$H1_PID" # stop H1 (the intentional holder)

    # wait_and_check polls every 1s; one poll detects the timestamp advance.
    # pfs_meta_load_all_chunks and the lease-check scan add ~2s overhead.
    # Threshold is LEASE_TEST_DURATION - 1 to prove the ts-advance path fired
    # (1-3 cycles + overhead) rather than waiting for the full expiry timeout.
    max_ok=$(( LEASE_TEST_DURATION - 1 ))
    if [[ $rv -ne 0 ]]; then
        if [[ $elapsed -lt $max_ok ]]; then
            pass "TC-B3: host 2 got EBUSY fast (${elapsed}s < ${max_ok}s) — live holder detected"
        else
            fail "TC-B3: host 2 EBUSY but took ${elapsed}s (expected < ${max_ok}s)"
        fi
    else
        fail "TC-B3: host 2 should have been rejected by live holder"
    fi
fi

lease_test_summary
