#!/bin/bash
# fscp-test.sh — integration tests for `pfs fscp -c` output behaviour.
#
# XCOM-222 changed how `pfs fscp -c` reports the result of comparing the
# source and destination blocks. These cases pin the two output-level
# regressions that are observable on a successful copy:
#
#   * (XCOM-222 #3) the per-block "crc check pass!" line is gone from
#     stdout, so it no longer clobbers the chunk_copy progress line
#     ("N blocks have been copied, M chunks are done");
#   * (XCOM-222 #2) the per-chunk pass message is a single, clean,
#     newline-terminated log line ("chunk N verified"), not the
#     old backslash-continued string whose source indentation leaked into
#     the output.
#
# The exit-path change (XCOM-222 #1) and the "block mismatch" message are
# only reachable when a copied block differs between src and dst, which
# needs a fault-injecting device to reproduce reliably, so they are not
# covered here.
#
# Called from test/run.sh before pfsd starts: `fscp` is an offline tool
# that mounts the source itself, so the source loop device must not yet be
# owned by the daemon. The source is the image run.sh already formatted and
# seeded; this script creates its own target image.
#
# Usage: bash test/fscp-test.sh <src_pbdname>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINDIR="${BINDIR:-$(cd "${SCRIPT_DIR}/../bin" && pwd)}"
PFS="$BINDIR/pfs"

SRC="${1:-${TEST_LOOP_DEVICE_NAME:-}}"
if [[ -z "$SRC" ]]; then
    echo "ERROR: source pbd name required (arg1 or TEST_LOOP_DEVICE_NAME)" >&2
    exit 1
fi

if [[ ! -x "$PFS" ]]; then
    echo "ERROR: $PFS not found. Build the project first." >&2
    exit 1
fi

# The target must be at least as large as the source (fscp requires
# src <= dst), so size it to match run.sh's source image.
DST_SIZE_GB="${TEST_IMAGE_SIZE_GB:-30}"
IMG_DST="${FSCP_DST_IMAGE_FILE:-$(dirname "$TEST_IMAGE_FILE")/polarfs_fscp_dst.img}"
LOOP_DST=""
STDOUT_FILE=""
STDERR_FILE=""
FAILURES=0

cleanup() {
    [[ -n "$LOOP_DST" ]] && losetup -d "$LOOP_DST" 2>/dev/null || true
    rm -f "$IMG_DST" "$STDOUT_FILE" "$STDERR_FILE" 2>/dev/null || true
}
trap cleanup EXIT

pass() {
    echo "  PASS: $1"
}

fail() {
    echo "  FAIL: $1"
    FAILURES=$((FAILURES + 1))
}

echo "=== fscp -c output regression tests ==="

# --- setup: reuse run.sh's source, create a fresh target image ---
truncate -s ${DST_SIZE_GB}G "$IMG_DST"
LOOP_DST=$(losetup -f)
losetup "$LOOP_DST" "$IMG_DST"
DST=${LOOP_DST#/dev/}
DLOG="/var/log/pfs-$DST.log"
STDOUT_FILE=$(mktemp)
STDERR_FILE=$(mktemp)

echo "  src=$SRC dst=$DST"

if ! "$PFS" mkfs -f "$DST" >/dev/null 2>&1; then
    fail "mkfs $DST failed"
    exit 1
fi

# mkfs mounts the dst and may leave a log; drop it so the assertions below
# only see this fscp run.
rm -f "$DLOG"

# --- run the copy with verification enabled ---
# Run under `set -e`; use an `if` so a non-zero exit is recorded as `rc`
# instead of aborting the script before the result can be reported.
if "$PFS" fscp -c "$SRC" "$DST" >"$STDOUT_FILE" 2>"$STDERR_FILE"; then
    rc=0
else
    rc=$?
fi

if [[ $rc -ne 0 ]]; then
    fail "fscp -c exited $rc (expected 0)"
else
    pass "fscp -c completed successfully"
fi

# XCOM-222 #3: the per-block "crc check pass!" spam is gone from stdout.
if grep -q "crc check pass" "$STDOUT_FILE"; then
    fail "stdout still contains per-block 'crc check pass' spam"
else
    pass "stdout has no per-block 'crc check pass' spam"
fi

# The chunk_copy progress line must still be present (not clobbered).
if grep -q "blocks have been copied" "$STDOUT_FILE"; then
    pass "chunk_copy progress line present on stdout"
else
    fail "chunk_copy progress line missing from stdout"
fi

# XCOM-222 #2: a clean, single-line per-chunk pass message in the log.
# The pfs_itrace line must not carry the backslash-continuation source
# indentation (tabs/spaces) that the old message leaked into the output.
# Anchor both ends so any leading/trailing whitespace in the message fails
# the match.
if grep -qE "^\[PFS_LOG\].* chunk [0-9]+ verified$" "$DLOG"; then
    pass "clean per-chunk pass message in log"
else
    fail "missing clean 'chunk N verified' line in $DLOG"
fi

echo ""
if [[ $FAILURES -ne 0 ]]; then
    echo "=== fscp -c tests: $FAILURES check(s) FAILED ==="
    exit 1
fi
echo "=== fscp -c tests: all checks PASSED ==="
