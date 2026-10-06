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
# TC-M6: A host stalled between verify and acquire cannot mount.  H12
#        passes verify and pauses before acquire until its PREPARE has
#        expired; H11 (lower ballot) mounts.  When H12 resumes, it must not
#        mount next to H11.
#
# TC-M7: Holder clock ahead of the reader.  H11 writes timestamps 3s in the
#        reader's future; H12 must still see H11 as live and be refused.
#
# TC-M8: Holder clock behind the reader within paxos_clock_skew_max.  H11's
#        timestamps lag by LEASE_TEST_DURATION seconds, so its record looks
#        at least a lease duration old right after each renewal.  H12 runs
#        with paxos_clock_skew_max=LEASE_TEST_DURATION and must be refused.
#
# TC-M9: Same as TC-M6, but H12's pause emulates a system suspend, which
#        CLOCK_MONOTONIC does not count.  H12 must still see that its
#        PREPARE has expired and not mount next to H11.

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

# Shared by TC-M5, TC-M6 and TC-M9: a "late" host H<late> runs
# pfs_lease_hold in a delay mode (pid $late_pid, stdout in $late_out) and
# pauses at <STEP>_DELAY while H<holder> mounts RW (HOLDER_PID).  Checks
# that the late host's mount is refused with EBUSY once it resumes, having
# logged every <pattern> after byte <log_offset> of $PFS_LOG (see
# refusal_problem), and that the holder keeps the lease.
# Usage: check_late_host <tc> <STEP> <late> <late_pid> <late_out> <holder>
#                        <budget_s> <log_offset> <pattern>...
check_late_host() {
    local tc="$1" step="$2" late="$3" late_pid="$4" late_out="$5"
    local holder="$6" budget="$7" offset="$8"
    shift 8
    local problem

    if grep -q "^${step}_RESUMED$" "$late_out" 2>/dev/null; then
        fail "$tc: inconclusive — H${late} resumed before H${holder} finished mounting (raise the delay)"
        return
    fi

    local late_rv=""
    local deadline=$((SECONDS + budget))
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -q "^MOUNTED$" "$late_out" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$late_pid" 2>/dev/null; then
            late_rv=0
            wait "$late_pid" 2>/dev/null || late_rv=$?
            break
        fi
        sleep 0.2
    done

    if grep -q "^MOUNTED$" "$late_out" 2>/dev/null; then
        fail "$tc: split-brain — H${late} mounted RW while H${holder} holds the lease"
        return
    elif [[ -z "$late_rv" ]]; then
        fail "$tc: H${late} neither mounted nor exited after its delay"
        return
    elif ! kill -0 "$HOLDER_PID" 2>/dev/null; then
        fail "$tc: H${holder} lost its RW mount"
        return
    fi

    problem=$(refusal_problem "$late_pid" "$late_rv" "$late_out" "$offset" \
        "$EBUSY" "$@")
    if [[ -n "$problem" ]]; then
        fail "$tc: H${late} did not mount, but $problem"
    else
        pass "$tc: H${late} refused (errno $EBUSY), H${holder} keeps the lease"
    fi
}

# Wait up to <budget_s> for "<STEP>_DELAY" in <out> while <pid> is alive.
wait_for_delay() {
    local step="$1" pid="$2" out="$3" budget="$4"
    local deadline=$((SECONDS + budget))
    while [[ $SECONDS -lt $deadline ]]; do
        grep -q "^${step}_DELAY$" "$out" 2>/dev/null && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}

# ---------------------------------------------------------------------------
# TC-M5: Late higher-ballot host cannot take over an established holder
# ---------------------------------------------------------------------------
echo ""
echo "TC-M5: late higher-ballot host blocked by established holder"

# Both hosts start at generation 1, so H12's ballot (1*254+12) is higher
# than H11's (1*254+11).  H12 runs in rw-delay-prepare mode: it sleeps
# after its live-holder check (no holder yet) and before writing its
# prepare.  H11 mounts during that window.
M5_DELAY_MS=15000
m5_out=$(mktemp)
m5_log=$(log_offset)

lease_hold 12 rw-delay-prepare "$M5_DELAY_MS" >"$m5_out" &
M5_PID=$!

if ! wait_for_delay PREPARE "$M5_PID" "$m5_out" $((LEASE_TEST_DURATION + 20)); then
    fail "TC-M5: H12 never reached the pre-prepare delay"
