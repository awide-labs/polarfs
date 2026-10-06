#!/bin/bash
# tc-i-corruption.sh — Group I: corrupt paxos sector handling.
#
# A corrupt sector can't be trusted to say who wrote it or when, so a new RW
# mount watches it: if its bytes change, someone is writing it (EBUSY); if
# they stay the same for the expiry window
# (LEASE_TEST_DURATION + LEASE_TEST_CLOCK_SKEW + 1), its writer is dead and
# the mount proceeds.  A sector that stays unreadable blocks the mount.
#
# TC-I1: Sector with bad magic (0xdeadbeef) that never changes — the RW
#        mount succeeds after waiting out the expiry window.
# TC-I2: Sector with valid PFS_HOST_MAGIC, a fresh timestamp and a wrong
#        checksum that never changes — same as TC-I1.
# TC-I3: A live holder's sector reads back corrupt for 3s — the RW mount
#        must be refused (the corrupt bytes keep changing as it renews).
# TC-I4: A live holder's sector fails to read (EIO) for 3s — the RW mount
#        must be refused.
# TC-I5: An empty sector stays unreadable (EIO) — the RW mount must be
#        refused rather than assume the sector is empty.
# TC-I6: A sector's first read is slow and returns corrupt bytes that change
#        just before paxos_lease_duration has passed since that read
#        returned.  The window must count from when the bytes were seen,
#        so the mount sees the change and is refused.
# TC-I7: Same, but the sector is unreadable for 2s first, so the bytes are
#        first seen by a poll read rather than the initial scan.
# TC-I8: A dead writer's corrupt sector whose first read takes longer than
#        a whole window.  Its window starts when the read returns, and a
#        slow scan must not cut it short: the mount succeeds once the
#        window has passed.

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group I: corrupt paxos sector handling ==="

# Expiry window for a corrupt sector, and a budget for a refused mount.
CORRUPT_WINDOW=$(( LEASE_TEST_DURATION + LEASE_TEST_CLOCK_SKEW + 1 ))
REFUSE_BUDGET=$(( CORRUPT_WINDOW + 25 ))

# ---------------------------------------------------------------------------
# write_corrupt_sector <hostid> <type: badmagic|badchecksum>
#
# Writes a corrupt host record into sector <hostid> of .pfs-paxos using
# pfs_lease_hold corrupt-sector.  This mode mounts RW as host 50 (using the
# same pfs_write_paxos_sector path as pfs_rw_lease_acquire) and writes the
# corrupt bytes directly to the paxos sector, bypassing the VFS/journal.
#
# The written record has PFS_HOST_FL_RW set and a fresh timestamp, so it
# *looks* like a live RW holder but for its corrupt magic or checksum.  A
# corrupt record says nothing reliable about its writer, so an RW mount does
# not skip it: it watches the sector, and goes ahead only once the bytes have
# stayed the same for the expiry window (TC-I1, TC-I2).
# ---------------------------------------------------------------------------
write_corrupt_sector() {
    local target_hostid="$1"
    local type="$2"   # badmagic | badchecksum
    local out

    out=$(lease_hold 50 corrupt-sector "$target_hostid" "$type") || return 1
    [[ "$out" == "CORRUPT_WRITTEN" ]]
}

# ---------------------------------------------------------------------------
# TC-I1: Unchanging bad-magic sector; RW mount succeeds after the window
# ---------------------------------------------------------------------------
echo ""
echo "TC-I1: unchanging bad-magic sector in slot 1 is waited out by host 2"

if ! write_corrupt_sector 1 badmagic; then
    fail "TC-I1: setup: could not write bad-magic sector to slot 1"
else
    before=$SECONDS
    HOLDER_PID=""
    if start_rw_holder 2; then
        elapsed=$(( SECONDS - before ))
        stop_holder
        # elapsed is whole seconds; allow 1s of rounding
        if [[ $elapsed -ge $(( CORRUPT_WINDOW - 1 )) ]]; then
            pass "TC-I1: RW mount by host 2 succeeded after ${elapsed}s, having waited out the bad-magic sector"
        else
            fail "TC-I1: RW mount succeeded in ${elapsed}s — the corrupt sector was not watched for ${CORRUPT_WINDOW}s"
        fi
    else
        fail "TC-I1: RW mount by host 2 failed — the unchanging bad-magic sector was not treated as dead"
    fi
fi

# ---------------------------------------------------------------------------
# TC-I2: Unchanging bad-checksum sector with fresh timestamp;
#        RW mount succeeds after the window
# ---------------------------------------------------------------------------
echo ""
echo "TC-I2: unchanging bad-checksum sector (fresh ts) in slot 1 is waited out"

# The corrupt sector looks exactly like a live holder except for the checksum.
if ! write_corrupt_sector 1 badchecksum; then
    fail "TC-I2: setup: could not write bad-checksum sector to slot 1"
