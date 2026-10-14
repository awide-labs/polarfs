#!/bin/bash
# Group N — Timer-kill watchdog tests.
#
# TC-N1: Watchdog kills stalled holder.
#   Mount H1 RW (watchdog armed).  Suspend log thread via watchdog-stall
#   mode (stops renewal petting, watchdog stays armed).  Verify the
#   process is killed by SIGKILL (raised by the timer's signal handler)
#   after paxos_lease_duration seconds.
#
# TC-N2: Second host mounts after watchdog kill.
#   After H1 is killed by the watchdog (TC-N1), mount H2 RW.  H2's
#   wait_and_check should detect H1's stale record and succeed.
#   This proves the watchdog kill achieves proper fencing — the killed
#   host's lease record is left on disk (not zeroed), and a new host
#   can take over after the lease expires.
#
# TC-N3: RW mount refused when the kill timer cannot be created.
#   Nothing would fence the host if it stalled, so it must give the
#   lease back (zero its sector) and fail the mount.
#
# TC-N4: RW mount refused when the kill timer cannot be armed.
#   Same as TC-N3, with timer creation succeeding.
#
# TC-N5: Holder killed when renewals cannot re-arm the kill timer.
#   The deadline armed at acquisition stands, so the holder is killed
#   within paxos_lease_duration of mounting even though it keeps
#   renewing.

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

# ── TC-N3 / TC-N4: no kill timer, no RW mount ────────────────────────

# Each host must have written its RW record before giving it back, so the
# zeroed sector shows the release rather than a sector never written.
# timer_create's EAGAIN is reported as ENOMEM.

echo ""
echo "--- TC-N3: RW mount refused when the kill timer cannot be created ---"

expect_mount_refused TC-N3 3 "the kill timer could not be created" "$ENOMEM" \
    'rw_lease_acquire: acquired RW lease host_id=3 ' \
    'watchdog_open: timer_create failed: -11' \
    'rw_lease: host_id=3 cannot arm lease watchdog, giving up' \
    -- rw-kill-timer-fail create
assert_sector_zeroed "TC-N3b: H3 gave its lease back" 3

echo ""
echo "--- TC-N4: RW mount refused when the kill timer cannot be armed ---"

expect_mount_refused TC-N4 4 "the kill timer could not be armed" "$EINVAL" \
    'rw_lease_acquire: acquired RW lease host_id=4 ' \
    'watchdog: arming kill timer failed: -22' \
    'rw_lease: host_id=4 cannot arm lease watchdog, giving up' \
    -- rw-kill-timer-fail arm
assert_sector_zeroed "TC-N4b: H4 gave its lease back" 4

# ── TC-N5: renewals cannot re-arm the kill timer ─────────────────────

echo ""
echo "--- TC-N5: holder killed when renewals cannot re-arm the kill timer ---"

if ! start_rw_holder 5 rw-kill-timer-fail rearm; then
    fail "TC-N5" "could not mount H5"
else
    t0=$SECONDS
    deadline=$((SECONDS + LEASE_TEST_DURATION + 5))
    while [[ $SECONDS -lt $deadline ]] && kill -0 "$HOLDER_PID" 2>/dev/null; do
        sleep 0.2
    done
    elapsed=$((SECONDS - t0))
    if kill -0 "$HOLDER_PID" 2>/dev/null; then
        fail "TC-N5" "H5 still alive ${elapsed}s after mounting"
        stop_holder
    else
        rv=0
        wait "$HOLDER_PID" 2>/dev/null || rv=$?
        HOLDER_PID=""
        if [[ $rv -eq 137 ]]; then
            pass "TC-N5: H5 killed by watchdog ${elapsed}s after mounting"
        else
            fail "TC-N5" "H5 exited with status $rv, expected 137 (SIGKILL)"
        fi
    fi
fi

# ── TC-N1 log diagnostic check ───────────────────────────────────────

echo ""
echo "--- TC-N1c: watchdog diagnostic message in log ---"

assert_log_contains "TC-N1c: watchdog message in log" \
    "RW lease watchdog expired"

lease_test_summary
