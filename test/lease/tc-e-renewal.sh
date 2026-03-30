#!/bin/bash
# tc-e-renewal.sh — Group E: lease renewal tests.
#
# TC-E1: Timestamp in the host record advances over time while mount is held
#        (confirms the log thread is renewing periodically).
# TC-E2: Release (clean unmount) zeroes the sector regardless of how many
#        renewals occurred.

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group E: Lease renewal ==="

# Helper: read hr_timestamp (uint64 at offset 16 within the host record)
# pfs_host_record_t layout:
#   [0]  hr_magic      uint32
#   [4]  hr_flags      uint32
#   [8]  hr_host_id    uint32
#   [12] hr_generation uint32
#   [16] hr_timestamp  uint64
read_timestamp() {
    local hostid="$1"
    local sector_offset=$(paxos_sector_offset "$hostid")
    local ts_offset=$(( sector_offset + 16 ))
    local tmp
    tmp=$(mktemp)
    pfs_cmd 50 read -o "$ts_offset" -l 8 \
        "/${TEST_LOOP_DEVICE_NAME}/.pfs-paxos" >"$tmp" 2>/dev/null || {
        rm -f "$tmp"
        echo ""
        return 1
    }
    # od -t u8 gives unsigned 8-byte integers
    local val
    val=$(od -t u8 -A n "$tmp" | tr -s ' ' '\n' | grep -v '^$' | head -1)
    rm -f "$tmp"
    echo "$val"
}

# ---------------------------------------------------------------------------
# TC-E1: Timestamp advances over time (renewal is working)
# ---------------------------------------------------------------------------
echo ""
echo "TC-E1: timestamp advances while mount is held"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-E1: setup: could not mount as host 1"
    lease_test_summary
    exit 1
fi

# Sample timestamp at t=0
ts0=$(read_timestamp 1) || ts0=""

# Wait 3 seconds (should get 2-3 renewals at 1s intervals)
sleep 3

# Sample timestamp at t=3
ts1=$(read_timestamp 1) || ts1=""

stop_holder

if [[ -z "$ts0" || -z "$ts1" ]]; then
    fail "TC-E1: could not read timestamp from paxos sector"
elif [[ "$ts1" -gt "$ts0" ]]; then
    pass "TC-E1: timestamp advanced from $ts0 to $ts1 (renewal working)"
else
    fail "TC-E1: timestamp did not advance (ts0=$ts0, ts1=$ts1)"
fi

# ---------------------------------------------------------------------------
# TC-E2: Clean unmount zeroes the sector after multiple renewals
# ---------------------------------------------------------------------------
echo ""
echo "TC-E2: sector zeroed after clean unmount (even after renewals)"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-E2: setup: could not mount as host 1"
    lease_test_summary
    exit 1
fi

# Let several renewals happen
sleep 3

# Clean unmount
stop_holder

# Sector 1 must now be all zeros.
# Use a RO mount to read (doesn't need RW permission since paxos file is inner).
# Actually we need some mount to access the inner file.  Use host 99 RO.
sector_offset=$(paxos_sector_offset 1)
tmp=$(mktemp)
pfs_cmd 50 read -o "$sector_offset" -l "$LEASE_TEST_SECTOR_SIZE" \
    "/${TEST_LOOP_DEVICE_NAME}/.pfs-paxos" >"$tmp" 2>/dev/null || {
    rm -f "$tmp"
    fail "TC-E2: could not read paxos sector 1 after umount"
    lease_test_summary
    exit 1
}

nonzero=$(od -v -t x1 -A n "$tmp" | tr -s ' \n' '\n' | grep -v '^00$' | grep -v '^$' | wc -l) || true
rm -f "$tmp"

if [[ "$nonzero" -eq 0 ]]; then
    pass "TC-E2: sector 1 is all zeros after clean unmount"
else
    fail "TC-E2: sector 1 has $nonzero non-zero bytes after clean unmount"
fi

lease_test_summary
