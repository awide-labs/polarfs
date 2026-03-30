#!/bin/bash
# common.sh — shared helpers for cross-host RW lease tests.
#
# Source this file at the top of each test script:
#   source "$(dirname "$0")/common.sh"

set -euo pipefail

# ---------------------------------------------------------------------------
# Environment (override via exports before sourcing)
# ---------------------------------------------------------------------------
TEST_IMAGE_FILE="${TEST_IMAGE_FILE:-/tmp/polarfs_disk.img}"
TEST_LOOP_DEVICE="${TEST_LOOP_DEVICE:-/dev/loop100}"
TEST_LOOP_DEVICE_NAME="${TEST_LOOP_DEVICE_NAME:-loop100}"
TEST_IMAGE_SIZE_GB="${TEST_IMAGE_SIZE_GB:-10}"
LEASE_TEST_SECTOR_SIZE="${LEASE_TEST_SECTOR_SIZE:-4096}"
LEASE_TEST_DURATION="${LEASE_TEST_DURATION:-5}"  # seconds (short for CI)
LEASE_TEST_CLUSTER="${LEASE_TEST_CLUSTER:-disk}"
LEASE_TEST_TRACE_PLEVEL="${LEASE_TEST_TRACE_PLEVEL:-3}"  # 3=info, 4=debug

# Paths to binaries (build output directory)
BINDIR="${BINDIR:-$(cd "$(dirname "$0")/../../bin" && pwd)}"
PFS="${BINDIR}/pfs"
PFS_LEASE_HOLD="${BINDIR}/pfs_lease_hold"

# Temp config file — uses PFS_CONFIG_PATH env var so we never touch /etc/polarfs.conf
LEASE_TEST_CONF="$(mktemp /tmp/pfs-lease-test-conf.XXXXXX)"
export PFS_CONFIG_PATH="$LEASE_TEST_CONF"

# Log file.  pfs_trace_redirect (for MNTFLG_TOOL mounts) opens this with
# O_APPEND and dup2's it onto stderr.  pfs_lease_hold is not a tool mount
# so its traces go to stderr — we redirect stderr to the same file via
# shell 2>> (also O_APPEND).  Both paths are safe for concurrent writes.
PFS_LOG="/var/log/pfs-${TEST_LOOP_DEVICE_NAME}.log"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Wrapper: runs pfs_lease_hold with the test cluster/pbdname and stderr
# redirected to $PFS_LEASE_HOLD_LOG.  Pass remaining args (hostid, mode, …).
# Uses exec so that when backgrounded with &, $! is the real pfs_lease_hold
# PID (not a bash subshell), and signals from stop_holder reach it directly.
# Callers that run lease_hold in the foreground (not &) must wrap it in a
# subshell: (lease_hold …) or $(lease_hold …).
lease_hold() {
    exec "$PFS_LEASE_HOLD" "$LEASE_TEST_CLUSTER" "$TEST_LOOP_DEVICE_NAME" \
        "$@" 2>>"$PFS_LOG"
}

# Wrapper: runs pfs tool with the test cluster and -E 0 (no pfsd).
# Usage: pfs_cmd <hostid> <subcommand> [args…]
pfs_cmd() {
    local hostid="$1"; shift
    "$PFS" -H "$hostid" -C "$LEASE_TEST_CLUSTER" -E 0 "$@"
}

# Pass/fail bookkeeping
TESTS_PASSED=0
TESTS_FAILED=0

pass() {
    local name="$1"
    echo "  PASS: $name"
    TESTS_PASSED=$((TESTS_PASSED + 1))
}

fail() {
    local name="$1"
    local reason="${2:-}"
    echo "  FAIL: $name${reason:+ — $reason}"
    TESTS_FAILED=$((TESTS_FAILED + 1))
}

assert_exit0() {
    local name="$1"
    shift
    if "$@"; then
        pass "$name"
    else
        fail "$name" "command exited non-zero: $*"
    fi
}

assert_exit_nonzero() {
    local name="$1"
    shift
    if "$@"; then
        fail "$name" "expected failure but command succeeded: $*"
    else
        pass "$name"
    fi
}

# Run command and check that its exit code matches expected
assert_exit() {
    local name="$1"
    local expected_exit="$2"
    shift 2
    local actual_exit=0
    "$@" || actual_exit=$?
    if [[ $actual_exit -eq $expected_exit ]]; then
        pass "$name"
    else
        fail "$name" "expected exit $expected_exit, got $actual_exit: $*"
    fi
}

