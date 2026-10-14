#!/bin/bash
# tc-p-fscp-reset.sh — Group P: lease records on a pbd made by fscp or by
# chunk restore.
#
# Both copy the source's paxos file block, host lease records included, and
# must then clear those records on the destination: they belong to the
# source's hosts.  The source gets records for hosts 1 (crashed RW holder),
# 2 and 254 before anything is copied, so a reset that misses any of them
# shows.
#
# TC-P1: After fscp, the destination's host records are empty while the
#        source's are still there, and the copied files are on the
#        destination.
# TC-P2: The crashed holder's host id starts over at generation 1 on the
#        fscp destination, rather than inheriting the source's generation.
# TC-P3: After 'pfs chunk' backup and restore onto a blank pbd,
#        the destination's host records are empty and its files are there.
# TC-P4: Restoring a backup whose paxos leader record is corrupt fails, and
#        the reset writes nothing to the destination's paxos block.
#
# Not covered: write errors while clearing records (nothing here can inject
# them), and the libpfs chunk stream API (pfs_chunk_writestream), which
# calls the same reset as 'pfs chunk' restore.

source "$(dirname "$0")/common.sh"

FSCP_DST_IMAGE_FILE="${FSCP_DST_IMAGE_FILE:-${TEST_IMAGE_FILE%.img}-fscp-dst.img}"
FSCP_DST_LOOP_DEVICE="${FSCP_DST_LOOP_DEVICE:-/dev/loop101}"
FSCP_DST_LOOP_DEVICE_NAME="${FSCP_DST_LOOP_DEVICE_NAME:-loop101}"
CHUNK_BACKUP_FILE="${CHUNK_BACKUP_FILE:-${TEST_IMAGE_FILE%.img}-chunk0.bak}"
CHUNK_REST_BACKUP_FILE="${CHUNK_REST_BACKUP_FILE:-${TEST_IMAGE_FILE%.img}-chunks.bak}"
DST_PFS_LOG="/var/log/pfs-${FSCP_DST_LOOP_DEVICE_NAME}.log"

# PFS_BLOCK_SIZE: the unit of chunk backup streams and of the paxos block.
BLOCK_SIZE=$((4 << 20))
# PBD_CHUNK_SIZE is 10GB.
NCHUNK=$((TEST_IMAGE_SIZE_GB / 10))
# Hosts whose records the source gets before it is copied.
SEEDED_HOSTS=(1 2 254)

detach_dst() {
    fuser -k "$FSCP_DST_LOOP_DEVICE" 2>/dev/null || true
    losetup -d "$FSCP_DST_LOOP_DEVICE" 2>/dev/null || true
    rm -f "$FSCP_DST_IMAGE_FILE"
}

# Give the destination a blank image.
fresh_dst() {
    detach_dst
    truncate -s "${TEST_IMAGE_SIZE_GB}G" "$FSCP_DST_IMAGE_FILE"
    if ! losetup --direct-io=on "$FSCP_DST_LOOP_DEVICE" "$FSCP_DST_IMAGE_FILE"; then
        echo "ERROR: cannot attach loop device $FSCP_DST_LOOP_DEVICE"
        exit 1
    fi
}

fscp_test_teardown() {
    lease_test_teardown
    detach_dst
    rm -f "$CHUNK_BACKUP_FILE" "$CHUNK_REST_BACKUP_FILE"
}

lease_test_setup
trap fscp_test_teardown EXIT
fresh_dst

echo ""
echo "=== Group P: lease reset on copied pbds ==="

# Helper: number of non-zero bytes on stdin.
count_nonzero() {
    od -v -t x1 -A n | tr -s ' \n' '\n' | grep -v '^00$' |
        grep -cv '^$' || true
}

# Helper: read <len> bytes at <offset> of pbd <pbd>'s paxos file to stdout.
read_paxos() {
    local pbd="$1" offset="$2" len="$3"
    pfs_cmd 1 read -o "$offset" -l "$len" "/${pbd}/.pfs-paxos" 2>/dev/null
}

