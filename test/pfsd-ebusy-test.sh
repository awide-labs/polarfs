#!/bin/bash
# pfsd-ebusy-test.sh — pfsdaemon must not crash after a
# failed RW promote (EBUSY recovery path).
#
# Expects pfsdaemon already running on TEST_LOOP_DEVICE_NAME.
# Called from test/run.sh after pfsdaemon startup.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINDIR="${BINDIR:-$(cd "${SCRIPT_DIR}/../bin" && pwd)}"
PFS_LOG="/var/log/pfs-${TEST_LOOP_DEVICE_NAME}.log"
EBUSY_TEST="$BINDIR/pfsd_ebusy_test"
LEASE_HOLD="$BINDIR/pfs_lease_hold"
CLUSTER=disk
PBD="$TEST_LOOP_DEVICE_NAME"

echo ""
echo "=== pfsd_ebusy_test ==="

# Step 1: start external RW lease holder (hid=1, not through pfsdaemon).
tmpout=$(mktemp)
"$LEASE_HOLD" "$CLUSTER" "$PBD" 1 rw >"$tmpout" 2>>"$PFS_LOG" &
LEASE_PID=$!
deadline=$((SECONDS + 30))
while [[ $SECONDS -lt $deadline ]]; do
    grep -q "^MOUNTED$" "$tmpout" 2>/dev/null && break
    kill -0 "$LEASE_PID" 2>/dev/null || break
    sleep 0.2
done
rm -f "$tmpout"
if ! kill -0 "$LEASE_PID" 2>/dev/null; then
    echo "FAIL: could not start RW lease holder"
    exit 1
fi
echo "  RW lease holder started (hid=1, pid=$LEASE_PID)"

# Step 2: start RO client through pfsdaemon (hid=2, stays alive).
tmpout=$(mktemp)
"$EBUSY_TEST" "$CLUSTER" "$PBD" hold-ro 2 >"$tmpout" 2>>"$PFS_LOG" &
RO_PID=$!
deadline=$((SECONDS + 15))
while [[ $SECONDS -lt $deadline ]]; do
    grep -q "^MOUNTED$" "$tmpout" 2>/dev/null && break
    kill -0 "$RO_PID" 2>/dev/null || break
    sleep 0.2
done
rm -f "$tmpout"
if ! kill -0 "$RO_PID" 2>/dev/null; then
    echo "FAIL: could not start RO client"
    kill -TERM "$LEASE_PID" 2>/dev/null || true
    wait "$LEASE_PID" 2>/dev/null || true
    exit 1
fi
echo "  RO client started (hid=2, pid=$RO_PID)"

# Step 3: attempt RW promote (hid=3) — should get EBUSY.
echo "  Attempting RW promote (hid=3)..."
tmpout=$(mktemp)
"$EBUSY_TEST" "$CLUSTER" "$PBD" promote-rw 3 >"$tmpout" 2>>"$PFS_LOG" || true
result=$(cat "$tmpout")
rm -f "$tmpout"
echo "  Promote result: $result"

if echo "$result" | grep -q "^PROMOTE_FAILED:"; then
    echo "  PASS: promote correctly rejected"
elif echo "$result" | grep -q "^PROMOTE_OK$"; then
    echo "  FAIL: promote unexpectedly succeeded"
    kill -TERM "$RO_PID" 2>/dev/null || true
    kill -TERM "$LEASE_PID" 2>/dev/null || true
    wait "$RO_PID" 2>/dev/null || true
    wait "$LEASE_PID" 2>/dev/null || true
    exit 1
fi

# Step 3b: repeat failed promotes with abrupt disconnect (no unmount).
# Exposes refcount leaks: if the server clears host_id_ on failed
# remount, readEOF skips pfs_mount_release and each cycle leaks one
# reference.  After enough leaks pfsdaemon can no longer serve clients.
for attempt in 1 2 3; do
    echo "  promote-crash attempt $attempt/3..."
    tmpout=$(mktemp)
    "$EBUSY_TEST" "$CLUSTER" "$PBD" promote-crash 3 >"$tmpout" 2>>"$PFS_LOG" || true
    result=$(cat "$tmpout")
    rm -f "$tmpout"
    echo "  Result: $result"
done

# Step 4: stat via pfsd SDK (hid=4) — pfsdaemon must still be alive.
# Run with timeout: if pfsdaemon crashed the SDK blocks forever waiting
# for a reply on the shared-memory channel.
echo "  Running stat (hid=4)..."
tmpout=$(mktemp)
"$EBUSY_TEST" "$CLUSTER" "$PBD" stat 4 \
    "/$PBD/nonexistent_ebusy_test" >"$tmpout" 2>>"$PFS_LOG" &
STAT_PID=$!
deadline=$((SECONDS + 10))
while [[ $SECONDS -lt $deadline ]]; do
    kill -0 "$STAT_PID" 2>/dev/null || break
    sleep 0.2
done
if kill -0 "$STAT_PID" 2>/dev/null; then
    echo "  stat timed out (pfsdaemon not responding)"
    kill -9 "$STAT_PID" 2>/dev/null || true
    wait "$STAT_PID" 2>/dev/null || true
    result="STAT_TIMEOUT"
else
    wait "$STAT_PID" 2>/dev/null || true
    result=$(cat "$tmpout")
fi
rm -f "$tmpout"
echo "  Stat result: $result"

if echo "$result" | grep -q "^STAT_ENOENT$\|^STAT_OK$"; then
    echo "  PASS: pfsdaemon survived EBUSY promote"
else
    echo "  FAIL: pfsdaemon crashed or hung ($result)"
    kill -TERM "$RO_PID" 2>/dev/null || true
    kill -TERM "$LEASE_PID" 2>/dev/null || true
    wait "$RO_PID" 2>/dev/null || true
    wait "$LEASE_PID" 2>/dev/null || true
    exit 1
fi

# Step 5: stop external RW lease holder so promote can succeed.
kill -TERM "$RO_PID" 2>/dev/null || true
kill -TERM "$LEASE_PID" 2>/dev/null || true
wait "$RO_PID" 2>/dev/null || true
wait "$LEASE_PID" 2>/dev/null || true

# Step 6: verify no refcount leak — promote with the same hid=3 that
# was used in the promote-crash cycles.  Without the lease holder the
# promote should succeed.  If the server leaked refcounts for hid=3
# (by clearing host_id_ on failed remount), the accumulated refs
# cause pfs_host_promot_ref to fail (refcount != 1) and the promote
# is rejected even though nothing holds the RW lease.
echo "  Verifying no refcount leak (promote hid=3 with no blocker)..."
tmpout=$(mktemp)
"$EBUSY_TEST" "$CLUSTER" "$PBD" promote-rw 3 >"$tmpout" 2>>"$PFS_LOG" || true
result=$(cat "$tmpout")
rm -f "$tmpout"
echo "  Promote result: $result"

if echo "$result" | grep -q "^PROMOTE_OK$"; then
    echo "  PASS: no refcount leak"
else
    echo "  FAIL: promote rejected — refcounts leaked ($result)"
    exit 1
fi
