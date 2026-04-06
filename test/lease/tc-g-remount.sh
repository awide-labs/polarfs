#!/bin/bash
# tc-g-remount.sh — Group G: pfs_remount (live RO→RW promotion) tests.
#
# pfs_remount() promotes a live RO mount to RW in-place: the filesystem
# stays mounted, the log thread is only suspended (not stopped), and the
# lease acquire/release runs through pfs_leader_unload + pfs_leader_load
# on the same mount object.  This is the actual promotion path used by
# pfsd, as opposed to umount + mount.
#
# TC-G1: Basic pfs_remount on a clean FS — no competing RW holder,
#        promotion succeeds and the RW lease record appears on disk.
# TC-G2: pfs_remount blocked by live RW holder — returns EBUSY and restores
#        the mount to RO (does not terminate the process).
# TC-G3: pfs_remount after RW holder crash — waits for lease expiry then
#        succeeds (same wait-and-check path as a fresh RW mount attempt).
# TC-G4: Concurrent pfs_remount race — multiple RO replicas trigger
#        promotion simultaneously; at most one must win (no split-brain),
#        and every losing host's sector must be zeroed by conflict detection.
# TC-G5: pfs_remount cleans up correctly on unmount — after a successful
#        promotion the lease sector is zeroed on clean unmount.
# TC-G6: pfs_remount_ro (RW→RO demotion) — after pfs_mount_release() drops
#        the last RW client while an RO client remains, pfs_remount_ro() is
#        called internally; the RW lease sector must be zeroed afterwards.
# TC-G7: After RW→RO demotion the lease is available — a third host can
#        immediately acquire the RW lease without waiting for expiry.
# TC-G8: Mount usable after failed promote — after pfs_remount_rw returns
#        EBUSY, the restored RO mount must still be functional (stat on root
#        succeeds).  Regression test for missing pfs_notify_inited in the
#        EBUSY recovery path.

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group G: pfs_remount (live RO-to-RW promotion) ==="

# ---------------------------------------------------------------------------
# TC-G1: Basic pfs_remount succeeds — RW lease record appears on disk
# ---------------------------------------------------------------------------
echo ""
echo "TC-G1: basic pfs_remount promotes RO mount to RW"

HOLDER_PID=""
if ! start_promote_holder 1; then
    fail "TC-G1: could not start promote holder for host 1"
else
    if trigger_promote; then
        # Verify sector 1 is now non-zero (lease record written)
        sector_offset=$(paxos_sector_offset 1)
        tmp=$(mktemp)
        pfs_cmd 50 read -o "$sector_offset" -l "$LEASE_TEST_SECTOR_SIZE" \
            "/${TEST_LOOP_DEVICE_NAME}/.pfs-paxos" >"$tmp" 2>/dev/null || {
            rm -f "$tmp"; stop_holder
            fail "TC-G1: could not read paxos sector 1 after remount"
            lease_test_summary; exit 1
        }
        nonzero=$(od -v -t x1 -A n "$tmp" | tr -s ' \n' '\n' \
            | grep -v '^00$' | grep -v '^$' | wc -l) || true
        rm -f "$tmp"
        stop_holder

        if [[ "$nonzero" -gt 0 ]]; then
            pass "TC-G1: pfs_remount succeeded, RW lease record on disk ($nonzero non-zero bytes)"
        else
            fail "TC-G1: pfs_remount reported success but lease sector is still zero"
        fi
    else
        stop_holder
        fail "TC-G1: pfs_remount failed unexpectedly ($PROMOTE_RESULT)"
    fi
fi

