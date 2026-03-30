#!/bin/bash
# tc-m-ballot.sh — Group M: Ballot-based lease acquisition (Disk Paxos Phase 1).
#
# These tests verify the ballot-based prepare→verify→acquire→conflict protocol
# that replaced the old write-settle(1s)-verify protocol.
#
# TC-M1: Stale prepare from crashed host doesn't block new mounts.
#        H3 writes a FL_PREPARE record and "crashes" (exits before acquire).
#        H5 should mount RW quickly — the stale prepare is superseded by
#        H5's higher ballot during verify_prepare.
#
# TC-M2: Concurrent mount race — exactly one winner (highest ballot).
#        Same as TC-F4 but specifically validates ballot-based behavior.
#        Three hosts start simultaneously; exactly one wins.
#
# TC-M3: Uncontested mount is faster without settle sleep.
#        A single host mounts RW from a clean slate.  Should complete
#        significantly faster than the old 1s settle sleep would allow.

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group M: Ballot-based lease acquisition ==="

# ---------------------------------------------------------------------------
# TC-M1: Stale prepare from crashed host doesn't block new mounts
# ---------------------------------------------------------------------------
echo ""
echo "TC-M1: stale prepare from crashed host doesn't block"

# Plant a FL_PREPARE record for host 3 with generation=1.
# The writer (host 1) mounts, writes the prepare sector, then unmounts
# cleanly (its own sector is zeroed).  Host 3's sector remains as a
# FL_PREPARE with ballot = 1*254+3 = 257.
tmpout=$(mktemp)
"$PFS_LEASE_HOLD" "$LEASE_TEST_CLUSTER" "$TEST_LOOP_DEVICE_NAME" \
    1 write-prepare-record 3 1 >"$tmpout" 2>>"$PFS_LOG"
rv=$?
if [[ $rv -ne 0 ]] || ! grep -q "^PREPARE_WRITTEN$" "$tmpout"; then
    rm -f "$tmpout"
    fail "TC-M1: could not plant stale prepare record for host 3"
else
    rm -f "$tmpout"

    # Host 5 mounts RW.  It should succeed quickly because:
    # 1. pfs_rw_lease_check ignores FL_PREPARE (only checks FL_RW)
    # 2. verify_prepare sees host 3's mbal, but host 5's ballot
    #    (gen=1*254+5=259) > host 3's (257), so it proceeds
    before=$SECONDS
    if start_rw_holder 5; then
        elapsed=$(( SECONDS - before ))
        stop_holder
        if [[ $elapsed -lt $((LEASE_TEST_DURATION - 1)) ]]; then
            pass "TC-M1: H5 mounted despite stale prepare from H3 (${elapsed}s)"
        else
            fail "TC-M1: H5 mount took too long (${elapsed}s, expected < $((LEASE_TEST_DURATION - 1))s)"
        fi
    else
        fail "TC-M1: H5 failed to mount with stale prepare from H3"
    fi
fi

# ---------------------------------------------------------------------------
# TC-M2: Concurrent mount race — exactly one winner
# ---------------------------------------------------------------------------
echo ""
echo "TC-M2: concurrent ballot race — exactly one winner"

# Same structure as TC-F4 but from a clean slate.
# Three hosts start simultaneously; the ballot protocol ensures exactly
# one wins (highest ballot = highest generation * num_hosts + host_id).
tmpout6=$(mktemp); tmpout7=$(mktemp); tmpout8=$(mktemp)
lease_hold 6 rw >"$tmpout6" & PID6=$!
lease_hold 7 rw >"$tmpout7" & PID7=$!
lease_hold 8 rw >"$tmpout8" & PID8=$!

# Give enough time for the winner to mount and losers to fail.
# Losers may need up to LEASE_TEST_DURATION for wait_and_check if they
# see the winner's RW record as live during the check phase.
sleep $((LEASE_TEST_DURATION + 3))

winners=(); loser_pids=()
for tuple in "6:$PID6:$tmpout6" "7:$PID7:$tmpout7" "8:$PID8:$tmpout8"; do
    hostid="${tuple%%:*}"; rest="${tuple#*:}"; pid="${rest%%:*}"; tmpf="${rest#*:}"
    if grep -q "^MOUNTED$" "$tmpf" 2>/dev/null; then
        winners+=("$hostid:$pid")
    else
        loser_pids+=("$pid")
    fi
    rm -f "$tmpf"
