#!/bin/bash
# tc-i-corruption.sh — Group I: corrupt paxos sector handling.
#
# TC-I1: Sector with bad magic (0xdeadbeef) is ignored by
#        pfs_rw_lease_check — a new RW mount succeeds immediately even
#        though the sector has a non-zero, non-empty magic value.
# TC-I2: Sector with valid PFS_HOST_MAGIC but wrong checksum and a
#        *fresh* timestamp is ignored by pfs_rw_lease_check.  Without
#        the checksum error the fresh timestamp would block the mount for
#        the full lease duration; with it the mount completes in < 3s.

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group I: corrupt paxos sector handling ==="

# ---------------------------------------------------------------------------
# write_corrupt_sector <hostid> <type: badmagic|badchecksum>
#
# Writes a corrupt host record into sector <hostid> of .pfs-paxos using
# pfs_lease_hold corrupt-sector.  This mode mounts RW as host 50 (using the
# same pfs_write_paxos_sector path as pfs_rw_lease_acquire) and writes the
# corrupt bytes directly to the paxos sector, bypassing the VFS/journal.
#
# The written record has PFS_HOST_FL_RW set and a fresh timestamp, so it
# *looks* like a live RW holder — the only reason pfs_rw_lease_check should
# skip it is the corrupt magic or checksum.
# ---------------------------------------------------------------------------
write_corrupt_sector() {
    local target_hostid="$1"
    local type="$2"   # badmagic | badchecksum
    local out

    out=$(lease_hold 50 corrupt-sector "$target_hostid" "$type") || return 1
    [[ "$out" == "CORRUPT_WRITTEN" ]]
}

# ---------------------------------------------------------------------------
# TC-I1: Bad-magic sector is skipped; RW mount succeeds immediately
# ---------------------------------------------------------------------------
echo ""
echo "TC-I1: bad-magic sector in slot 1 does not block RW mount by host 2"

if ! write_corrupt_sector 1 badmagic; then
    fail "TC-I1: setup: could not write bad-magic sector to slot 1"
else
    before=$SECONDS
    HOLDER_PID=""
    if start_rw_holder 2; then
        elapsed=$(( SECONDS - before ))
        stop_holder
        # ballot acquire is fast; overall should complete well under
        # LEASE_TEST_DURATION (would be blocked that long without skip)
        if [[ $elapsed -lt $(( LEASE_TEST_DURATION - 1 )) ]]; then
            pass "TC-I1: RW mount by host 2 succeeded in ${elapsed}s despite bad-magic sector in slot 1"
        else
            fail "TC-I1: RW mount succeeded but took ${elapsed}s — may not have skipped the corrupt sector"
        fi
    else
        fail "TC-I1: RW mount by host 2 failed — bad-magic sector may not have been skipped"
    fi
fi

# ---------------------------------------------------------------------------
# TC-I2: Bad-checksum sector with fresh timestamp is skipped;
#        mount completes quickly (not blocked for LEASE_TEST_DURATION)
# ---------------------------------------------------------------------------
echo ""
echo "TC-I2: bad-checksum sector (fresh ts) in slot 1 does not block RW mount"

# The corrupt sector looks exactly like a live holder except for the checksum.
# A valid version of this sector would block host 2 for >= LEASE_TEST_DURATION.
if ! write_corrupt_sector 1 badchecksum; then
    fail "TC-I2: setup: could not write bad-checksum sector to slot 1"
else
    before=$SECONDS
    HOLDER_PID=""
    if start_rw_holder 2; then
        elapsed=$(( SECONDS - before ))
        stop_holder
        if [[ $elapsed -lt $(( LEASE_TEST_DURATION - 1 )) ]]; then
            pass "TC-I2: RW mount by host 2 succeeded in ${elapsed}s — bad-checksum sector skipped (not blocked for ${LEASE_TEST_DURATION}s)"
        else
            fail "TC-I2: RW mount took ${elapsed}s — corrupt sector may have triggered a wait instead of being skipped"
        fi
    else
        fail "TC-I2: RW mount by host 2 failed — bad-checksum sector may not have been skipped"
    fi
fi

lease_test_summary