# Check that a string appears in the PFS log
assert_log_contains() {
    local name="$1"
    local pattern="$2"
    if grep -qE "$pattern" "$PFS_LOG" 2>/dev/null; then
        pass "$name"
    else
        fail "$name" "pattern '$pattern' not found in $PFS_LOG"
    fi
}

# Mount RW and keep mount open (via pfs_lease_hold).
# Sets HOLDER_PID.  Waits for "MOUNTED" from the helper before returning.
# Returns non-zero if mount fails.
start_rw_holder() {
    local hostid="$1"
    local tmpout
    tmpout=$(mktemp)

    lease_hold "$hostid" rw >"$tmpout" &
    HOLDER_PID=$!

    # Wait for "MOUNTED" (or process death meaning failure).
    # Budget: pfs_meta_load_all_chunks (≤8s under CI load) +
    # pfs_rw_lease_wait_and_check in crash-recovery scenario
    # (≤LEASE_TEST_DURATION) + ballot prepare/verify/acquire (≤2s) +
    # log start/replay/poll (≤5s) + scheduling jitter (5s).
    local deadline=$((SECONDS + LEASE_TEST_DURATION + 20))
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -q "^MOUNTED$" "$tmpout" 2>/dev/null; then
            rm -f "$tmpout"
            return 0
        fi
        if ! kill -0 "$HOLDER_PID" 2>/dev/null; then
            # Process died — mount failed
            wait "$HOLDER_PID" 2>/dev/null || true
            rm -f "$tmpout"
            HOLDER_PID=""
            return 1
        fi
        sleep 0.2
    done

    # Timeout — kill the helper
    kill "$HOLDER_PID" 2>/dev/null || true
    wait "$HOLDER_PID" 2>/dev/null || true
    rm -f "$tmpout"
    HOLDER_PID=""
    return 1
}

# Stop a running holder gracefully (SIGTERM → pfs_umount).
stop_holder() {
    local pid="${1:-${HOLDER_PID:-}}"
    [[ -z "$pid" ]] && return 0
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    HOLDER_PID=""
}

# Mount RO and keep mount open (via pfs_lease_hold).
# Sets HOLDER_PID.  Waits for "MOUNTED" from the helper before returning.
# Returns non-zero if mount fails.
start_ro_holder() {
    local hostid="$1"
    local tmpout
    tmpout=$(mktemp)

    lease_hold "$hostid" ro >"$tmpout" &
    HOLDER_PID=$!

    local deadline=$((SECONDS + 10))
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -q "^MOUNTED$" "$tmpout" 2>/dev/null; then
            rm -f "$tmpout"
            return 0
        fi
        if ! kill -0 "$HOLDER_PID" 2>/dev/null; then
            wait "$HOLDER_PID" 2>/dev/null || true
            rm -f "$tmpout"
            HOLDER_PID=""
            return 1
        fi
        sleep 0.2
    done

    kill "$HOLDER_PID" 2>/dev/null || true
    wait "$HOLDER_PID" 2>/dev/null || true
    rm -f "$tmpout"
    HOLDER_PID=""
    return 1
}

# Start a holder in "promote" mode: mounts RO, then waits for trigger_promote
# to call pfs_remount() in-place.  Sets HOLDER_PID and PROMOTE_TMPOUT.
# Waits for "MOUNTED_RO" before returning.  Returns non-zero on failure.
PROMOTE_TMPOUT=""
start_promote_holder() {
    local hostid="$1"
    local tmpout
    tmpout=$(mktemp)
    PROMOTE_TMPOUT="$tmpout"

    lease_hold "$hostid" promote >"$tmpout" &
    HOLDER_PID=$!

    local deadline=$((SECONDS + 10))
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -q "^MOUNTED_RO$" "$tmpout" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "$HOLDER_PID" 2>/dev/null; then
            wait "$HOLDER_PID" 2>/dev/null || true
            rm -f "$tmpout"
            PROMOTE_TMPOUT=""; HOLDER_PID=""
            return 1
        fi
        sleep 0.2
    done
    kill "$HOLDER_PID" 2>/dev/null || true
    wait "$HOLDER_PID" 2>/dev/null || true
    rm -f "$tmpout"
    PROMOTE_TMPOUT=""; HOLDER_PID=""
    return 1
}

