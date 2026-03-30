#!/bin/bash
# tc-f-promotion.sh — Group F: RO-to-RW promotion and RW-remount tests.
#
# These tests cover the replica-promotion scenario: a PolarDB cluster has one
# RW primary and N RO replicas.  When the primary fails or steps down, one of
# the replicas must promote to RW.
#
# TC-F1: Same-host clean RW→RW remount (primary restarts with same host_id).
# TC-F2: RO replica promotes to RW after planned primary failover
#        (primary did a clean unmount → sector zeroed → immediate promotion).
# TC-F3: RO replica promotes to RW after primary crash
#        (primary crashed → stale record on disk → must wait for lease expiry).
# TC-F4: Multiple RO replicas race to promote concurrently.
#        The lease check must ensure at most one wins (no split-brain).
# TC-F5: RW re-promotion while sibling RO replicas remain mounted;
#        the RO holders must be undisturbed by the new RW mount.

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group F: RO-to-RW promotion ==="

# ---------------------------------------------------------------------------
# TC-F1: Same-host RW→RW clean remount (primary restart)
# ---------------------------------------------------------------------------
echo ""
echo "TC-F1: same-host RW clean remount"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-F1: initial RW mount as host 1 failed"
else
    stop_holder   # clean unmount → sector 1 zeroed

    # Same host_id mounts again — should succeed immediately
    before=$SECONDS
    if start_rw_holder 1; then
        elapsed=$(( SECONDS - before ))
        stop_holder
        if [[ $elapsed -lt $((LEASE_TEST_DURATION - 1)) ]]; then
            pass "TC-F1: host 1 RW remount succeeded immediately (${elapsed}s)"
        else
            fail "TC-F1: host 1 RW remount took too long (${elapsed}s, expected < $((LEASE_TEST_DURATION - 1))s)"
        fi
    else
        fail "TC-F1: host 1 RW remount failed after clean unmount"
    fi
fi

# ---------------------------------------------------------------------------
# TC-F2: RO replica promotes to RW after planned primary failover
# ---------------------------------------------------------------------------
echo ""
echo "TC-F2: RO replica promotes to RW after planned primary failover"

# Simulate: H1 was primary (RW), does planned shutdown (clean unmount).
# H2 was a replica (RO holder).  H2 promotes to RW.
HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-F2: setup: could not start primary as host 1"
else
    H1_PID="$HOLDER_PID"

    # H2 is an RO replica that stays mounted throughout
    if ! start_ro_holder 2; then
        stop_holder "$H1_PID"
        fail "TC-F2: setup: could not start replica as host 2 RO"
    else
        H2_RO_PID="$HOLDER_PID"

        # Planned primary failover: H1 cleanly unmounts
        stop_holder "$H1_PID"

        # H2 stops its RO mount and promotes to RW
        stop_holder "$H2_RO_PID"

        before=$SECONDS
        if start_rw_holder 2; then
            elapsed=$(( SECONDS - before ))
            stop_holder
            if [[ $elapsed -lt $((LEASE_TEST_DURATION - 1)) ]]; then
                pass "TC-F2: H2 promoted to RW immediately after planned H1 failover (${elapsed}s)"
            else
                fail "TC-F2: H2 promotion took too long (${elapsed}s)"
            fi
        else
            fail "TC-F2: H2 RW promotion failed after planned H1 failover"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# TC-F3: RO replica promotes to RW after primary crash
# ---------------------------------------------------------------------------
echo ""
echo "TC-F3: RO replica promotes to RW after primary crash"

# Simulate: H1 was primary, crashed (SIGKILL).  H2 promotes after lease expires.
HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-F3: setup: could not start primary as host 1"
else
    sleep 1   # let one renewal tick so the timestamp is fresh at kill time
    kill -KILL "$HOLDER_PID" 2>/dev/null || true
    wait "$HOLDER_PID" 2>/dev/null || true
    HOLDER_PID=""

    # H2 must wait for the stale lease to expire before it can promote
    echo "  Waiting ${LEASE_TEST_DURATION}s for stale lease to expire..."
    before=$SECONDS
    if start_rw_holder 2; then
        elapsed=$(( SECONDS - before ))
        stop_holder
        pass "TC-F3: H2 promoted to RW after H1 crash+expiry (waited ${elapsed}s)"
    else
        fail "TC-F3: H2 failed to promote after H1 crash"
    fi