# Helper: number of non-zero bytes in host sector <hostid> of pbd <pbd>, or
# "" if it can't be read.
sector_nonzero() {
    local pbd="$1" hostid="$2"
    local tmp
    tmp=$(mktemp)
    if ! read_paxos "$pbd" "$(paxos_sector_offset "$hostid")" \
        "$LEASE_TEST_SECTOR_SIZE" >"$tmp"; then
        rm -f "$tmp"
        echo ""
        return 1
    fi
    count_nonzero <"$tmp"
    rm -f "$tmp"
}

# Helper: hr_generation (uint32 at offset 12) of host <hostid> on pbd <pbd>.
read_generation() {
    local pbd="$1" hostid="$2"
    read_paxos "$pbd" $(( $(paxos_sector_offset "$hostid") + 12 )) 4 |
        od -t u4 -A n | tr -s ' ' '\n' | grep -v '^$' | head -1
}

# Helper: list the seeded hosts whose record on pbd <pbd> is not empty
# ("want" = empty) or empty ("want" = nonempty), as " host N (...)" items.
seeded_mismatch() {
    local pbd="$1" want="$2" h nz bad=""
    for h in "${SEEDED_HOSTS[@]}"; do
        nz=$(sector_nonzero "$pbd" "$h") || nz=""
        if [[ -z "$nz" ]] ||
            { [[ "$want" == empty ]] && [[ "$nz" -ne 0 ]]; } ||
            { [[ "$want" == nonempty ]] && [[ "$nz" -eq 0 ]]; }; then
            bad+=" host $h (non-zero bytes: '${nz}')"
        fi
    done
    echo "$bad"
}

# Helper: whether <file> is in the root directory of pbd <pbd>.
pbd_has_file() {
    local pbd="$1" file="$2"
    # Not grep -q: pfs must not die of SIGPIPE under pipefail.
    pfs_cmd 1 ls "/${pbd}/" 2>/dev/null | grep "$file" >/dev/null
}

# Helper: write <len> bytes at <offset> of <file> to stdout.
file_bytes() {
    local file="$1" offset="$2" len="$3"
    dd if="$file" bs=1M iflag=skip_bytes,count_bytes skip="$offset" \
        count="$len" status=none
}

# Helper: byte offset of block <blkid> in the chunk backup stream <file>.
# The stream is: header | used-block table (uint32 each) | used blocks in
# table order.  The header is shorter than one block plus its table entry,
# which gives both the header size and the number of blocks.
backup_block_offset() {
    local file="$1" blkid="$2"
    local size hdr nused i id
    size=$(stat -c %s "$file")
    hdr=$(( size % (BLOCK_SIZE + 4) ))
    nused=$(( size / (BLOCK_SIZE + 4) ))
    i=0
    for id in $(file_bytes "$file" "$hdr" $((nused * 4)) | od -v -t u4 -A n); do
        if [[ "$id" -eq "$blkid" ]]; then
            echo $(( hdr + nused * 4 + i * BLOCK_SIZE ))
            return 0
        fi
        i=$((i + 1))
    done
    return 1
}

# ---------------------------------------------------------------------------
# Source setup: crashed RW holder on host 1, valid records for hosts 2, 254
# ---------------------------------------------------------------------------
echo ""
echo "Setup: seed host records 1, 2 and 254 on the source"

seeded=0
HOLDER_PID=""
if ! pfs_cmd 1 touch "/${TEST_LOOP_DEVICE_NAME}/fscp_marker" 2>/dev/null; then
    fail "setup: could not create marker file on the source"
elif ! start_rw_holder 1; then
    fail "setup: could not mount the source as host 1"