done

# Stop winners cleanly
for entry in "${winners[@]:-}"; do
    pid="${entry#*:}"
    [[ -n "$pid" ]] && { kill -TERM "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }
done
# Force-kill any losers still in flight
for pid in "${loser_pids[@]:-}"; do
    [[ -n "$pid" ]] && { kill -KILL "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }
done

nwon=${#winners[@]}
if [[ $nwon -eq 1 ]]; then
    pass "TC-M2: exactly 1 of 3 concurrent mounts won (host ${winners[0]%%:*})"
elif [[ $nwon -eq 0 ]]; then
    fail "TC-M2: no host won the concurrent ballot race (all failed)"
else
    winner_hosts=""
    for e in "${winners[@]}"; do winner_hosts+="${e%%:*} "; done
    fail "TC-M2: split-brain — $nwon hosts mounted RW simultaneously (hosts $winner_hosts)"
fi

# ---------------------------------------------------------------------------
# TC-M3: Uncontested mount completes without 1s settle delay
# ---------------------------------------------------------------------------
echo ""
echo "TC-M3: uncontested mount — no settle sleep overhead"

# Measure wall-clock time for a single RW mount from clean slate.
# The ballot protocol has no mandatory sleep; the old protocol slept 1s.
# We verify the mount completes in a reasonable time (< LEASE_TEST_DURATION-1).
before=$SECONDS
if start_rw_holder 10; then
    elapsed=$(( SECONDS - before ))
    stop_holder
    if [[ $elapsed -lt $((LEASE_TEST_DURATION - 1)) ]]; then
        pass "TC-M3: uncontested RW mount completed in ${elapsed}s (no settle sleep)"
    else
        fail "TC-M3: uncontested RW mount took ${elapsed}s (expected < $((LEASE_TEST_DURATION - 1))s)"
    fi
else
    fail "TC-M3: uncontested RW mount failed"
fi

# ---------------------------------------------------------------------------
# TC-M4: Higher-ballot stale prepare causes immediate EBUSY
# ---------------------------------------------------------------------------
echo ""
echo "TC-M4: higher-ballot stale prepare blocks mount with EBUSY"

# Plant a FL_PREPARE record for host 3 with generation=2.
# Host 3's ballot = 2*254+3 = 511.
# Host 5's initial gen=1 → ballot = 1*254+5 = 259 < 511.
# verify_prepare returns -EBUSY immediately (no retry).
tmpout=$(mktemp)
"$PFS_LEASE_HOLD" "$LEASE_TEST_CLUSTER" "$TEST_LOOP_DEVICE_NAME" \
    1 write-prepare-record 3 2 >"$tmpout" 2>>"$PFS_LOG"
rv=$?
if [[ $rv -ne 0 ]] || ! grep -q "^PREPARE_WRITTEN$" "$tmpout"; then
    rm -f "$tmpout"
    fail "TC-M4: could not plant prepare record for host 3 (gen=2)"
else
    rm -f "$tmpout"

    # Host 5 should fail fast — verify_prepare sees higher ballot, returns
    # -EBUSY, pfs_leader_load clears sector and returns -EBUSY.
    before=$SECONDS
    if start_rw_holder 5; then
        elapsed=$(( SECONDS - before ))
        stop_holder
        fail "TC-M4: H5 should have been blocked by H3's higher ballot (got ${elapsed}s)"
    else
        elapsed=$(( SECONDS - before ))
        if [[ $elapsed -lt $((LEASE_TEST_DURATION - 1)) ]]; then
            pass "TC-M4: H5 correctly rejected by H3's higher-ballot prepare (${elapsed}s, fast fail)"
        else
            fail "TC-M4: H5 rejected but took too long (${elapsed}s, expected fast fail)"
        fi
    fi

    # Clean up the stale prepare: mount as host 3 itself (reads its own
    # old gen=2, bumps to gen=3, ballot=3*254+3=765 > 511 → succeeds)
    if start_rw_holder 3; then
        stop_holder
    fi
fi

lease_test_summary