fi

# ---------------------------------------------------------------------------
# TC-F4: Multiple RO replicas race to promote — exactly one must win
# ---------------------------------------------------------------------------
echo ""
echo "TC-F4: concurrent promotion race — exactly one winner, no split-brain"

# Start three candidate promoters simultaneously from a clean slate.
# The lease mechanism must ensure at most one obtains the RW lease.
#
# The ballot-based protocol (Disk Paxos Phase 1) resolves concurrent
# acquirers by ballot number: the host with the highest ballot wins.
# Ballots are generation * num_hosts + host_id, guaranteeing uniqueness.
# A split-brain outcome here (nwon > 1) is a real bug worth reporting.
tmpout2=$(mktemp); tmpout3=$(mktemp); tmpout4=$(mktemp)
lease_hold 2 rw >"$tmpout2" & PID2=$!
lease_hold 3 rw >"$tmpout3" & PID3=$!
lease_hold 4 rw >"$tmpout4" & PID4=$!

# Give enough time for the winner to mount and begin renewing, which causes
# the losers' pfs_rw_lease_wait_and_check to return EBUSY quickly (~2s).
sleep $((LEASE_TEST_DURATION + 3))

winners=(); loser_pids=()
for tuple in "2:$PID2:$tmpout2" "3:$PID3:$tmpout3" "4:$PID4:$tmpout4"; do
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
    pass "TC-F4: exactly 1 of 3 concurrent promotions won (host ${winners[0]%%:*})"
elif [[ $nwon -eq 0 ]]; then
    fail "TC-F4: no host won the concurrent promotion race (all failed)"
else
    winner_hosts=""
    for e in "${winners[@]}"; do winner_hosts+="${e%%:*} "; done
    fail "TC-F4: split-brain — $nwon hosts mounted RW simultaneously (hosts $winner_hosts)"
fi

# ---------------------------------------------------------------------------
# TC-F5: RW re-promotion while sibling RO replicas remain mounted
# ---------------------------------------------------------------------------
echo ""
echo "TC-F5: new RW promotion does not disturb existing RO replicas"

# Start RO replicas on hosts 3 and 4
HOLDER3_PID=""
HOLDER4_PID=""
if ! start_ro_holder 3; then
    fail "TC-F5: setup: could not start RO replica as host 3"
else
    HOLDER3_PID="$HOLDER_PID"
    if ! start_ro_holder 4; then
        stop_holder "$HOLDER3_PID"
        fail "TC-F5: setup: could not start RO replica as host 4"
    else
        HOLDER4_PID="$HOLDER_PID"

        # Host 1 promotes to RW
        if ! start_rw_holder 1; then
            stop_holder "$HOLDER3_PID"
            stop_holder "$HOLDER4_PID"
            fail "TC-F5: RW promotion by host 1 failed while RO replicas are up"
        else
            H1_PID="$HOLDER_PID"

            # Verify RO replicas are still alive
            h3_alive=false; h4_alive=false
            kill -0 "$HOLDER3_PID" 2>/dev/null && h3_alive=true
            kill -0 "$HOLDER4_PID" 2>/dev/null && h4_alive=true

            # H1 does work (touch a file), then cleanly unmounts
            pfs_cmd 1 touch "/${TEST_LOOP_DEVICE_NAME}/tc_f5_probe" 2>/dev/null || true
            stop_holder "$H1_PID"

            # H1 re-promotes (e.g., planned re-election keeps same host)
            if ! start_rw_holder 1; then
                stop_holder "$HOLDER3_PID"
                stop_holder "$HOLDER4_PID"
                fail "TC-F5: H1 RW re-promotion failed"
            else
                stop_holder   # stop H1's second RW mount

                # Clean up RO replicas
                stop_holder "$HOLDER3_PID"
                stop_holder "$HOLDER4_PID"

                if $h3_alive && $h4_alive; then
                    pass "TC-F5: RW promotion and re-promotion succeeded with RO replicas undisturbed"
                else
                    fail "TC-F5: at least one RO replica died unexpectedly during RW promotion"
                fi
            fi
        fi
    fi
fi

lease_test_summary
