#!/bin/bash
# pfsadm Python 3 regression tests. Called from test/run.sh after pfsd
# startup; expects the read-test file seeded before the daemon mounted.
#
# pfsd mounts lazily, and the admin socket only exists while a pbd is
# mounted, so we hold a RO mount for the daemon-backed checks.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINDIR="${BINDIR:-$(cd "${SCRIPT_DIR}/../bin" && pwd)}"
PFSADM="${PFSADM:-$BINDIR/pfsadm}"
CLUSTER="${PFSADM_CLUSTER:-disk}"
PBD="${TEST_LOOP_DEVICE_NAME:-loop100}"
READ_REF="${PFSADM_READ_REF:-/tmp/pfsadm_read_ref.bin}"
READ_PATH="/$PBD/pfsadm_read_test.bin"
READ_OUT="/tmp/pfsadm_read_out.bin"
# poll_interval defaults to 1, so config -s of it is idempotent.
SET_OPT="${PFSADM_SET_OPT:-poll_interval}"
SET_VAL="${PFSADM_SET_VAL:-1}"
HOLD_BIN="${PFSADM_HOLD_BIN:-$BINDIR/pfsd_ebusy_test}"
HOLD_HID="${PFSADM_HOLD_HID:-8}"
PFS_LOG="/var/log/pfs-${PBD}.log"
HOLD_PID=""

FAILURES=0

pass() {
    echo "  PASS: $1"
}

fail() {
    echo "  FAIL: $1"
    FAILURES=$((FAILURES + 1))
}

cleanup() {
    if [[ -n "$HOLD_PID" ]]; then
        kill -TERM "$HOLD_PID" 2>/dev/null || true
        wait "$HOLD_PID" 2>/dev/null || true
    fi
    rm -f "$READ_OUT" 2>/dev/null || true
}
trap cleanup EXIT

# Hold a RO mount in the background; returns 0 once it reports MOUNTED.
start_hold() {
    local tmpout deadline
    tmpout=$(mktemp)
    "$HOLD_BIN" "$CLUSTER" "$PBD" hold-ro "$HOLD_HID" >"$tmpout" 2>>"$PFS_LOG" &
    HOLD_PID=$!
    deadline=$((SECONDS + 30))
    while [[ $SECONDS -lt $deadline ]]; do
        grep -q "^MOUNTED$" "$tmpout" 2>/dev/null && break
        kill -0 "$HOLD_PID" 2>/dev/null || break
        sleep 0.2
    done
    if grep -q "^MOUNTED$" "$tmpout" 2>/dev/null; then
        rm -f "$tmpout"
        return 0
    fi
    rm -f "$tmpout"
    return 1
}

echo ""
echo "=== pfsadm_test (Python 3 regressions) ==="
echo "  pfsadm: $PFSADM"
echo "  pbd:    $PBD"

# No subcommand must print usage and exit 2 (add_subparsers required=True).
# Needs no mount, so run it first. Capture via if so set -e tolerates the
# non-zero exit.
echo "Checking: no subcommand prints usage..."
if noargs_err=$("$PFSADM" 2>&1 >/dev/null); then
    noargs_rc=0
else
    noargs_rc=$?
fi
if [[ $noargs_rc -eq 2 ]] && echo "$noargs_err" | grep -qi "required"; then
    pass "no subcommand exits 2 with usage error"
else
    fail "expected exit 2 with usage (got rc=$noargs_rc): $noargs_err"
fi

# Fail fast: nothing below works without an admin socket.
echo "Holding a RO mount (hid=$HOLD_HID) for daemon-backed checks..."
if ! start_hold; then
    fail "could not hold a mount ($HOLD_BIN)"
    echo ""
    echo "=== pfsadm_test: $FAILURES check(s) FAILED ==="
    exit 1
fi
pass "RO mount held (admin socket available)"

# config -l/-s/-r and trace -l exercise the str->bytes packing fix; a
# regression raised struct.error here on Python 3.
echo "Checking: config -l..."
if "$PFSADM" config -l "$PBD"; then
    pass "config -l succeeded"
else
    fail "config -l failed (rc=$?)"
fi

echo "Checking: config -s $SET_OPT $SET_VAL..."
if set_out=$("$PFSADM" config -s "$PBD" "$SET_OPT" "$SET_VAL" 2>&1); then
    set_rc=0
else
    set_rc=$?
fi
echo "$set_out"
if [[ $set_rc -eq 0 ]] && echo "$set_out" | grep -qi "succeeded"; then
    pass "config -s succeeded"
else
    fail "config -s failed (rc=$set_rc): $set_out"
fi

echo "Checking: config -r..."
if "$PFSADM" config -r "$PBD"; then
    pass "config -r succeeded"
else
    fail "config -r failed (rc=$?)"
fi

echo "Checking: trace -l..."
if "$PFSADM" trace -l "$PBD" '*'; then
    pass "trace -l succeeded"
else
    fail "trace -l failed (rc=$?)"
fi

# read must round-trip binary content (writes to stdout.buffer, no ascii
# decode).
echo "Checking: read binary round-trip..."
if [[ ! -f "$READ_REF" ]]; then
    fail "reference file $READ_REF missing (run.sh seed step did not run)"
else
    if "$PFSADM" read "$READ_PATH" >"$READ_OUT"; then
        read_rc=0
    else
        read_rc=$?
    fi
    if [[ $read_rc -ne 0 ]]; then
        fail "read $READ_PATH failed (rc=$read_rc)"
    elif cmp -s "$READ_REF" "$READ_OUT"; then
        pass "read content matches reference byte-for-byte"
    else
        fail "read content differs from reference ($READ_REF vs $READ_OUT)"
    fi
fi

echo ""
if [[ $FAILURES -ne 0 ]]; then
    echo "=== pfsadm_test: $FAILURES check(s) FAILED ==="
    exit 1
fi
echo "=== pfsadm_test: all checks PASSED ==="