# Send SIGUSR1 to the promote holder and wait for the result.
# Returns 0 (sets PROMOTE_RESULT="ok") on success,
#         1 (sets PROMOTE_RESULT="REMOUNT_FAILED:<errno>") on failure.
# pid defaults to $HOLDER_PID.  tmpout defaults to $PROMOTE_TMPOUT.
PROMOTE_RESULT=""
trigger_promote() {
    local pid="${1:-${HOLDER_PID:-}}"
    local tmpout="${2:-${PROMOTE_TMPOUT:-}}"
    [[ -z "$pid" ]] && return 1

    kill -USR1 "$pid" 2>/dev/null || return 1

    # Budget: pfs_log_suspend (≤3s) + wait_and_check (≤LEASE_TEST_DURATION) +
    # ballot prepare/verify/acquire (≤1s) + pfs_mount_sync/orphans_reclaim/
    # admin_init (≤15s under CI load with accumulated journal entries) +
    # polling jitter (3s).
    local deadline=$(( SECONDS + LEASE_TEST_DURATION + 25 ))
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -q "^MOUNTED_RW$" "$tmpout" 2>/dev/null; then
            rm -f "$tmpout"; PROMOTE_TMPOUT=""
            PROMOTE_RESULT="ok"
            return 0
        fi
        local failed
        failed=$(grep "^REMOUNT_FAILED:" "$tmpout" 2>/dev/null | head -1) || true
        if [[ -n "$failed" ]]; then
            rm -f "$tmpout"; PROMOTE_TMPOUT=""
            PROMOTE_RESULT="$failed"
            return 1
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            rm -f "$tmpout"; PROMOTE_TMPOUT=""
            return 1
        fi
        sleep 0.2
    done
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$tmpout"; PROMOTE_TMPOUT=""
    return 1
}

# Start a holder in "demote" mode: mounts RW with rw_hostid, registers an
# additional RO client with ro_hostid, then waits for trigger_demote to call
# pfs_mount_release(rw_hostid), which triggers pfs_remount_ro() internally.
# Sets HOLDER_PID and DEMOTE_TMPOUT.
# Waits for "MOUNTED_RW" before returning.  Returns non-zero on failure.
DEMOTE_TMPOUT=""
start_demote_holder() {
    local rw_hostid="$1"
    local ro_hostid="$2"
    local tmpout
    tmpout=$(mktemp)
    DEMOTE_TMPOUT="$tmpout"

    lease_hold "$rw_hostid" demote "$ro_hostid" >"$tmpout" &
    HOLDER_PID=$!

    local deadline=$((SECONDS + 15))
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -q "^MOUNTED_RW$" "$tmpout" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "$HOLDER_PID" 2>/dev/null; then
            wait "$HOLDER_PID" 2>/dev/null || true
            rm -f "$tmpout"
            DEMOTE_TMPOUT=""; HOLDER_PID=""
            return 1
        fi
        sleep 0.2
    done
    kill "$HOLDER_PID" 2>/dev/null || true
    wait "$HOLDER_PID" 2>/dev/null || true
    rm -f "$tmpout"
    DEMOTE_TMPOUT=""; HOLDER_PID=""
    return 1
}

# Send SIGUSR1 to the demote holder and wait for the result.
# Returns 0 (sets DEMOTE_RESULT="ok") on success,
#         1 (sets DEMOTE_RESULT="DEMOTE_FAILED:<errno>" or "died") on failure.
# pid defaults to $HOLDER_PID.  tmpout defaults to $DEMOTE_TMPOUT.
DEMOTE_RESULT=""
trigger_demote() {
    local pid="${1:-${HOLDER_PID:-}}"
    local tmpout="${2:-${DEMOTE_TMPOUT:-}}"
    [[ -z "$pid" ]] && return 1

    kill -USR1 "$pid" 2>/dev/null || return 1

    # pfs_remount_ro is synchronous; give it up to 10s
    local deadline=$(( SECONDS + 10 ))
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -q "^DEMOTED_RO$" "$tmpout" 2>/dev/null; then
            rm -f "$tmpout"; DEMOTE_TMPOUT=""
            DEMOTE_RESULT="ok"
            return 0
        fi
        local failed
        failed=$(grep "^DEMOTE_FAILED:" "$tmpout" 2>/dev/null | head -1) || true
        if [[ -n "$failed" ]]; then
            rm -f "$tmpout"; DEMOTE_TMPOUT=""
            DEMOTE_RESULT="$failed"
            return 1
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            rm -f "$tmpout"; DEMOTE_TMPOUT=""
            DEMOTE_RESULT="died"
            return 1
        fi
        sleep 0.2
    done
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$tmpout"; DEMOTE_TMPOUT=""
    DEMOTE_RESULT="timeout"
    return 1
}

