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
# TC-M2: Concurrent mount race — at most one winner per round.
#        Three hosts start simultaneously; at most one may mount.  A round
#        can end with every host yielding (EBUSY), so it is retried until
#        one wins.
#
# TC-M3: Uncontested mount is faster without settle sleep.
#        A single host mounts RW from a clean slate.  Should complete
#        significantly faster than the old 1s settle sleep would allow.
#
# TC-M4: Higher-ballot stale prepare causes immediate EBUSY.
#
# TC-M5: A late higher-ballot host cannot take over an established holder.
#        H12 passes the live-holder check and pauses before prepare; H11
#        (lower ballot) completes acquisition and mounts RW meanwhile.  When
#        H12 resumes, its higher ballot must not let it mount next to H11.
#
# TC-M7: Holder clock ahead of the reader.  H11 writes timestamps 3s in the
#        reader's future; H12 must still see H11 as live and be refused.
#
# TC-M8: Holder clock behind the reader within paxos_clock_skew_max.  H11's
#        timestamps lag by LEASE_TEST_DURATION seconds, so its record looks
#        at least a lease duration old right after each renewal.  H12 runs
#        with paxos_clock_skew_max=LEASE_TEST_DURATION and must be refused.

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
rv=0
"$PFS_LEASE_HOLD" "$LEASE_TEST_CLUSTER" "$TEST_LOOP_DEVICE_NAME" \
    1 write-prepare-record 3 1 >"$tmpout" 2>>"$PFS_LOG" || rv=$?
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

    # Clear the planted prepare: while live, its ballot (257) preempts any
    # lower-ballot host, e.g. TC-M4's writer.  H3 mounting (reads its own
    # gen=1, bumps to gen=2) and unmounting cleanly zeroes its sector.
    if start_rw_holder 3; then
        stop_holder
    else
        fail "TC-M1: cleanup: H3 could not mount to clear its planted prepare"
    fi
fi

# ---------------------------------------------------------------------------
# TC-M2: Concurrent mount race — at most one winner per round
# ---------------------------------------------------------------------------
echo ""
echo "TC-M2: concurrent ballot race — at most one winner, eventually one"

race_rw_mounts TC-M2 6 7 8

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
rv=0
"$PFS_LEASE_HOLD" "$LEASE_TEST_CLUSTER" "$TEST_LOOP_DEVICE_NAME" \
    1 write-prepare-record 3 2 >"$tmpout" 2>>"$PFS_LOG" || rv=$?
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

# ---------------------------------------------------------------------------
# TC-M5: Late higher-ballot host cannot take over an established holder
# ---------------------------------------------------------------------------
echo ""
echo "TC-M5: late higher-ballot host blocked by established holder"

# Both hosts start at generation 1, so H12's ballot (1*254+12) is higher
# than H11's (1*254+11).  H12 runs in rw-delay-prepare mode: it sleeps
# after its live-holder check (no holder yet) and before writing its
# prepare.  H11 mounts during that window.
M5_LOW=11
M5_HIGH=12
M5_DELAY_MS=15000
m5_out=$(mktemp)

lease_hold "$M5_HIGH" rw-delay-prepare "$M5_DELAY_MS" >"$m5_out" &
M5_HIGH_PID=$!

# Wait for H12 to pass its live-holder check and enter the delay.
m5_paused=0
deadline=$((SECONDS + LEASE_TEST_DURATION + 20))
while [[ $SECONDS -lt $deadline ]]; do
    if grep -q "^PREPARE_DELAY$" "$m5_out" 2>/dev/null; then
        m5_paused=1
        break
    fi
    kill -0 "$M5_HIGH_PID" 2>/dev/null || break
    sleep 0.2
done

if [[ $m5_paused -ne 1 ]]; then
    fail "TC-M5: H${M5_HIGH} never reached the pre-prepare delay"
elif ! start_rw_holder "$M5_LOW"; then
    fail "TC-M5: H${M5_LOW} could not mount RW while H${M5_HIGH} was paused"
elif grep -q "^PREPARE_RESUMED$" "$m5_out" 2>/dev/null; then
    stop_holder
    fail "TC-M5: inconclusive — H${M5_HIGH} resumed before H${M5_LOW} finished mounting (raise M5_DELAY_MS)"
else
    # H11 holds the lease.  Let H12 resume and see whether it mounts.
    m5_high_rv=""
    deadline=$((SECONDS + M5_DELAY_MS / 1000 + LEASE_TEST_DURATION + 20))
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -q "^MOUNTED$" "$m5_out" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$M5_HIGH_PID" 2>/dev/null; then
            m5_high_rv=0
            wait "$M5_HIGH_PID" 2>/dev/null || m5_high_rv=$?
            break
        fi
        sleep 0.2
    done

    if grep -q "^MOUNTED$" "$m5_out" 2>/dev/null; then
        fail "TC-M5: split-brain — H${M5_HIGH} mounted RW while H${M5_LOW} holds the lease"
    elif [[ -z "$m5_high_rv" ]]; then
        fail "TC-M5: H${M5_HIGH} neither mounted nor exited after its delay"
    elif ! kill -0 "$HOLDER_PID" 2>/dev/null; then
        fail "TC-M5: H${M5_LOW} lost its RW mount"
    else
        pass "TC-M5: H${M5_HIGH} rejected (exit $m5_high_rv), H${M5_LOW} keeps the lease"
    fi
    stop_holder
fi

kill -TERM "$M5_HIGH_PID" 2>/dev/null || true
wait "$M5_HIGH_PID" 2>/dev/null || true
rm -f "$m5_out"

# ---------------------------------------------------------------------------
# TC-M7: Holder clock ahead of the reader
# ---------------------------------------------------------------------------
echo ""
echo "TC-M7: holder clock 3s ahead — holder still blocks"

if ! start_rw_holder 11 rw-clock-offset 3; then
    fail "TC-M7: H11 could not mount RW with clock offset +3s"
else
    sleep 2   # let H11 renew
    expect_mount_refused TC-M7 12 "H11 (clock +3s) holds the lease" "$EBUSY" \
        'rw_lease_check: BUSY - host 11 holds live RW lease' \
        -- rw
    stop_holder
fi

# ---------------------------------------------------------------------------
# TC-M8: Holder clock behind the reader, within paxos_clock_skew_max
# ---------------------------------------------------------------------------
echo ""
echo "TC-M8: holder clock ${LEASE_TEST_DURATION}s behind, reader allows that skew — holder still blocks"

m8_skew=$LEASE_TEST_DURATION
m8_conf=$(mktemp /tmp/pfs-lease-test-conf.XXXXXX)
sed "s/^paxos_clock_skew_max=.*/paxos_clock_skew_max=${m8_skew}/" \
    "$LEASE_TEST_CONF" >"$m8_conf"

if ! start_rw_holder 11 rw-clock-offset "-${m8_skew}"; then
    fail "TC-M8: H11 could not mount RW with clock offset -${m8_skew}s"
else
    sleep 2   # let H11 renew
    # The skew= field shows H12 picked up its own paxos_clock_skew_max.
    PFS_CONFIG_PATH="$m8_conf" expect_mount_refused TC-M8 12 \
        "H11 (clock -${m8_skew}s) holds the lease" "$EBUSY" \
        "rw_lease_check: BUSY - host 11 holds live RW lease .*skew=${m8_skew}s" \
        -- rw
    stop_holder
fi
rm -f "$m8_conf"

lease_test_summary