# ---------------------------------------------------------------------------
# TC-G2: pfs_remount blocked by live RW holder — returns EBUSY
#
# The remount must not succeed.  Timing: under CI load pfs_remount_rw may
# spend up to LEASE_TEST_DURATION seconds in wait_and_check before the
# ballot conflict check catches the live holder, then ~2s to restore the
# RO mount.  Threshold: < D + 5.
# ---------------------------------------------------------------------------
echo ""
echo "TC-G2: pfs_remount blocked by live RW holder"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-G2: setup: could not start RW holder as host 1"
else
    H1_PID="$HOLDER_PID"
    sleep 2   # ensure H1 is actively renewing

    # H2 is RO, attempts promotion while H1 holds live RW lease
    if ! start_promote_holder 2; then
        stop_holder "$H1_PID"
        fail "TC-G2: could not start promote holder for host 2"
    else
        before=$SECONDS
        if trigger_promote; then
            elapsed=$(( SECONDS - before ))
            stop_holder       # stop H2 (promoted, shouldn't happen)
            stop_holder "$H1_PID"
            fail "TC-G2: pfs_remount should have been blocked by H1's live lease"
        else
            elapsed=$(( SECONDS - before ))
            stop_holder       # stop H2 (restored to RO after failed promote)
            stop_holder "$H1_PID"
            max_ok=$(( LEASE_TEST_DURATION + 5 ))
            if [[ $elapsed -lt $max_ok ]]; then
                pass "TC-G2: pfs_remount got EBUSY (${elapsed}s < ${max_ok}s) — live holder detected"
            else
                fail "TC-G2: pfs_remount EBUSY but took ${elapsed}s (expected < ${max_ok}s)"
            fi
        fi
    fi
fi

# ---------------------------------------------------------------------------
# TC-G3: pfs_remount after RW holder crash — waits for expiry, then succeeds
# ---------------------------------------------------------------------------
echo ""
echo "TC-G3: pfs_remount succeeds after RW holder crash + lease expiry"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-G3: setup: could not start RW holder as host 1"
else
    sleep 1   # let one renewal tick
    kill -KILL "$HOLDER_PID" 2>/dev/null || true
    wait "$HOLDER_PID" 2>/dev/null || true
    HOLDER_PID=""

    # H2 mounts RO (no blocking — stale record for H1, not H2)
    if ! start_promote_holder 2; then
        fail "TC-G3: could not start promote holder for host 2"
    else
        echo "  Triggering promotion (will wait for stale H1 lease to expire)..."
        before=$SECONDS
        if trigger_promote; then
            elapsed=$(( SECONDS - before ))
            stop_holder
            pass "TC-G3: pfs_remount succeeded after H1 crash+expiry (${elapsed}s)"
        else
            elapsed=$(( SECONDS - before ))
            stop_holder
            fail "TC-G3: pfs_remount failed after H1 crash ($PROMOTE_RESULT, ${elapsed}s)"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# TC-G4: Concurrent pfs_remount race — at most one winner
# ---------------------------------------------------------------------------
echo ""
echo "TC-G4: concurrent pfs_remount race — exactly one winner, no split-brain"

# Start three RO holders sequentially — wait for each "MOUNTED_RO" before
# launching the next.  This avoids a race where concurrent pfs_mount_acquire
# calls to the same device serialise internally and one times out.  The
# concurrent race we are testing is in the promote phase, not the mount phase.
wait_mounted_ro() {
    local pid="$1" tmpf="$2" hostid="$3"
    local deadline=$((SECONDS + 15))
    while [[ $SECONDS -lt $deadline ]]; do
        grep -q "^MOUNTED_RO$" "$tmpf" 2>/dev/null && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}

g4_ready=true
tmpout2=$(mktemp); tmpout3=$(mktemp); tmpout4=$(mktemp)
PID2="" PID3="" PID4=""

lease_hold 2 promote >"$tmpout2" & PID2=$!
if ! wait_mounted_ro "$PID2" "$tmpout2" 2; then
    g4_ready=false
fi

if $g4_ready; then
    lease_hold 3 promote >"$tmpout3" & PID3=$!
    if ! wait_mounted_ro "$PID3" "$tmpout3" 3; then
        g4_ready=false
    fi
fi

if $g4_ready; then
    lease_hold 4 promote >"$tmpout4" & PID4=$!
    if ! wait_mounted_ro "$PID4" "$tmpout4" 4; then
        g4_ready=false
    fi
fi