else
    # Crash: host 1's RW record stays on the source.
    kill -KILL "$HOLDER_PID" 2>/dev/null || true
    wait "$HOLDER_PID" 2>/dev/null || true
    HOLDER_PID=""

    # Host 50 waits out host 1's record, plants 2 and 254, and clears its
    # own record on unmount.
    if ! (lease_hold 50 write-two-records 2 254) >/dev/null; then
        fail "setup: write-two-records 2 254 failed"
    else
        bad=$(seeded_mismatch "$TEST_LOOP_DEVICE_NAME" nonempty)
        if [[ -n "$bad" ]]; then
            fail "setup: seeded records missing on the source:$bad"
        else
            seeded=1
            pass "setup: source has records for hosts ${SEEDED_HOSTS[*]}"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# TC-P1: fscp clears the copied host records
# ---------------------------------------------------------------------------
echo ""
echo "TC-P1: fscp clears host records on the destination"

fscp_ok=0
if [[ $seeded -ne 1 ]]; then
    fail "TC-P1: skipped, source setup failed"
elif ! "$PFS" -C "$LEASE_TEST_CLUSTER" -E 0 fscp \
    "$TEST_LOOP_DEVICE_NAME" "$FSCP_DST_LOOP_DEVICE_NAME"; then
    fail "TC-P1: fscp failed"
else
    fscp_ok=1
    bad=$(seeded_mismatch "$FSCP_DST_LOOP_DEVICE_NAME" empty)
    if [[ -n "$bad" ]]; then
        fail "TC-P1: destination host records not cleared:$bad"
    else
        pass "TC-P1: destination host records are empty"
    fi

    bad=$(seeded_mismatch "$TEST_LOOP_DEVICE_NAME" nonempty)
    if [[ -n "$bad" ]]; then
        fail "TC-P1: source host records changed:$bad"
    else
        pass "TC-P1: source host records left alone"
    fi

    if pbd_has_file "$FSCP_DST_LOOP_DEVICE_NAME" fscp_marker; then
        pass "TC-P1: copied file present on the destination"
    else
        fail "TC-P1: fscp_marker missing on the destination"
    fi
fi

# ---------------------------------------------------------------------------
# TC-P2: generation starts over on the destination
# ---------------------------------------------------------------------------
echo ""
echo "TC-P2: host 1 starts at generation 1 on the destination"

if [[ $fscp_ok -ne 1 ]]; then
    fail "TC-P2: skipped, fscp did not complete in TC-P1"
elif ! TEST_LOOP_DEVICE_NAME="$FSCP_DST_LOOP_DEVICE_NAME" start_rw_holder 1; then
    fail "TC-P2: host 1 could not mount the destination RW"
else
    gen=$(read_generation "$FSCP_DST_LOOP_DEVICE_NAME" 1) || gen=""
    stop_holder
    if [[ "$gen" == "1" ]]; then
        pass "TC-P2: generation = 1 on the destination"
    else
        fail "TC-P2: expected generation=1, got '$gen'"
    fi
fi

# ---------------------------------------------------------------------------
# TC-P3: chunk restore clears the restored host records
# ---------------------------------------------------------------------------
echo ""
echo "TC-P3: chunk restore clears host records on the destination"

backup_ok=0
paxos_off=""
if [[ $seeded -ne 1 ]]; then
    fail "TC-P3: skipped, source setup failed"
elif ! "$PFS" -C "$LEASE_TEST_CLUSTER" -E 0 chunk -m backup -c 0 \
    "$TEST_LOOP_DEVICE_NAME" >"$CHUNK_BACKUP_FILE"; then
    fail "TC-P3: chunk 0 backup of the source failed"
elif [[ $NCHUNK -gt 1 ]] &&
    ! "$PFS" -C "$LEASE_TEST_CLUSTER" -E 0 chunk -m backup -c 1 -e "$NCHUNK" \
    "$TEST_LOOP_DEVICE_NAME" >"$CHUNK_REST_BACKUP_FILE"; then
    fail "TC-P3: chunk 1..$((NCHUNK - 1)) backup of the source failed"
elif ! paxos_off=$(backup_block_offset "$CHUNK_BACKUP_FILE" 1); then
    fail "TC-P3: paxos block (block 1) not found in the backup"