# Start a holder in "self-corrupt-renew" mode.
# The holder mounts RW as hostid and waits for SIGUSR1.  On SIGUSR1 it
# corrupts its own paxos sector (bad checksum) and waits 3s for the log
# thread to self-heal, then reports the result.
# Sets HOLDER_PID and SELF_CORRUPT_TMPOUT.  Waits for "MOUNTED".
SELF_CORRUPT_TMPOUT=""
SELF_CORRUPT_RESULT=""
start_self_corrupt_holder() {
    local hostid="$1"
    local tmpout
    tmpout=$(mktemp)
    SELF_CORRUPT_TMPOUT="$tmpout"

    lease_hold "$hostid" self-corrupt-renew >"$tmpout" &
    HOLDER_PID=$!

    local deadline=$((SECONDS + 15))
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -q "^MOUNTED$" "$tmpout" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "$HOLDER_PID" 2>/dev/null; then
            wait "$HOLDER_PID" 2>/dev/null || true
            rm -f "$tmpout"; SELF_CORRUPT_TMPOUT=""; HOLDER_PID=""
            return 1
        fi
        sleep 0.2
    done
    kill "$HOLDER_PID" 2>/dev/null || true
    wait "$HOLDER_PID" 2>/dev/null || true
    rm -f "$tmpout"; SELF_CORRUPT_TMPOUT=""; HOLDER_PID=""
    return 1
}

# Send SIGUSR1 to the self-corrupt holder and wait for the result.
# The holder will corrupt its own sector, sleep 3s, then report
# "SELF_HEALED" or "SELF_HEAL_FAILED:<rv>".  Timeout 15s.
# Returns 0 and sets SELF_CORRUPT_RESULT="healed" on success.
trigger_self_corrupt() {
    local pid="${1:-${HOLDER_PID:-}}"
    local tmpout="${2:-${SELF_CORRUPT_TMPOUT:-}}"
    [[ -z "$pid" ]] && return 1

    kill -USR1 "$pid" 2>/dev/null || return 1

    # Wait for CORRUPTED first (fast)
    local deadline=$((SECONDS + 5))
    while [[ $SECONDS -lt $deadline ]]; do
        grep -q "^CORRUPTED$" "$tmpout" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || { SELF_CORRUPT_RESULT="died"; return 1; }
        sleep 0.2
    done

    # Then wait for SELF_HEALED / SELF_HEAL_FAILED (up to 10s: 3s sleep + buffer)
    deadline=$((SECONDS + 10))
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -q "^SELF_HEALED$" "$tmpout" 2>/dev/null; then
            rm -f "$tmpout"; SELF_CORRUPT_TMPOUT=""
            SELF_CORRUPT_RESULT="healed"
            return 0
        fi
        local failed
        failed=$(grep "^SELF_HEAL_FAILED:" "$tmpout" 2>/dev/null | head -1) || true
        if [[ -n "$failed" ]]; then
            rm -f "$tmpout"; SELF_CORRUPT_TMPOUT=""
            SELF_CORRUPT_RESULT="$failed"
            return 1
        fi
        kill -0 "$pid" 2>/dev/null || { SELF_CORRUPT_RESULT="died"; return 1; }
        sleep 0.2
    done
    SELF_CORRUPT_RESULT="timeout"
    return 1
}

# Start a holder in "watchdog-stall" mode: mounts RW, then waits for
# trigger_watchdog_stall to suspend the log thread (stops renewal petting)
# while keeping the timer-kill watchdog armed.  The process should be
# killed by SIGUSR2→SIGKILL after paxos_lease_duration seconds.
# Sets HOLDER_PID and WATCHDOG_STALL_TMPOUT.  Waits for "MOUNTED".
WATCHDOG_STALL_TMPOUT=""
start_watchdog_stall_holder() {
    local hostid="$1"
    local tmpout
    tmpout=$(mktemp)
    WATCHDOG_STALL_TMPOUT="$tmpout"

    lease_hold "$hostid" watchdog-stall >"$tmpout" &
    HOLDER_PID=$!

    local deadline=$((SECONDS + LEASE_TEST_DURATION + 20))
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -q "^MOUNTED$" "$tmpout" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "$HOLDER_PID" 2>/dev/null; then
            wait "$HOLDER_PID" 2>/dev/null || true
            rm -f "$tmpout"; WATCHDOG_STALL_TMPOUT=""; HOLDER_PID=""
            return 1
        fi
        sleep 0.2
    done
    kill "$HOLDER_PID" 2>/dev/null || true
    wait "$HOLDER_PID" 2>/dev/null || true
    rm -f "$tmpout"; WATCHDOG_STALL_TMPOUT=""; HOLDER_PID=""
    return 1
}

