#!/bin/bash
# tc-c-ro-isolation.sh — Group C: RO mount isolation tests.
#
# TC-C1: RO mount does not write a host record (sector remains zeroed).
# TC-C2: Multiple concurrent RO mounts from different hosts all succeed.
# TC-C3: RO mount after RW mount shows correct data (basic sanity).

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group C: RO isolation ==="

# ---------------------------------------------------------------------------
# TC-C1: RO mount leaves host sector zeroed (no lease record written)
# ---------------------------------------------------------------------------
echo ""
echo "TC-C1: RO mount does not write host record"

# Need an RW mount first to make the FS writable before RO reads
rv=0
try_rw_mount 1 || rv=$?
if [[ $rv -ne 0 ]]; then
    fail "TC-C1: setup: initial RW touch failed"
else
    # Now do RO mount as host 2
    rv=0
    try_ro_mount 2 || rv=$?

    if [[ $rv -ne 0 ]]; then
        fail "TC-C1: RO mount as host 2 failed (exit $rv)"
    else
        # Verify sector 2 is zeroed.  We do this with a temporary RW mount
        # (host 1) to read the paxos inner file.
        # Use pfs read to check sector 2 content.
        sector_offset=$(paxos_sector_offset 2)
        tmp=$(mktemp)
        pfs_cmd 1 read -o "$sector_offset" -l "$LEASE_TEST_SECTOR_SIZE" \
            "/${TEST_LOOP_DEVICE_NAME}/.pfs-paxos" >"$tmp" 2>/dev/null || {
            rm -f "$tmp"
            fail "TC-C1: could not read paxos sector 2"
            lease_test_summary
            exit 1
        }
        # Check all bytes are zero
        nonzero=$(od -v -t x1 -A n "$tmp" | tr -s ' \n' '\n' | grep -v '^00$' | grep -v '^$' | wc -l) || true
        rm -f "$tmp"

        if [[ "$nonzero" -eq 0 ]]; then
            pass "TC-C1: sector 2 is all zeros after RO mount by host 2"
        else
            fail "TC-C1: sector 2 has non-zero bytes after RO mount (host 2 wrote a record)"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# TC-C2: Multiple concurrent RO mounts succeed
# ---------------------------------------------------------------------------
echo ""
echo "TC-C2: concurrent RO mounts from hosts 2, 3, 4"

# Start three RO holders
HOLDER2_PID=""
HOLDER3_PID=""
HOLDER4_PID=""
ok=true

start_rw_holder_as() {
    local hostid="$1"
    local mode="$2"
    local tmpout
    tmpout=$(mktemp)
    lease_hold "$hostid" "$mode" >"$tmpout" &
    local pid=$!
    local deadline=$((SECONDS + 10))
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -q "^MOUNTED$" "$tmpout" 2>/dev/null; then
            rm -f "$tmpout"
            echo $pid
            return 0
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            rm -f "$tmpout"
            return 1
        fi
        sleep 0.2
    done
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$tmpout"
    return 1
}

HOLDER2_PID=$(start_rw_holder_as 2 ro) || ok=false
HOLDER3_PID=$(start_rw_holder_as 3 ro) || ok=false
HOLDER4_PID=$(start_rw_holder_as 4 ro) || ok=false

for pid in ${HOLDER2_PID:-} ${HOLDER3_PID:-} ${HOLDER4_PID:-}; do
    [[ -n "$pid" ]] && kill -TERM "$pid" 2>/dev/null || true
done
for pid in ${HOLDER2_PID:-} ${HOLDER3_PID:-} ${HOLDER4_PID:-}; do
    [[ -n "$pid" ]] && wait "$pid" 2>/dev/null || true
done

if $ok; then
    pass "TC-C2: three concurrent RO mounts (hosts 2,3,4) all succeeded"
else
    fail "TC-C2: at least one concurrent RO mount failed"
fi

# ---------------------------------------------------------------------------
# TC-C3: Data written by RW mount is visible to subsequent RO mount
# ---------------------------------------------------------------------------
echo ""
echo "TC-C3: RO mount sees data written by prior RW mount"

# Create a file via RW as host 1
pfs_path="/${TEST_LOOP_DEVICE_NAME}/tc_c3_testfile"
echo "hello from TC-C3" | pfs_cmd 1 write "$pfs_path" 2>/dev/null || {
    fail "TC-C3: setup: write via RW mount failed"
    lease_test_summary
    exit 1
}

# Read it back via RO as host 2
content=$(pfs_cmd 2 read "$pfs_path" 2>/dev/null) || {
    fail "TC-C3: RO read failed"
    lease_test_summary
    exit 1
}

if [[ "$content" == "hello from TC-C3" ]]; then
    pass "TC-C3: RO mount reads data written by RW mount"
else
    fail "TC-C3: data mismatch: got '$content'"
fi

lease_test_summary