elif ! start_rw_holder 11; then
    fail "TC-M5: H11 could not mount RW while H12 was paused"
else
    check_late_host TC-M5 PREPARE 12 "$M5_PID" "$m5_out" 11 \
        $((M5_DELAY_MS / 1000 + LEASE_TEST_DURATION + 20)) "$m5_log" \
        'rw_lease_verify_prepare: host 11 holds fresh RW lease'
    stop_holder
fi

kill -TERM "$M5_PID" 2>/dev/null || true
wait "$M5_PID" 2>/dev/null || true
rm -f "$m5_out"

# ---------------------------------------------------------------------------
# TC-M6: Host stalled between verify and acquire cannot mount
# ---------------------------------------------------------------------------
echo ""
echo "TC-M6: host stalled after verify cannot mount once its prepare expired"

# H12 (higher ballot) prepares and passes verify, then sleeps before writing
# its RW record.  Once its PREPARE is past the expiry age
# (LEASE_TEST_DURATION + LEASE_TEST_CLOCK_SKEW + 1), H11 ignores it as stale
# and mounts.  When H12 resumes, its acquire and conflict check see only
# H11's lower ballot; it must still not mount.
m6_stale=$(( LEASE_TEST_DURATION + LEASE_TEST_CLOCK_SKEW + 2 ))
M6_DELAY_MS=$(( (m6_stale + 25) * 1000 ))
m6_out=$(mktemp)
m6_log=$(log_offset)

lease_hold 12 rw-delay-acquire "$M6_DELAY_MS" >"$m6_out" &
M6_PID=$!

if ! wait_for_delay ACQUIRE "$M6_PID" "$m6_out" $((LEASE_TEST_DURATION + 20)); then
    fail "TC-M6: H12 never reached the pre-acquire delay"
else
    echo "  Waiting ${m6_stale}s for H12's prepare to expire..."
    sleep "$m6_stale"
    if ! start_rw_holder 11; then
        fail "TC-M6: H11 could not mount RW after H12's prepare expired"
    else
        check_late_host TC-M6 ACQUIRE 12 "$M6_PID" "$m6_out" 11 \
            $((M6_DELAY_MS / 1000 + LEASE_TEST_DURATION + 20)) "$m6_log" \
            'rw_lease: host_id=12 prepare expired before acquire completed'
        stop_holder
    fi
fi

kill -TERM "$M6_PID" 2>/dev/null || true
wait "$M6_PID" 2>/dev/null || true
rm -f "$m6_out"

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

# ---------------------------------------------------------------------------
# TC-M9: Host suspended between verify and acquire cannot mount
# ---------------------------------------------------------------------------
echo ""
echo "TC-M9: host suspended after verify cannot mount once its prepare expired"

# As TC-M6, with rw-suspend-acquire instead of rw-delay-acquire: H12's own
# deadline clock must count the emulated suspend.
m9_stale=$(( LEASE_TEST_DURATION + LEASE_TEST_CLOCK_SKEW + 2 ))
M9_DELAY_MS=$(( (m9_stale + 25) * 1000 ))
m9_out=$(mktemp)
m9_log=$(log_offset)

lease_hold 12 rw-suspend-acquire "$M9_DELAY_MS" >"$m9_out" &
M9_PID=$!

if ! wait_for_delay ACQUIRE "$M9_PID" "$m9_out" $((LEASE_TEST_DURATION + 20)); then
    fail "TC-M9: H12 never reached the pre-acquire suspend"
else
    echo "  Waiting ${m9_stale}s for H12's prepare to expire..."
    sleep "$m9_stale"
    if ! start_rw_holder 11; then
        fail "TC-M9: H11 could not mount RW after H12's prepare expired"
    else
        check_late_host TC-M9 ACQUIRE 12 "$M9_PID" "$m9_out" 11 \
            $((M9_DELAY_MS / 1000 + LEASE_TEST_DURATION + 20)) "$m9_log" \
            'rw_lease: host_id=12 prepare expired before acquire completed'
        stop_holder
    fi
fi

kill -TERM "$M9_PID" 2>/dev/null || true
wait "$M9_PID" 2>/dev/null || true
rm -f "$m9_out"

lease_test_summary