# Send SIGUSR1 to the watchdog-stall holder (suspends log, stops petting).
# Then wait for the process to die (killed by watchdog timer).
# Returns 0 if the process was killed by signal (exit >= 128).
# Timeout: 2 × paxos_lease_duration + buffer.
trigger_watchdog_stall() {
    local pid="${1:-${HOLDER_PID:-}}"
    local tmpout="${2:-${WATCHDOG_STALL_TMPOUT:-}}"
    [[ -z "$pid" ]] && return 1

    kill -USR1 "$pid" 2>/dev/null || return 1

    # Wait for "STALLED" confirmation (should be fast)
    local deadline=$((SECONDS + 5))
    while [[ $SECONDS -lt $deadline ]]; do
        grep -q "^STALLED$" "$tmpout" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.2
    done

    # Now wait for the process to die from the watchdog kill.
    # Timer fires after paxos_lease_duration; add buffer for scheduling.
    deadline=$((SECONDS + LEASE_TEST_DURATION * 2 + 5))
    while [[ $SECONDS -lt $deadline ]]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            local exit_code=0
            wait "$pid" 2>/dev/null || exit_code=$?
            rm -f "$tmpout"; WATCHDOG_STALL_TMPOUT=""
            HOLDER_PID=""
            # Killed by signal → exit code >= 128 (137 for SIGKILL)
            if [[ $exit_code -ge 128 ]]; then
                return 0
            fi
            return 1
        fi
        sleep 0.2
    done

    # Timed out — process didn't die; kill it manually
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$tmpout"; WATCHDOG_STALL_TMPOUT=""
    HOLDER_PID=""
    return 1
}

# Try a single-shot RW mount (touch /pbdname/probe_file), return exit code.
try_rw_mount() {
    local hostid="$1"
    local rv=0
    pfs_cmd "$hostid" touch "/${TEST_LOOP_DEVICE_NAME}/probe_${hostid}" 2>/dev/null || rv=$?
    return $rv
}

# Try a single-shot RO mount (ls /pbdname/), return exit code.
try_ro_mount() {
    local hostid="$1"
    local rv=0
    pfs_cmd "$hostid" ls "/${TEST_LOOP_DEVICE_NAME}/" 2>/dev/null || rv=$?
    return $rv
}

# Compute byte offset of host record sector on the raw device.
# Sector 0 = leader record.  Sector host_id = host record.
paxos_sector_offset() {
    local hostid="$1"
    echo $(( hostid * LEASE_TEST_SECTOR_SIZE ))
}

# Read the 8-byte hr_timestamp field from a host sector on the raw block device.
# The paxos file is an inner PFS file; on a freshly formatted FS we rely on
# pfs read to extract it.  For raw inspection we use 'pfs read'.
# Returns the decimal timestamp, or "" on error.
read_lease_timestamp_raw() {
    local hostid="$1"
    # Offset of hr_timestamp within pfs_host_record_t:
    #   hr_magic(4) + hr_flags(4) + hr_host_id(4) + hr_generation(4) = 16 bytes
    local field_offset=16
    local sector_offset=$(paxos_sector_offset "$hostid")
    local byte_offset=$(( sector_offset + field_offset ))

    # Use pfs read to get bytes from the .pfs-paxos inner file
    local tmp
    tmp=$(mktemp)
    pfs_cmd "$hostid" read -o "$byte_offset" -l 8 \
        "/${TEST_LOOP_DEVICE_NAME}/.pfs-paxos" \
        >"$tmp" 2>/dev/null || { rm -f "$tmp"; echo ""; return 1; }

    # Interpret as little-endian uint64 via od (suppress address, get first value)
    local val
    val=$(od -t u8 -A n "$tmp" 2>/dev/null | tr -s ' ' '\n' | grep -v '^$' | head -1)
    rm -f "$tmp"
    echo "$val"
}

