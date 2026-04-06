#!/bin/bash
# tc-o-refcount.sh — Group O: Multi-client refcount verification tests.
#
# These tests exercise the pfsd mount refcount system (pfs_host_incref/decref,
# pfsd_mnt_ref_count, pfsd_mnt_wrref_count) and host_id transfer logic in
# pfs_mount_release with multiple clients using different host_ids.
#
# TC-O1: RW + RO lifecycle — acquire RW(A), RO(B), release A (demote),
#        verify host_id transfer to B, release B (umount).
# TC-O2: RO-only lifecycle — two RO clients, release first transfers identity.
# TC-O3: Rejection cases — repeat RW same host, RO on RW same host, RW on RO.
# TC-O4: Three-client demote chain — RW(A) + RO(B) + RO(C), chained release.

source "$(dirname "$0")/common.sh"

lease_test_setup
trap lease_test_teardown EXIT

echo ""
echo "=== Group O: Multi-client refcount verification ==="

# ---------------------------------------------------------------------------
# TC-O1: RW + RO lifecycle with refcount verification
# ---------------------------------------------------------------------------
echo ""
echo "TC-O1: RW + RO multi-client lifecycle"

tmpout=$(mktemp)
(lease_hold 1 refcount-multi 2 >"$tmpout" 2>>"$PFS_LOG")
rc=$?

if [[ $rc -eq 0 ]] && grep -q "^ALL_PHASES_OK$" "$tmpout"; then
    pass "TC-O1"
else
    fail "TC-O1" "exit=$rc, output: $(cat "$tmpout")"
fi
rm -f "$tmpout"

# ---------------------------------------------------------------------------
# TC-O2: RO-only multi-client lifecycle
# ---------------------------------------------------------------------------
echo ""
echo "TC-O2: RO-only multi-client lifecycle"

# Reformat so the previous umount left a clean state
"$PFS" -C "$LEASE_TEST_CLUSTER" mkfs \
    -f -s "$LEASE_TEST_SECTOR_SIZE" -u 254 "$TEST_LOOP_DEVICE_NAME"

tmpout=$(mktemp)
(lease_hold 3 refcount-ro-only 4 >"$tmpout" 2>>"$PFS_LOG")
rc=$?

if [[ $rc -eq 0 ]] && grep -q "^ALL_PHASES_OK$" "$tmpout"; then
    pass "TC-O2"
else
    fail "TC-O2" "exit=$rc, output: $(cat "$tmpout")"
fi
rm -f "$tmpout"

# ---------------------------------------------------------------------------
# TC-O3: Same-host rejection cases
# ---------------------------------------------------------------------------
echo ""
echo "TC-O3: same-host rejection cases"

"$PFS" -C "$LEASE_TEST_CLUSTER" mkfs \
    -f -s "$LEASE_TEST_SECTOR_SIZE" -u 254 "$TEST_LOOP_DEVICE_NAME"

tmpout=$(mktemp)
(lease_hold 5 refcount-reject >"$tmpout" 2>>"$PFS_LOG")
rc=$?

if [[ $rc -eq 0 ]] && grep -q "^ALL_PHASES_OK$" "$tmpout"; then
    pass "TC-O3"
else
    fail "TC-O3" "exit=$rc, output: $(cat "$tmpout")"
fi
rm -f "$tmpout"

# ---------------------------------------------------------------------------
# TC-O4: Three-client demote chain
# ---------------------------------------------------------------------------
echo ""
echo "TC-O4: three-client demote chain"

"$PFS" -C "$LEASE_TEST_CLUSTER" mkfs \
    -f -s "$LEASE_TEST_SECTOR_SIZE" -u 254 "$TEST_LOOP_DEVICE_NAME"

tmpout=$(mktemp)
(lease_hold 10 refcount-three-client 20 30 >"$tmpout" 2>>"$PFS_LOG")
rc=$?

if [[ $rc -eq 0 ]] && grep -q "^ALL_PHASES_OK$" "$tmpout"; then
    pass "TC-O4"
else
    fail "TC-O4" "exit=$rc, output: $(cat "$tmpout")"
fi
rm -f "$tmpout"

# ---------------------------------------------------------------------------
lease_test_summary
