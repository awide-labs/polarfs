#!/bin/bash
# Group N — Timer-kill watchdog tests.
#
# TC-N1: Watchdog kills stalled holder.
#   Mount H1 RW (watchdog armed).  Suspend log thread via watchdog-stall
#   mode (stops renewal petting, watchdog stays armed).  Verify the
#   process is killed by the timer after paxos_lease_duration seconds.
#
# TC-N2: Second host mounts after watchdog kill.
#   After H1 is killed by the watchdog (TC-N1), mount H2 RW.  H2's
#   wait_and_check should detect H1's stale record and succeed.
#   This proves the watchdog kill achieves proper fencing — the killed
#   host's lease record is left on disk (not zeroed), and a new host
#   can take over after the lease expires.

source "$(dirname "$0")/common.sh"
lease_test_setup
trap lease_test_teardown EXIT

# ── TC-N1: Watchdog kills stalled holder ──────────────────────────────

echo ""
echo "--- TC-N1: watchdog kills stalled holder ---"

if ! start_watchdog_stall_holder 1; then
    fail "TC-N1" "could not mount H1 in watchdog-stall mode"
else
    # Let H1 renew at least once so the lease record has a fresh timestamp
    sleep 1

    t0=$SECONDS
    if trigger_watchdog_stall; then
        elapsed=$((SECONDS - t0))
        # The watchdog should fire after paxos_lease_duration seconds.
        # Allow some slack but verify it didn't take excessively long.
        if [[ $elapsed -lt $((LEASE_TEST_DURATION + 5)) ]]; then
            pass "TC-N1: H1 killed by watchdog in ${elapsed}s"
        else
            fail "TC-N1" "H1 killed but took too long (${elapsed}s, expected ~${LEASE_TEST_DURATION}s)"
        fi
    else
        fail "TC-N1" "H1 was not killed by watchdog"
    fi
fi

# Verify the stale lease record is still on disk (not zeroed — the
# watchdog kill is unclean, like a crash).
echo ""
echo "--- TC-N1b: stale sector not zeroed after watchdog kill ---"

ts=$(read_lease_timestamp_raw 1)
if [[ -n "$ts" && "$ts" -gt 0 ]]; then
    pass "TC-N1b: H1 sector has non-zero timestamp ($ts)"
else
    fail "TC-N1b" "H1 sector timestamp is zero or unreadable (expected stale record)"
fi

# ── TC-N2: Second host mounts after watchdog kill ─────────────────────

echo ""
echo "--- TC-N2: H2 mounts RW after H1 watchdog kill ---"

t0=$SECONDS
if start_rw_holder 2; then
    elapsed=$((SECONDS - t0))
    # H2 should succeed after wait_and_check detects H1's stale record.
    # Budget: meta_load (up to 8s) + wait_and_check (up to LEASE_TEST_DURATION)
    # + acquire/settle/log (up to 5s).
    if [[ $elapsed -lt $((LEASE_TEST_DURATION + 15)) ]]; then
        pass "TC-N2: H2 mounted RW in ${elapsed}s after H1 watchdog kill"
    else
        fail "TC-N2" "H2 mounted but took too long (${elapsed}s)"
    fi
    stop_holder
else
    fail "TC-N2" "H2 could not mount RW after H1 watchdog kill"
fi

# ── TC-N1 log diagnostic check ───────────────────────────────────────

echo ""
echo "--- TC-N1c: watchdog diagnostic message in log ---"

assert_log_contains "TC-N1c: watchdog message in log" \
    "RW lease watchdog expired"

lease_test_summary
