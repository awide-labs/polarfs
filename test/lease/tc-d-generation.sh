#!/bin/bash
# tc-d-generation.sh — Group D: generation counter tests.
#
# TC-D1: First RW mount of a fresh FS sets generation = 1.
# TC-D2: After a simulated crash (SIGKILL), re-mounting the same host increments
#        the generation counter (old record still on disk → gen+1).
#        After a clean unmount the sector is zeroed so re-mount always starts at 1.

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group D: Generation counter ==="

# Helper: read hr_generation (uint32 at offset 8 within the host record)
# pfs_host_record_t layout:
#   [0]  hr_magic     uint32
#   [4]  hr_flags     uint32
#   [8]  hr_host_id   uint32
#   [12] hr_generation uint32
read_generation() {
    local hostid="$1"
    local sector_offset=$(paxos_sector_offset "$hostid")
    local gen_offset=$(( sector_offset + 12 ))
    local tmp
    tmp=$(mktemp)
    pfs_cmd 1 read -o "$gen_offset" -l 4 \
        "/${TEST_LOOP_DEVICE_NAME}/.pfs-paxos" >"$tmp" 2>/dev/null || {
        rm -f "$tmp"
        echo ""
        return 1
    }
    # od -t u4 gives unsigned 4-byte ints; take the first value
    local val
    val=$(od -t u4 -A n "$tmp" | tr -s ' ' '\n' | grep -v '^$' | head -1)
    rm -f "$tmp"
    echo "$val"
}

# ---------------------------------------------------------------------------
# TC-D1: First mount sets generation = 1 (fresh FS has no prior record)
# ---------------------------------------------------------------------------
echo ""
echo "TC-D1: first RW mount sets generation = 1"

HOLDER_PID=""
if start_rw_holder 1; then
    gen=$(read_generation 1) || gen=""
    stop_holder

    if [[ "$gen" == "1" ]]; then
        pass "TC-D1: generation = 1 on first RW mount"
    else
        fail "TC-D1: expected generation=1, got '$gen'"
    fi
else
    fail "TC-D1: setup: could not mount as host 1"
fi

# ---------------------------------------------------------------------------
# TC-D2: Crash + re-mount increments generation to 2
#
# After a clean unmount the sector is zeroed, so re-mount starts at gen=1 again
# (no history survives the release).  Generation only exceeds 1 when the
# previous record is still on disk — i.e. after a crash (SIGKILL).
# ---------------------------------------------------------------------------
echo ""
echo "TC-D2: crash-recovery re-mount increments generation"

HOLDER_PID=""
if ! start_rw_holder 1; then
    fail "TC-D2: setup: could not mount as host 1 (first mount)"
else
    # Simulate crash: kill without clean release → sector stays on disk with gen=1
    kill -KILL "$HOLDER_PID" 2>/dev/null || true
    wait "$HOLDER_PID" 2>/dev/null || true
    HOLDER_PID=""

    # Wait for lease to expire so the second mount is not blocked
    echo "  Waiting ${LEASE_TEST_DURATION}s for stale lease to expire..."
    sleep $((LEASE_TEST_DURATION + 1))

    if start_rw_holder 1; then
        gen=$(read_generation 1) || gen=""
        stop_holder

        if [[ "$gen" == "2" ]]; then
            pass "TC-D2: generation = 2 after crash-recovery re-mount"
        else
            fail "TC-D2: expected generation=2, got '$gen'"
        fi
    else
        fail "TC-D2: setup: could not mount as host 1 on second attempt"
    fi
fi

# ---------------------------------------------------------------------------
# TC-D3: Generation is independent per host_id
# ---------------------------------------------------------------------------
echo ""
echo "TC-D3: generation counters are independent per host"

HOLDER_PID=""
# Mount host 1 twice with clean unmounts (sector cleared each time, gen resets to 1),
# then mount host 2 once (gen=1) — verify host 2 is independent from host 1
if start_rw_holder 1; then stop_holder; fi
if start_rw_holder 1; then stop_holder; fi

if start_rw_holder 2; then
    gen2=$(read_generation 2) || gen2=""
    stop_holder

    if [[ "$gen2" == "1" ]]; then
        pass "TC-D3: host 2 generation = 1 (independent from host 1)"
    else
        fail "TC-D3: expected host 2 generation=1, got '$gen2'"
    fi
else
    fail "TC-D3: could not mount as host 2"
fi

lease_test_summary