else
    before=$SECONDS
    HOLDER_PID=""
    if start_rw_holder 2; then
        elapsed=$(( SECONDS - before ))
        stop_holder
        if [[ $elapsed -ge $(( CORRUPT_WINDOW - 1 )) ]]; then
            pass "TC-I2: RW mount by host 2 succeeded after ${elapsed}s, having waited out the bad-checksum sector"
        else
            fail "TC-I2: RW mount succeeded in ${elapsed}s — the corrupt sector was not watched for ${CORRUPT_WINDOW}s"
        fi
    else
        fail "TC-I2: RW mount by host 2 failed — the unchanging bad-checksum sector was not treated as dead"
    fi
fi

# ---------------------------------------------------------------------------
# TC-I3: Live holder whose sector reads back corrupt
# ---------------------------------------------------------------------------
echo ""
echo "TC-I3: live holder's sector reads back corrupt — RW mount refused"

if ! start_rw_holder 1; then
    fail "TC-I3: setup: could not start RW holder as host 1"
else
    sleep 2   # let H1 renew
    expect_mount_refused TC-I3 2 "H1's sector read back corrupt" "$EBUSY" \
        'rw_lease_bad: host 1 sector corrupt' \
        'rw_lease_bad: host 1 corrupt sector changed' \
        -- rw-bad-read 1 corrupt 3000
    kill -0 "$HOLDER_PID" 2>/dev/null || fail "TC-I3: H1 lost its RW mount"
    stop_holder
fi

# ---------------------------------------------------------------------------
# TC-I4: Live holder whose sector is unreadable
# ---------------------------------------------------------------------------
echo ""
echo "TC-I4: live holder's sector fails to read — RW mount refused"

if ! start_rw_holder 1; then
    fail "TC-I4: setup: could not start RW holder as host 1"
else
    sleep 2   # let H1 renew
    expect_mount_refused TC-I4 2 "H1's sector was unreadable" "$EBUSY" \
        'rw_lease_bad: host 1 sector unreadable' \
        'rw_lease_check: BUSY - host 1 holds live RW lease' \
        -- rw-bad-read 1 eio 3000
    kill -0 "$HOLDER_PID" 2>/dev/null || fail "TC-I4: H1 lost its RW mount"
    stop_holder
fi

# ---------------------------------------------------------------------------
# TC-I5: Empty sector that stays unreadable
# ---------------------------------------------------------------------------
echo ""
echo "TC-I5: empty sector stays unreadable — RW mount refused"

expect_mount_refused TC-I5 2 "H7's sector stayed unreadable" "$EBUSY" \
    'rw_lease_bad: host 7 sector unreadable for [0-9]+s, refusing RW lease' \
    -- rw-bad-read 7 eio 600000

# ---------------------------------------------------------------------------
# TC-I6 / TC-I7: the window counts from when the corrupt bytes were seen
# ---------------------------------------------------------------------------
# The first read of H7's sector that returns data takes SLOW_MS longer.  If
# the window started when the read was issued (or earlier), it would end
# SLOW_MS early, before the bytes change at paxos_lease_duration - 0.5s
# after they were seen; SLOW_MS exceeds the window's slack over
# paxos_lease_duration (LEASE_TEST_CLOCK_SKEW + 1) by enough to cover poll
# granularity.
SLOW_MS=$(( (LEASE_TEST_CLOCK_SKEW + 5) * 1000 ))

echo ""
echo "TC-I6: corrupt sector seen late by the initial scan — RW mount refused"

expect_mount_refused TC-I6 2 "H7's late-seen corrupt sector changed" "$EBUSY" \
    'rw_lease_bad: host 7 corrupt sector changed' \
    -- rw-slow-corrupt 7 "$SLOW_MS" 0

echo ""
echo "TC-I7: corrupt sector seen late after being unreadable — RW mount refused"

expect_mount_refused TC-I7 2 "H7's late-seen corrupt sector changed" "$EBUSY" \
    'rw_lease_bad: host 7 sector unreadable' \
    'rw_lease_bad: host 7 corrupt sector changed' \
    -- rw-slow-corrupt 7 "$SLOW_MS" 2000

# ---------------------------------------------------------------------------
# TC-I8: a slow scan does not cut a stable corrupt sector's window short
# ---------------------------------------------------------------------------
echo ""
echo "TC-I8: dead writer's corrupt sector behind a slow first read — mount succeeds"

i8_delay=$(( CORRUPT_WINDOW + 2 ))
before=$SECONDS
if HOLDER_EXTRA_BUDGET=$i8_delay \
    start_rw_holder 2 rw-slow-corrupt 7 $(( i8_delay * 1000 )) 0 nochange; then
    elapsed=$(( SECONDS - before ))
    stop_holder
    if [[ $elapsed -ge $(( i8_delay + CORRUPT_WINDOW - 1 )) ]]; then
        pass "TC-I8: RW mount succeeded after ${elapsed}s, having watched the slow-read sector for a full window"
    else
        fail "TC-I8: RW mount succeeded in ${elapsed}s — less than the ${i8_delay}s read plus the ${CORRUPT_WINDOW}s window"
    fi
else
    fail "TC-I8: RW mount refused — the slow scan cut the corrupt sector's window short"
fi

lease_test_summary