# Check that a host's sector is all zeros (lease released).
# Uses pfs read to inspect the sector.
assert_sector_zeroed() {
    local name="$1"
    local hostid="$2"
    local sector_offset=$(paxos_sector_offset "$hostid")
    local tmp
    tmp=$(mktemp)
    pfs_cmd "$hostid" read -o "$sector_offset" -l "$LEASE_TEST_SECTOR_SIZE" \
        "/${TEST_LOOP_DEVICE_NAME}/.pfs-paxos" >"$tmp" 2>/dev/null || {
        rm -f "$tmp"
        fail "$name" "could not read paxos sector $hostid"
        return
    }
    local nonzero
    nonzero=$(od -v -t x1 -A n "$tmp" | tr -s ' \n' '\n' | grep -v '^00$' | grep -v '^$' | wc -l) || true
    if [[ "$nonzero" -ne 0 ]]; then
        rm -f "$tmp"
        fail "$name" "sector $hostid is not zeroed ($nonzero non-zero bytes)"
        return
    fi
    rm -f "$tmp"
    pass "$name"
}

# ---------------------------------------------------------------------------
# Setup / teardown
# ---------------------------------------------------------------------------

lease_test_setup() {
    echo "=== lease_test_setup ==="

    # Write test config (picked up via PFS_CONFIG_PATH, never touches /etc)
    cat >"$LEASE_TEST_CONF" <<EOF
[pfs]
paxos_lease_duration=${LEASE_TEST_DURATION}
trace_plevel=${LEASE_TEST_TRACE_PLEVEL}
EOF

    mkdir -p /var/run/pfs /var/log
    # Truncate so previous run's traces don't mix with this run, but the
    # file survives for post-mortem if a test fails mid-run.
    : > "$PFS_LOG"

    # Kill any processes holding the loop device, then force-detach it.
    # This handles leftover state from a previous incomplete test run.
    fuser -k "$TEST_LOOP_DEVICE" 2>/dev/null || true
    losetup -d "$TEST_LOOP_DEVICE" 2>/dev/null || true

    # Create and attach image
    rm -f "$TEST_IMAGE_FILE"
    touch "$TEST_IMAGE_FILE"
    fallocate -l "${TEST_IMAGE_SIZE_GB}G" "$TEST_IMAGE_FILE"
    if ! losetup --direct-io=on "$TEST_LOOP_DEVICE" "$TEST_IMAGE_FILE"; then
        echo "ERROR: cannot attach loop device"
        exit 1
    fi

    # Format filesystem with max num_hosts so any host_id up to 254 works
    "$PFS" -C "$LEASE_TEST_CLUSTER" mkfs \
        -f -s "$LEASE_TEST_SECTOR_SIZE" -u 254 "$TEST_LOOP_DEVICE_NAME"

    echo "=== setup complete (loop=$TEST_LOOP_DEVICE, duration=${LEASE_TEST_DURATION}s) ==="
}

lease_test_teardown() {
    echo "=== lease_test_teardown ==="

    # Stop any stale holders
    if [[ -n "${HOLDER_PID:-}" ]]; then
        stop_holder "$HOLDER_PID"
    fi

    # Kill any pfs_lease_hold processes that may have leaked (e.g., from
    # tests that manage two holders and forgot to track one of them).
    pkill -TERM -f "pfs_lease_hold.*${TEST_LOOP_DEVICE_NAME}" 2>/dev/null || true
    sleep 0.5
    pkill -KILL -f "pfs_lease_hold.*${TEST_LOOP_DEVICE_NAME}" 2>/dev/null || true
    sleep 0.2

    # Detach loop device — retry up to 5 times in case a process is still
    # releasing file descriptors.
    local i
    for i in 1 2 3 4 5; do
        if losetup -d "$TEST_LOOP_DEVICE" 2>/dev/null; then
            break
        fi
        sleep 1
    done

    # Remove image and temp config
    rm -f "$TEST_IMAGE_FILE"
    rm -f "$LEASE_TEST_CONF"

    echo "=== teardown complete ==="
}

lease_test_summary() {
    echo ""
    echo "Results: $TESTS_PASSED passed, $TESTS_FAILED failed"
    if [[ $TESTS_FAILED -gt 0 ]]; then
        exit 1
    fi
}