if ! $g4_ready; then
    for pid in ${PID2:-} ${PID3:-} ${PID4:-}; do
        [[ -n "$pid" ]] && { kill -KILL "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }
    done
    rm -f "$tmpout2" "$tmpout3" "$tmpout4"
    fail "TC-G4: not all three holders reached MOUNTED_RO"
else
    # Fire all three promotions simultaneously
    kill -USR1 $PID2 $PID3 $PID4 2>/dev/null || true

    # Wait for all three to settle (winner mounts, losers get EBUSY quickly)
    sleep $((LEASE_TEST_DURATION + 3))

    winners=(); loser_pids=(); loser_hostids=()
    for tuple in "2:$PID2:$tmpout2" "3:$PID3:$tmpout3" "4:$PID4:$tmpout4"; do
        hostid="${tuple%%:*}"; rest="${tuple#*:}"
        pid="${rest%%:*}"; tmpf="${rest#*:}"
        if grep -q "^MOUNTED_RW$" "$tmpf" 2>/dev/null; then
            winners+=("$hostid:$pid")
        else
            loser_pids+=("$pid")
            loser_hostids+=("$hostid")
        fi
        rm -f "$tmpf"
    done

    for entry in "${winners[@]:-}"; do
        local_pid="${entry#*:}"
        [[ -n "$local_pid" ]] && { kill -TERM "$local_pid" 2>/dev/null || true; wait "$local_pid" 2>/dev/null || true; }
    done
    for local_pid in "${loser_pids[@]:-}"; do
        [[ -n "$local_pid" ]] && { kill -KILL "$local_pid" 2>/dev/null || true; wait "$local_pid" 2>/dev/null || true; }
    done

    nwon=${#winners[@]}
    if [[ $nwon -eq 1 ]]; then
        pass "TC-G4: exactly 1 of 3 concurrent remounts won (host ${winners[0]%%:*})"
    elif [[ $nwon -eq 0 ]]; then
        fail "TC-G4: no host won the concurrent remount race (all failed)"
    else
        winner_hosts=""
        for e in "${winners[@]}"; do winner_hosts+="${e%%:*} "; done
        fail "TC-G4: split-brain — $nwon hosts promoted simultaneously (hosts $winner_hosts)"
    fi

    # Verify each losing host's paxos sector was zeroed by conflict detection.
    # The loser (lower ballot) is rejected at verify_prepare or check_conflict;
    # pfs_leader_load calls pfs_host_record_clear before returning, so the
    # clear completes before pfs_remount returns -EBUSY.
    for loser_hid in "${loser_hostids[@]:-}"; do
        sector_offset=$(paxos_sector_offset "$loser_hid")
        tmp=$(mktemp)
        pfs_cmd 50 read -o "$sector_offset" -l "$LEASE_TEST_SECTOR_SIZE" \
            "/${TEST_LOOP_DEVICE_NAME}/.pfs-paxos" >"$tmp" 2>/dev/null || {
            rm -f "$tmp"
            fail "TC-G4: could not read paxos sector for loser host $loser_hid"
            continue
        }
        nonzero=$(od -v -t x1 -A n "$tmp" | tr -s ' \n' '\n' \
            | grep -v '^00$' | grep -v '^$' | wc -l) || true
        rm -f "$tmp"
        if [[ "$nonzero" -eq 0 ]]; then
            pass "TC-G4: loser host $loser_hid sector zeroed after conflict detection"
        else
            fail "TC-G4: loser host $loser_hid sector has $nonzero non-zero bytes — not cleared after yielding"
        fi
    done
fi

# ---------------------------------------------------------------------------
# TC-G5: After pfs_remount, clean unmount zeroes the lease sector
# ---------------------------------------------------------------------------
echo ""
echo "TC-G5: clean unmount after pfs_remount zeroes the lease sector"

HOLDER_PID=""
if ! start_promote_holder 1; then
    fail "TC-G5: could not start promote holder"
else
    if ! trigger_promote; then
        stop_holder
        fail "TC-G5: pfs_remount failed ($PROMOTE_RESULT)"
    else
        # Clean unmount
        stop_holder

        sector_offset=$(paxos_sector_offset 1)
        tmp=$(mktemp)
        pfs_cmd 50 read -o "$sector_offset" -l "$LEASE_TEST_SECTOR_SIZE" \
            "/${TEST_LOOP_DEVICE_NAME}/.pfs-paxos" >"$tmp" 2>/dev/null || {
            rm -f "$tmp"
            fail "TC-G5: could not read paxos sector 1 after unmount"
            lease_test_summary; exit 1
        }
        nonzero=$(od -v -t x1 -A n "$tmp" | tr -s ' \n' '\n' \
            | grep -v '^00$' | grep -v '^$' | wc -l) || true
        rm -f "$tmp"

        if [[ "$nonzero" -eq 0 ]]; then
            pass "TC-G5: sector 1 zeroed after clean unmount of remounted RW holder"
        else
            fail "TC-G5: sector 1 has $nonzero non-zero bytes after clean unmount"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# TC-G6: pfs_remount_ro (RW→RO demotion) — lease sector zeroed after demote
# ---------------------------------------------------------------------------
echo ""
echo "TC-G6: pfs_remount_ro demotes RW mount to RO, RW lease sector zeroed"

HOLDER_PID=""
if ! start_demote_holder 1 2; then
    fail "TC-G6: could not start demote holder (rw=1, ro=2)"
else
    if trigger_demote; then
        # Verify sector 1 is now zeroed (RW lease released by pfs_remount_ro)
        sector_offset=$(paxos_sector_offset 1)
        tmp=$(mktemp)
        pfs_cmd 50 read -o "$sector_offset" -l "$LEASE_TEST_SECTOR_SIZE" \
            "/${TEST_LOOP_DEVICE_NAME}/.pfs-paxos" >"$tmp" 2>/dev/null || {
            rm -f "$tmp"; stop_holder
            fail "TC-G6: could not read paxos sector 1 after demote"
            lease_test_summary; exit 1
        }
        nonzero=$(od -v -t x1 -A n "$tmp" | tr -s ' \n' '\n' \
            | grep -v '^00$' | grep -v '^$' | wc -l) || true
        rm -f "$tmp"
        stop_holder

        if [[ "$nonzero" -eq 0 ]]; then
            pass "TC-G6: pfs_remount_ro succeeded, RW sector 1 zeroed"
        else
            fail "TC-G6: pfs_remount_ro reported success but sector 1 has $nonzero non-zero bytes"
        fi
    else
        stop_holder
        fail "TC-G6: pfs_remount_ro (demote) failed ($DEMOTE_RESULT)"
    fi
fi

# ---------------------------------------------------------------------------
# TC-G7: After RW→RO demotion, another host can acquire the RW lease
# ---------------------------------------------------------------------------
echo ""
echo "TC-G7: another host can mount RW immediately after pfs_remount_ro demote"

HOLDER_PID=""
if ! start_demote_holder 1 2; then
    fail "TC-G7: could not start demote holder"
else
    if ! trigger_demote; then
        stop_holder
        fail "TC-G7: demote failed ($DEMOTE_RESULT)"
    else
        DEMOTE_PID="$HOLDER_PID"

        # Host 3 attempts RW mount — should succeed now that host 1 released
        if start_rw_holder 3; then
            stop_holder           # stop host 3 (RW)
            stop_holder "$DEMOTE_PID"
            pass "TC-G7: host 3 acquired RW lease immediately after host 1 demoted"
        else
            stop_holder "$DEMOTE_PID"
            fail "TC-G7: host 3 could not mount RW after host 1 demoted"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# TC-G8: Mount usable after failed promote (EBUSY recovery)
#
# Regression test for the pfs_notify_inited bug: pfs_remount_rw EBUSY path
# used to leave mnt_status=0, making pfs_meta_lock a silent no-op.  The
# first stat/open that hit a namecache miss would crash on
# PFS_ASSERT(pfs_meta_islocked(mnt)) in pfs_inode_dir_find.
#
# After the fix, the promote helper does pfs_stat("/pbdname/") immediately
# after REMOUNT_FAILED and prints MOUNT_USABLE on success.  Without the fix
# the helper process crashes (detected as process death + missing output).
# ---------------------------------------------------------------------------
echo ""
echo "TC-G8: mount usable after failed promote (EBUSY recovery)"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-G8: setup: could not start RW holder as host 1"
else
    H1_PID="$HOLDER_PID"
    sleep 2   # ensure H1 is actively renewing

    if ! start_promote_holder 2; then
        stop_holder "$H1_PID"
        fail "TC-G8: could not start promote holder for host 2"
    else
        H2_PID="$HOLDER_PID"
        H2_OUT="$PROMOTE_TMPOUT"

        # Send promote signal manually — we need to keep the output file
        # alive (trigger_promote deletes it) so we can check MOUNT_USABLE.
        kill -USR1 "$H2_PID" 2>/dev/null

        # Wait for REMOUNT_FAILED + MOUNT_USABLE + REFCOUNT_OK (or
        # process death).  Budget: wait_and_check (≤LEASE_TEST_DURATION)
        # + restore (≤3s) + stat + refcount check (instant) + jitter (5s).
        deadline=$(( SECONDS + LEASE_TEST_DURATION + 10 ))
        got_result=false
        while [[ $SECONDS -lt $deadline ]]; do
            if grep -q "^REFCOUNT_OK$\|^REFCOUNT_BAD:" "$H2_OUT" 2>/dev/null; then
                got_result=true
                break
            fi
            if grep -q "^MOUNT_USABLE$" "$H2_OUT" 2>/dev/null; then
                # MOUNT_USABLE appeared but REFCOUNT line not yet — brief
                # extra wait for the next printf to land.
                sleep 0.2
                got_result=true
                break
            fi
            if grep -q "^MOUNT_UNUSABLE:" "$H2_OUT" 2>/dev/null; then
                got_result=true
                break
            fi
            if grep -q "^MOUNTED_RW$" "$H2_OUT" 2>/dev/null; then
                # Promote unexpectedly succeeded
                got_result=true
                break
            fi
            if ! kill -0 "$H2_PID" 2>/dev/null; then
                break
            fi
            sleep 0.2
        done

        if grep -q "^MOUNT_USABLE$" "$H2_OUT" 2>/dev/null; then
            if grep -q "^REFCOUNT_OK$" "$H2_OUT" 2>/dev/null; then
                rm -f "$H2_OUT"
                stop_holder "$H2_PID"
                stop_holder "$H1_PID"
                pass "TC-G8: mount usable and refcounts correct after failed promote"
            elif grep -q "^REFCOUNT_BAD:" "$H2_OUT" 2>/dev/null; then
                rc_info=$(grep "^REFCOUNT_BAD:" "$H2_OUT" | head -1)
                rm -f "$H2_OUT"
                stop_holder "$H2_PID"
                stop_holder "$H1_PID"
                fail "TC-G8: mount usable but refcounts wrong ($rc_info)"
            else
                rm -f "$H2_OUT"
                stop_holder "$H2_PID"
                stop_holder "$H1_PID"
                pass "TC-G8: mount still usable after failed promote"
            fi
        elif ! kill -0 "$H2_PID" 2>/dev/null; then
            rm -f "$H2_OUT"
            stop_holder "$H1_PID"
            fail "TC-G8: promote holder crashed after EBUSY (pfs_notify_inited missing?)"
        elif grep -q "^MOUNTED_RW$" "$H2_OUT" 2>/dev/null; then
            rm -f "$H2_OUT"
            stop_holder "$H2_PID"
            stop_holder "$H1_PID"
            fail "TC-G8: promote should have failed with EBUSY"
        elif grep -q "^MOUNT_UNUSABLE:" "$H2_OUT" 2>/dev/null; then
            result=$(grep "^MOUNT_UNUSABLE:" "$H2_OUT" | head -1)
            rm -f "$H2_OUT"
            stop_holder "$H2_PID"
            stop_holder "$H1_PID"
            fail "TC-G8: stat failed after EBUSY recovery ($result)"
        else
            rm -f "$H2_OUT"
            stop_holder "$H2_PID"
            stop_holder "$H1_PID"
            fail "TC-G8: timed out waiting for MOUNT_USABLE output"
        fi
    fi
fi

lease_test_summary