else
    # The records the restore must clear are in the stream itself.
    bad=""
    for h in "${SEEDED_HOSTS[@]}"; do
        nz=$(file_bytes "$CHUNK_BACKUP_FILE" \
            $((paxos_off + $(paxos_sector_offset "$h"))) \
            "$LEASE_TEST_SECTOR_SIZE" | count_nonzero)
        if [[ "$nz" -eq 0 ]]; then
            bad+=" host $h"
        fi
    done
    if [[ -n "$bad" ]]; then
        fail "TC-P3: backup lacks records for:$bad"
    else
        backup_ok=1
        fresh_dst
        touch "$CHUNK_REST_BACKUP_FILE"
        if ! cat "$CHUNK_BACKUP_FILE" "$CHUNK_REST_BACKUP_FILE" |
            "$PFS" -C "$LEASE_TEST_CLUSTER" -E 0 chunk -m restore -c 0 \
            -e "$NCHUNK" "$FSCP_DST_LOOP_DEVICE_NAME" >/dev/null; then
            fail "TC-P3: chunk restore failed"
        else
            bad=$(seeded_mismatch "$FSCP_DST_LOOP_DEVICE_NAME" empty)
            if [[ -n "$bad" ]]; then
                fail "TC-P3: destination host records not cleared:$bad"
            else
                pass "TC-P3: destination host records are empty"
            fi

            if pbd_has_file "$FSCP_DST_LOOP_DEVICE_NAME" fscp_marker; then
                pass "TC-P3: restored file present on the destination"
            else
                fail "TC-P3: fscp_marker missing on the destination"
            fi
        fi
    fi
fi

# ---------------------------------------------------------------------------
# TC-P4: a corrupt leader record makes the reset fail without writing
# ---------------------------------------------------------------------------
echo ""
echo "TC-P4: restore with a corrupt leader record fails, writes no records"

if [[ $backup_ok -ne 1 ]]; then
    fail "TC-P4: skipped, no usable backup from TC-P3"
else
    # Flip a byte of the leader record's num_hosts (offset 16), which the
    # checksum covers.
    byte=$(file_bytes "$CHUNK_BACKUP_FILE" $((paxos_off + 16)) 1 |
        od -A n -t u1 | tr -d ' ')
    printf "\\$(printf '%03o' $(( byte ^ 0xff )))" |
        dd of="$CHUNK_BACKUP_FILE" bs=1 seek=$((paxos_off + 16)) \
        conv=notrunc status=none

    fresh_dst
    offset=$(stat -c %s "$DST_PFS_LOG" 2>/dev/null || echo 0)
    if "$PFS" -C "$LEASE_TEST_CLUSTER" -E 0 chunk -m restore -c 0 \
        "$FSCP_DST_LOOP_DEVICE_NAME" <"$CHUNK_BACKUP_FILE" >/dev/null; then
        fail "TC-P4: chunk restore succeeded despite a corrupt leader record"
    else
        pass "TC-P4: chunk restore failed"
    fi

    if tail -c +"$((offset + 1))" "$DST_PFS_LOG" 2>/dev/null |
        grep "cant read paxos leader" >/dev/null; then
        pass "TC-P4: reset reported the unreadable leader record"
    else
        fail "TC-P4: 'cant read paxos leader' not logged in $DST_PFS_LOG"
    fi

    # The destination's paxos block must hold exactly what the restore
    # wrote: the stream's block, seeded records included.
    if cmp -s <(file_bytes "$CHUNK_BACKUP_FILE" "$paxos_off" "$BLOCK_SIZE") \
        <(dd if="$FSCP_DST_LOOP_DEVICE" bs="$BLOCK_SIZE" skip=1 count=1 \
        iflag=direct status=none); then
        pass "TC-P4: reset left the destination's paxos block untouched"
    else
        fail "TC-P4: destination's paxos block differs from the restored one"
    fi
fi

lease_test_summary
