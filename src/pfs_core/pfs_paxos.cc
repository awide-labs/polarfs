/*
 * Copyright (c) 2017-2021, Alibaba Group Holding Limited
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/types.h>
#include <sys/time.h>

#include <assert.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <syslog.h>

/* for watchdog and timer-kill support */
#include <sys/ioctl.h>
#include <signal.h>

#include "pfs_impl.h"
#include "pfs_api.h"
#include "pfs_util.h"
#include "pfs_mount.h"
#include "pfs_dir.h"
#include "pfs_file.h"
#include "pfs_paxos.h"
#include "pfs_trace.h"
#include "pfs_option.h"


#define	PFS_MAX_DISKS		4

static int64_t paxos_lease_duration      = 30; /* seconds until another host may steal RW */
static int64_t paxos_watchdog_enable     = 0;  /* 0=disabled, 1=enable /dev/watchdog */
PFS_OPTION_REG(paxos_lease_duration,       pfs_check_ival_normal);
PFS_OPTION_REG(paxos_watchdog_enable,      pfs_check_ival_normal);

/* forward declarations: static helpers defined later in this file */
static int pfs_rw_lease_check(pfs_mount_t *mnt, uint32_t *blocker_ret);
static int pfs_rw_lease_check_conflict(pfs_mount_t *mnt);
static int pfs_rw_lease_wait_and_check(pfs_mount_t *mnt, uint32_t blocker_id);
static int pfs_host_record_clear(pfs_mount_t *mnt, uint32_t host_id);

static uint64_t
pfs_ballot_generate(pfs_mount_t *mnt)
{
	return mnt->mnt_host_generation * mnt->mnt_num_hosts + mnt->mnt_host_id;
}

/*
 * Macros to substitute for functions in original code.
 */

#define	leader_record_in(a, b)	(*(b) = *(a))
#define	leader_record_out(a, b)	(*(b) = *(a))

#define	request_record_in(a, b)	(*(a) = *(b))
#define	request_record_out(a, b) (*(b) = *(a))

#define	cpu_to_le32(a)		(a)

static inline int
direct_align(size_t sector_size)
{
	if (sector_size == 512)
		return 1024 * 1024;

	if (sector_size == 4096)
		return 4 * 1024 * 1024;

	return -EINVAL;
}

static uint64_t
monotime(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

int
get_rand(int a, int b)
{
#if notyet
#endif
	return -1;
}

static uint32_t
roundup_power_of_two(uint32_t val)
{
	val--;
	val |= val >> 1;
	val |= val >> 2;
	val |= val >> 4;
	val |= val >> 8;
	val |= val >> 16;
	val++;
	return val;
}

static int
pfs_write_paxos_sector(pfs_mount_t *mnt, int sector, void *buf)
{
	pfs_file_t *file = mnt->mnt_paxos_file;
	off_t offset = sector * mnt->mnt_sectsize;
	int rv;

	rv = pfs_file_pwrite(file, buf, mnt->mnt_sectsize, offset);
	if (rv < 0) {
		pfs_etrace("paxos write sector %d (offset %lld) failed rv=%d\n",
		    sector, (long long)offset, rv);
		if (rv == -ETIMEDOUT)
			rv = PFS_AIO_TIMEOUT;
		return rv;
	}
	if (rv != (int)mnt->mnt_sectsize) {
		pfs_etrace("paxos write sector %d (offset %lld) short write: "
		    "%d of %zu bytes\n",
		    sector, (long long)offset, rv, mnt->mnt_sectsize);
		return -EIO;
	}
	return 0;
}

static int
pfs_read_paxos_sectors(pfs_mount_t *mnt, int start_sector, int nsector, void *buf)
{
	pfs_file_t *file = mnt->mnt_paxos_file;
	off_t offset = start_sector * mnt->mnt_sectsize;
	int rv;

	size_t expected = nsector * mnt->mnt_sectsize;

	/* read IO doesn't have any time limited */
	rv = pfs_file_pread(file, buf, expected, offset);
	if (rv < 0) {
		pfs_etrace("paxos reads from offset %lld failed, rv=%d\n",
		    (long long)offset, rv);
		return rv;
	}
	if (rv != (int)expected) {
		pfs_etrace("paxos short read at offset %lld: %d of %zu bytes\n",
		    (long long)offset, rv, expected);
		return -EIO;
	}
	return rv;
}

static int
write_leader(pfs_mount_t *mnt, struct pfs_leader_record *lr)
{
	size_t sector_size = mnt->mnt_sectsize;
	struct pfs_leader_record *lr_end;
	uint32_t checksum = 0;
	int rv;

	rv = pfs_mem_memalign((void **)&lr_end, sector_size, sector_size,
	    M_PAXOS_SECTOR);
	if (rv)
		return -rv;
	memset(lr_end, 0, sector_size);

	leader_record_out(lr, lr_end);

	/*
	 * N.B. must compute checksum after the data has been byte swapped.
	 */
	checksum = leader_checksum(lr_end);
	lr->checksum = checksum;
	lr_end->checksum = cpu_to_le32(checksum);

	rv = pfs_write_paxos_sector(mnt, 0, lr_end);
	pfs_mem_free(lr_end, M_PAXOS_SECTOR);
	return rv;
}

int
read_leader(pfs_mount_t *mnt, struct pfs_leader_record *lr, uint32_t *checksum)
{
	size_t sector_size = mnt->mnt_sectsize;
	struct pfs_leader_record *lr_end;
	int rv;

	rv = pfs_mem_memalign((void **)&lr_end, sector_size, sector_size,
	    M_PAXOS_SECTOR);
	if (rv)
		return -rv;
	memset(lr_end, 0, sector_size);

	/* 0 = leader record is first sector */
	rv = pfs_read_paxos_sectors(mnt, 0, 1, lr_end);
	/* N.B. checksum is computed while the data is in ondisk format. */
	if (checksum)
		*checksum = leader_checksum(lr_end);
	leader_record_in(lr_end, lr);
	pfs_mem_free(lr_end, M_PAXOS_SECTOR);
	return rv;
}

static int
verify_leader(pfs_mount_t *mnt, struct pfs_leader_record *lr, uint32_t checksum)
{
	struct pfs_leader_record leader_rr;
	int result;

	if (lr->magic == PFS_LEADER_CLEAR)
		return PFS_LEADER_EMAGIC;

	if (lr->magic != PFS_LEADER_MAGIC) {
		pfs_etrace("verify_leader wrong magic %x", lr->magic);
		result = PFS_LEADER_MAGIC;
		goto fail;
	}

	if ((lr->version & 0xFFFF0000) != PFS_LEADER_VERSION_PRIMARY) {
		pfs_etrace("verify_leader wrong version %x",
		    lr->version);
		result = PFS_LEADER_EVERSION;
		goto fail;
	}

	if (lr->sector_size != mnt->mnt_sectsize) {
		pfs_etrace("verify_leader wrong sector size %d %u",
		    lr->sector_size, mnt->mnt_sectsize);
		result = PFS_LEADER_ESECTORSIZE;
		goto fail;
	}

	if (lr->num_hosts < mnt->mnt_host_id) {
		pfs_etrace("verify_leader num_hosts too small %llu %llu",
		    (unsigned long long)lr->num_hosts,
		    (unsigned long long)mnt->mnt_host_id);
		result = PFS_LEADER_ENUMHOSTS;
		goto fail;
	}

	if (lr->checksum != checksum) {
		pfs_etrace("verify_leader wrong checksum %x %x",
		    lr->checksum, checksum);
		result = PFS_LEADER_ECHECKSUM;
		goto fail;
	}

	return PFS_OK;

 fail:
	return result;
}

static int
_leader_read_one(pfs_mount_t *mnt, struct pfs_leader_record *leader_ret)
{
	struct pfs_leader_record leader;
	uint32_t checksum = 0;
	int rv;

	memset(&leader, 0, sizeof(struct pfs_leader_record));
	rv = read_leader(mnt, &leader, &checksum);
	if (rv < 0) {
		memset(leader_ret, 0, sizeof(pfs_leader_record));
		return rv;
	}
	rv = verify_leader(mnt, &leader, checksum);

	/* copy what we read even if verify finds a problem */
	memcpy(leader_ret, &leader, sizeof(struct pfs_leader_record));
	return rv;
}

int
pfs_leader_read(pfs_mount_t *mnt, pfs_leader_record_t *leader_ret)
{
	int rv;

	/* _leader_read_num works fine for the single disk case, but
	   we can cut out a bunch of stuff when we know there's one disk */

	rv = _leader_read_one(mnt, leader_ret);

	return rv;
}

int
pfs_leader_write(pfs_mount_t *mnt, pfs_leader_record_t *nl)
{

	int rv = write_leader(mnt, nl);
	if (rv < 0) {
		pfs_etrace("write_leader failed：%d\n", rv);
	}

	pfs_dbgtrace("log txid (%lld, %lld] offset (%llu, %llu] %lld\n",
	    (long long)nl->tail_txid,
	    (long long)nl->head_txid,
	    (unsigned long long)nl->tail_offset,
	    (unsigned long long)nl->head_offset,
	    (long long)nl->head_lsn);

	return PFS_OK;
}

/*
 * The caller must make sure that both num_hosts and max_hosts
 * are not negative. A negative value will cause an implicit
 * conversion which results in an undefined behavior.
 */
int
pfs_leader_init(pfs_mount_t *mnt, int num_hosts, int max_hosts, int write_clear,
    size_t logsize)
{
	char *iobuf = NULL;
	struct pfs_leader_record leader;
	struct pfs_leader_record leader_end;
	uint32_t checksum = 0;
	int iobuf_len;
	int sector_size;
	int align_size;
	int num_disks = 1;
	int rv, d, fd = -1;

	if (!num_hosts)
		num_hosts = DEFAULT_MAX_HOSTS;
	if (!max_hosts)
		max_hosts = DEFAULT_MAX_HOSTS;

	if (max_hosts > DEFAULT_MAX_HOSTS)
		return -E2BIG;

	if (num_hosts > DEFAULT_MAX_HOSTS)
		return -EINVAL;

	if (num_hosts > max_hosts)
		return -EINVAL;

	sector_size = mnt->mnt_sectsize;
	align_size = direct_align(sector_size);
	if (align_size < 0)
		return align_size;

	if (sector_size * (2 + max_hosts) > align_size)
		return -E2BIG;

	iobuf_len = align_size;
	rv = pfs_mem_memalign((void **)&iobuf, getpagesize(), iobuf_len,
	    M_PAXOS_SECTOR);
	if (rv)
		return -rv;
	memset(iobuf, 0, iobuf_len);

	memset(&leader, 0, sizeof(leader));
	if (write_clear) {
		leader.magic = PFS_LEADER_CLEAR;
	} else {
		leader.magic = PFS_LEADER_MAGIC;
	}

	leader.version = PFS_LEADER_VERSION_PRIMARY | PFS_LEADER_VERSION_SECONDARY;
	leader.sector_size = sector_size;
	leader.num_hosts = num_hosts;
	leader.max_hosts = max_hosts;
	leader.tail_txid = 0;
	leader.head_txid = 0;	/* intial null tx range is (0, 0] */
	leader.head_lsn = 0;
	leader.log_size = logsize;
	leader.checksum = 0; /* set after leader_record_out */
	leader_record_out(&leader, &leader_end);

	/*
	 * N.B. must compute checksum after the data has been byte swapped.
	 */
	checksum = leader_checksum(&leader_end);
	leader.checksum = checksum;
	leader_end.checksum = cpu_to_le32(checksum);
	memcpy(iobuf, &leader_end, sizeof(struct pfs_leader_record));

	PFS_ASSERT(mnt->mnt_paxos_file == NULL);
	fd = pfs_file_open_impl(mnt, PAXOS_FILE_MONO, 0, &mnt->mnt_paxos_file,
	    INNER_FILE_BTIME);
	if (fd < 0) {
		rv = fd;
		goto out;
	}

	rv = 0;
	for (num_disks = 1, d = 0; d < num_disks; d++) {
		rv |= pfs_file_pwrite(mnt->mnt_paxos_file, iobuf, iobuf_len, 0);
		if (rv < 0)
			goto out;
	}
	rv = 0;

out:
	if (fd >= 0) {
		fd = -1;
		pfs_file_close(mnt->mnt_paxos_file);
		mnt->mnt_paxos_file = NULL;
	}

	if (iobuf) {
		pfs_mem_free(iobuf, M_PAXOS_SECTOR);
		iobuf = NULL;
	}
	return (rv < 0) ? rv : 0;
}

int
pfs_leader_load(pfs_mount_t *mnt)
{
	struct pfs_leader_record lr;
	int error, fd;
	uint32_t checksum = 0;

	fd = pfs_file_open_impl(mnt, PAXOS_FILE_MONO, 0,
	    &mnt->mnt_paxos_file, INNER_FILE_BTIME);
	error = (fd < 0) ? fd : 0;
	if (error < 0)
		return error;

	error = read_leader(mnt, &lr, &checksum);
	if (error < 0)
		return error;
	error = verify_leader(mnt, &lr, checksum);
	if (error < 0)
		return error;
	mnt->mnt_num_hosts = lr.num_hosts;

	if (mnt->mnt_host_id > mnt->mnt_num_hosts)
		ERR_RETVAL(EINVAL);
	if ((mnt->mnt_flags & (PFS_TOOL|MNTFLG_PFSD)) != 0 && mnt->mnt_host_id == 0)
		mnt->mnt_host_id = mnt->mnt_num_hosts;
	PFS_ASSERT(mnt->mnt_host_id > 0);
	/* For pfsd, paxos_hostid_local_lock is moved up to SDK side */
	if (!pfs_ispfsd(mnt) && pfs_writable(mnt)) {
		fd = paxos_hostid_local_lock(mnt->mnt_lockspace_name,
		   mnt->mnt_host_id, __func__);
		if (fd < 0)
			return fd;
		mnt->mnt_hostid_fd = fd;
	}

	if (pfs_writable(mnt)) {
		uint32_t blocker_id = 0;
		int rv = pfs_rw_lease_check(mnt, &blocker_id);
		if (rv == -EBUSY)
			rv = pfs_rw_lease_wait_and_check(mnt, blocker_id);
		if (rv < 0)
			return rv;

		/* Read old record to get generation counter */
		pfs_host_record_t old_hr;
		if (pfs_host_record_read(mnt, mnt->mnt_host_id, &old_hr) == PFS_OK)
			mnt->mnt_host_generation = (uint64_t)old_hr.hr_generation + 1;
		else
			mnt->mnt_host_generation = 1;

		/*
		 * Disk Paxos two-phase protocol:
		 * prepare → verify → acquire → conflict-check.
		 * Ballot-based arbitration handles concurrent acquires.
		 * If verify_prepare finds a higher ballot, we fail
		 * immediately with -EBUSY.
		 */
		rv = pfs_rw_lease_prepare(mnt);
		if (rv < 0) {
			pfs_host_record_clear(mnt, mnt->mnt_host_id);
			return rv;
		}

		rv = pfs_rw_lease_verify_prepare(mnt);
		if (rv < 0) {
			pfs_host_record_clear(mnt, mnt->mnt_host_id);
			return rv;
		}

		rv = pfs_rw_lease_acquire(mnt);
		if (rv < 0)
			return rv;

		rv = pfs_rw_lease_check_conflict(mnt);
		if (rv == -EBUSY) {
			pfs_host_record_clear(mnt, mnt->mnt_host_id);
			return -EBUSY;
		}

		mnt->mnt_rw_lease_held = true;

		paxos_watchdog_open(mnt);
	} else {
		mnt->mnt_host_generation = 0;
	}

	mnt->mnt_log.log_leader = lr;
	return PFS_OK;
}

void
pfs_leader_unload(pfs_mount_t *mnt)
{
	if (mnt->mnt_rw_lease_held) {
		paxos_watchdog_close(mnt);  /* disarm first (clean shutdown) */
		pfs_rw_lease_release(mnt);
		mnt->mnt_rw_lease_held = false;
	}
	if (mnt->mnt_hostid_fd >= 0) {
		/* For pfsd, paxos_hostid_local_unlock is moved up to SDK side*/
		PFS_ASSERT(!pfs_ispfsd(mnt));
		paxos_hostid_local_unlock(mnt->mnt_hostid_fd);
		mnt->mnt_hostid_fd = -1;
	}
	if (mnt->mnt_paxos_file) {
		pfs_file_close(mnt->mnt_paxos_file);
		mnt->mnt_paxos_file = NULL;
	}
}

#define FLK_LEN	1024
/*
 * Host id is requried for disk paxos. If more than one instances
 * claim to the same host id, the result is unpredictable. We ensure
 * different instances use different host ids on one node and in this
 * way prevent havoc, since currently only one node is allowed to read
 * and write.
 */
int
paxos_hostid_local_lock(const char *pbdname, int hostid, const char* caller)
{
	char pathbuf[PFS_MAX_PATHLEN];
	struct flock flk;
	mode_t omask;
	ssize_t size;
	int err, fd;

	size = snprintf(pathbuf, sizeof(pathbuf),
	    "/var/run/pfs/%s-paxos-hostid", pbdname);
	if (size >= (ssize_t)sizeof(pathbuf))
		ERR_RETVAL(ENAMETOOLONG);

	omask = umask(0000);
	err = fd = open(pathbuf, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
	(void)umask(omask);
	if (err < 0) {
		pfs_etrace("cant open file %s, err=%d, errno=%d\n",
		    pathbuf, err, errno);
		ERR_RETVAL(EACCES);
	}

	/*
	 * Writer with host N will try to lock FLK_LEN*[N, N+1) region
	 * of access file. If the writer is a mkfs/growfs which's hostid
	 * is 0, then both l_start and l_len are zero, the whole file will
	 * be locked according to fcntl(2).
	 */
	memset(&flk, 0, sizeof(flk));
	flk.l_type = F_WRLCK;
	flk.l_whence = SEEK_SET;
	flk.l_start = hostid * FLK_LEN;
	flk.l_len = hostid > 0 ? FLK_LEN : 0;
	err = fcntl(fd, F_SETLK, &flk);
	if (err < 0) {
		pfs_etrace("cant lock file %s [%d, %d), err=%d, errno=%d\n",
		    pathbuf, flk.l_start, flk.l_start + flk.l_len, err,
		    errno);
		(void)close(fd);
		ERR_RETVAL(EACCES);
	}

	return fd;
}

void
paxos_hostid_local_unlock(int fd)
{
	/*
	 * locks are automatically released if fd is closed.
	 */
	if (fd < 0)
		return;
	(void)close(fd);
}

/* ========== Cross-host RW lease (sanlock-style) ========== */

int64_t
pfs_paxos_lease_duration(void)
{
	return paxos_lease_duration;
}

static uint32_t
host_record_checksum(const pfs_host_record_t *hr)
{
	return crc32c((uint32_t)~1, (const uint8_t *)hr, HOST_CHECKSUM_LEN);
}

static int
pfs_host_record_write(pfs_mount_t *mnt, const pfs_host_record_t *hr)
{
	size_t sector_size = mnt->mnt_sectsize;
	void *buf;
	int rv;

	PFS_ASSERT(sector_size >= sizeof(pfs_host_record_t));

	rv = pfs_mem_memalign(&buf, sector_size, sector_size, M_PAXOS_SECTOR);
	if (rv)
		return -rv;
	memset(buf, 0, sector_size);
	memcpy(buf, hr, sizeof(pfs_host_record_t));

	rv = pfs_write_paxos_sector(mnt, (int)hr->hr_host_id, buf);
	pfs_mem_free(buf, M_PAXOS_SECTOR);
	return rv;
}

/*
 * Write raw bytes to a host sector via the paxos I/O path (same path as
 * pfs_rw_lease_acquire / pfs_host_record_write).  Used by test helpers to
 * inject corrupt sector contents that pfs_host_record_read will later parse.
 *
 * src must be at most 512 bytes (sizeof pfs_host_record_t); asserts if
 * srclen > 512.  The rest of the sector_size buffer is zeroed.
 * Returns 0 on success, negative on error.
 */
int
pfs_write_raw_host_sector(pfs_mount_t *mnt, uint32_t host_id,
    const void *src, size_t srclen)
{
	size_t sector_size = mnt->mnt_sectsize;
	void *buf;
	int rv;

	rv = pfs_mem_memalign(&buf, sector_size, sector_size, M_PAXOS_SECTOR);
	if (rv)
		return -rv;
	memset(buf, 0, sector_size);
	PFS_ASSERT(srclen <= 512);
	memcpy(buf, src, srclen);

	rv = pfs_write_paxos_sector(mnt, (int)host_id, buf);
	pfs_mem_free(buf, M_PAXOS_SECTOR);
	return rv;
}

int
pfs_host_record_read(pfs_mount_t *mnt, uint32_t host_id,
    pfs_host_record_t *hr_ret)
{
	size_t sector_size = mnt->mnt_sectsize;
	void *buf;
	pfs_host_record_t *hr;
	uint32_t checksum;
	int rv;

	rv = pfs_mem_memalign(&buf, sector_size, sector_size, M_PAXOS_SECTOR);
	if (rv)
		return -rv;
	memset(buf, 0, sector_size);

	rv = pfs_read_paxos_sectors(mnt, (int)host_id, 1, buf);
	if (rv < 0) {
		pfs_mem_free(buf, M_PAXOS_SECTOR);
		return rv;
	}

	hr = (pfs_host_record_t *)buf;

	if (hr->hr_magic == 0) {
		if (hr_ret)
			memset(hr_ret, 0, sizeof(pfs_host_record_t));
		pfs_mem_free(buf, M_PAXOS_SECTOR);
		return 0; /* empty / cleared sector */
	}

	if (hr->hr_magic != PFS_HOST_MAGIC) {
		pfs_etrace("pfs_host_record_read: bad magic %#x for host %u\n",
		    hr->hr_magic, host_id);
		pfs_mem_free(buf, M_PAXOS_SECTOR);
		return PFS_HOST_EMAGIC;
	}

	checksum = host_record_checksum(hr);
	if (hr->hr_checksum != checksum) {
		pfs_etrace("pfs_host_record_read: bad checksum %#x vs %#x "
		    "for host %u\n", hr->hr_checksum, checksum, host_id);
		pfs_mem_free(buf, M_PAXOS_SECTOR);
		return PFS_HOST_ECHECKSUM;
	}

	if (hr_ret)
		*hr_ret = *hr;
	pfs_mem_free(buf, M_PAXOS_SECTOR);
	return PFS_OK;
}

/*
 * Public read-only check: returns PFS_OK if host_id's sector holds a valid
 * record, 0 if the sector is empty, PFS_HOST_EMAGIC / PFS_HOST_ECHECKSUM if
 * corrupt.  Intended for test helpers that need to verify sector state from
 * within the mount process (e.g. confirm self-heal after corruption injection).
 */
int
pfs_check_host_sector(pfs_mount_t *mnt, uint32_t host_id)
{
	return pfs_host_record_read(mnt, host_id, nullptr);
}

static int
pfs_host_record_clear(pfs_mount_t *mnt, uint32_t host_id)
{
	size_t sector_size = mnt->mnt_sectsize;
	void *buf;
	int rv;

	rv = pfs_mem_memalign(&buf, sector_size, sector_size, M_PAXOS_SECTOR);
	if (rv)
		return -rv;
	memset(buf, 0, sector_size);

	rv = pfs_write_paxos_sector(mnt, (int)host_id, buf);
	pfs_mem_free(buf, M_PAXOS_SECTOR);
	return rv;
}

/*
 * Pre-lease check: scan all host sectors for an existing live RW holder.
 * Called before the prepare phase to detect an active lease early,
 * avoiding the overhead of a full prepare→acquire cycle when another
 * host already holds the lease.
 *
 * Returns 0 if no live RW holder is found, -EBUSY if a host has an
 * RW record whose timestamp is within paxos_lease_duration.  Corrupt
 * or unreadable sectors are skipped (logged as warnings).
 *
 * Also called from pfs_rw_lease_wait_and_check after the blocking
 * host's lease has expired, to re-verify no other host acquired in
 * the meantime.
 */
static int
pfs_rw_lease_check(pfs_mount_t *mnt, uint32_t *blocker_ret)
{
	pfs_host_record_t hr;
	struct timespec now;
	uint32_t freshest_id = 0;
	uint64_t freshest_ts = 0;
	uint32_t i;
	int rv;

	if (blocker_ret)
		*blocker_ret = 0;

	clock_gettime(CLOCK_REALTIME, &now);

	pfs_itrace("rw_lease_check: host_id=%u scanning %u host slots\n",
	    mnt->mnt_host_id, mnt->mnt_num_hosts);

	for (i = 1; i <= mnt->mnt_num_hosts; i++) {
		if (i == mnt->mnt_host_id)
			continue;

		rv = pfs_host_record_read(mnt, i, &hr);
		if (rv < 0) {
			pfs_etrace("rw_lease_check: host %u sector unreadable "
			    "(rv=%d), skipping\n", i, rv);
			continue;
		}
		if (rv == 0) /* empty sector */
			continue;

		/* rv == PFS_OK: valid record */
		if (!(hr.hr_flags & PFS_HOST_FL_RW))
			continue;

		uint64_t age = (uint64_t)now.tv_sec - hr.hr_timestamp;
		if (age < (uint64_t)paxos_lease_duration) {
			pfs_etrace("rw_lease_check: BUSY - host %u holds live "
			    "RW lease (gen=%u, ts=%llu, age=%llus, dur=%llds)\n",
			    hr.hr_host_id, hr.hr_generation,
			    (unsigned long long)hr.hr_timestamp,
			    (unsigned long long)age,
			    (long long)paxos_lease_duration);
			if (hr.hr_timestamp > freshest_ts ||
			    freshest_id == 0) {
				freshest_id = hr.hr_host_id;
				freshest_ts = hr.hr_timestamp;
			}
		} else {
			pfs_itrace("rw_lease_check: host %u has stale RW "
			    "lease (gen=%u, ts=%llu, age=%llus >= "
			    "dur=%llds), ignoring\n",
			    hr.hr_host_id, hr.hr_generation,
			    (unsigned long long)hr.hr_timestamp,
			    (unsigned long long)age,
			    (long long)paxos_lease_duration);
		}
	}

	if (freshest_id != 0) {
		if (blocker_ret)
			*blocker_ret = freshest_id;
		return -EBUSY;
	}

	pfs_itrace("rw_lease_check: no live RW holders found, safe to mount\n");
	return 0;
}

/*
 * Disk Paxos Phase 1 "prepare": write our sector with FL_PREPARE and mbal
 * set to our ballot.  This announces our intent to acquire the lease.
 *
 * Returns 0 on success, negative on I/O error.
 */
int
pfs_rw_lease_prepare(pfs_mount_t *mnt)
{
	pfs_host_record_t hr;
	struct timespec now;
	uint64_t ballot;
	int rv;

	ballot = pfs_ballot_generate(mnt);
	mnt->mnt_current_ballot = ballot;

	clock_gettime(CLOCK_REALTIME, &now);

	memset(&hr, 0, sizeof(hr));
	hr.hr_magic      = PFS_HOST_MAGIC;
	hr.hr_flags      = PFS_HOST_FL_PREPARE;
	hr.hr_host_id    = mnt->mnt_host_id;
	hr.hr_generation = (uint32_t)mnt->mnt_host_generation;
	hr.hr_timestamp  = (uint64_t)now.tv_sec;
	hr.hr_mbal       = ballot;
	hr.hr_bal        = 0;
	hr.hr_checksum   = host_record_checksum(&hr);

	rv = pfs_host_record_write(mnt, &hr);
	if (rv < 0) {
		pfs_etrace("rw_lease_prepare: write failed host_id=%u "
		    "ballot=%llu rv=%d\n",
		    mnt->mnt_host_id, (unsigned long long)ballot, rv);
	} else {
		pfs_itrace("rw_lease_prepare: wrote prepare host_id=%u "
		    "gen=%u ballot=%llu\n",
		    mnt->mnt_host_id, hr.hr_generation,
		    (unsigned long long)ballot);
	}
	return rv;
}

/*
 * Disk Paxos Phase 1 "verify prepare": re-read all other host sectors.
 * If any fresh record has mbal > our ballot, our prepare is preempted.
 *
 * Only records with a fresh timestamp (age < lease_duration) are considered.
 * Stale records from expired leases or crashed hosts are ignored — their
 * ballots are no longer relevant.
 *
 * Returns:
 *   0      — no higher ballot found; safe to proceed to acquire
 *  -EBUSY  — a higher ballot was found; another host is competing
 */
int
pfs_rw_lease_verify_prepare(pfs_mount_t *mnt)
{
	pfs_host_record_t hr;
	struct timespec now;
	uint32_t i;
	int rv;

	clock_gettime(CLOCK_REALTIME, &now);

	for (i = 1; i <= mnt->mnt_num_hosts; i++) {
		if (i == mnt->mnt_host_id)
			continue;

		rv = pfs_host_record_read(mnt, i, &hr);
		if (rv != PFS_OK)
			continue;

		uint64_t age = (uint64_t)now.tv_sec - hr.hr_timestamp;
		if (age >= (uint64_t)paxos_lease_duration)
			continue;  /* stale — ballot no longer relevant */

		if (hr.hr_mbal > mnt->mnt_current_ballot) {
			pfs_itrace("rw_lease_verify_prepare: host %u has "
			    "higher mbal %llu > our %llu (age=%llus), "
			    "preempted\n",
			    hr.hr_host_id,
			    (unsigned long long)hr.hr_mbal,
			    (unsigned long long)mnt->mnt_current_ballot,
			    (unsigned long long)age);
			return -EBUSY;
		}

		if (hr.hr_mbal == mnt->mnt_current_ballot) {
			pfs_etrace("rw_lease_verify_prepare: host %u has "
			    "SAME mbal %llu (should be impossible)\n",
			    hr.hr_host_id,
			    (unsigned long long)hr.hr_mbal);
			return -EBUSY;
		}
	}

	pfs_itrace("rw_lease_verify_prepare: no higher ballot found, "
	    "our ballot %llu is highest\n",
	    (unsigned long long)mnt->mnt_current_ballot);
	return 0;
}

/*
 * Post-acquire conflict detection (ballot-based).
 *
 * Called after pfs_rw_lease_acquire() has written our host record with
 * FL_RW and our ballot.  Re-reads every sector to detect races where two
 * hosts both passed prepare+verify before either had written acquire.
 *
 * Tie-break rule: highest ballot wins.  If another host has a fresh record
 * (FL_RW or FL_PREPARE) with a higher ballot, we are the loser.
 *
 * Returns:
 *   0      — we have the highest ballot; we hold the lease
 *  -EBUSY  — a higher-ballot host acquired concurrently; caller must clear
 *             our sector and abort the mount
 */
static int
pfs_rw_lease_check_conflict(pfs_mount_t *mnt)
{
	pfs_host_record_t hr;
	struct timespec now;
	uint32_t i;
	int rv;

	clock_gettime(CLOCK_REALTIME, &now);

	for (i = 1; i <= mnt->mnt_num_hosts; i++) {
		if (i == mnt->mnt_host_id)
			continue;

		rv = pfs_host_record_read(mnt, i, &hr);
		if (rv != PFS_OK)
			continue;
		if (!(hr.hr_flags & (PFS_HOST_FL_RW | PFS_HOST_FL_PREPARE)))
			continue;

		uint64_t age = (uint64_t)now.tv_sec - hr.hr_timestamp;
		if (age >= (uint64_t)paxos_lease_duration)
			continue;  /* stale — not a concurrent acquire */

		/*
		 * Ballot-based tie-break: compare the relevant ballot field.
		 * For FL_RW records, hr_bal is the acquired ballot.
		 * For FL_PREPARE records, hr_mbal is the promised ballot.
		 */
		uint64_t other_ballot = (hr.hr_flags & PFS_HOST_FL_RW) ?
		    hr.hr_bal : hr.hr_mbal;

		if (other_ballot > mnt->mnt_current_ballot) {
			pfs_itrace("rw_lease_check_conflict: host %u has "
			    "higher ballot %llu > our %llu — yielding\n",
			    hr.hr_host_id,
			    (unsigned long long)other_ballot,
			    (unsigned long long)mnt->mnt_current_ballot);
			return -EBUSY;
		}
		pfs_itrace("rw_lease_check_conflict: host %u has "
		    "ballot %llu <= our %llu; we win tie-break\n",
		    hr.hr_host_id,
		    (unsigned long long)other_ballot,
		    (unsigned long long)mnt->mnt_current_ballot);
	}

	return 0;
}

/*
 * Called when pfs_rw_lease_check() returned -EBUSY.  Polls the blocking
 * host's sector every second watching for three outcomes:
 *
 *   1. Timestamp advances  →  holder is alive and renewing; return -EBUSY
 *      immediately (no need to wait the full lease window).
 *
 *   2. Timestamp stays the same until initial_ts + paxos_lease_duration
 *      has passed  →  blocker has died; full re-check, return 0.
 *      Since pfs_rw_lease_check selects the blocker with the freshest
 *      timestamp, once this record expires all other simultaneous crash
 *      records (with older timestamps) have also expired.
 *
 *   3. Sector is cleared   →  holder unmounted cleanly; re-check, return 0.
 *
 * At most one wait cycle is performed, bounded by paxos_lease_duration.
 */
static int
pfs_rw_lease_wait_and_check(pfs_mount_t *mnt, uint32_t blocker_id)
{
	pfs_host_record_t hr;
	struct timespec now;
	uint32_t blocker_gen;
	uint64_t initial_ts;
	int rv;

	/*
	 * blocker_id is the host with the freshest live RW timestamp,
	 * identified by pfs_rw_lease_check.  Read its record to get
	 * initial_ts for the expiry and timestamp-advance checks.
	 */
	rv = pfs_host_record_read(mnt, blocker_id, &hr);
	if (rv != PFS_OK || !(hr.hr_flags & PFS_HOST_FL_RW)) {
		/* Blocker released or became unreadable since check. */
		pfs_itrace("rw_lease_wait: blocker %u gone before polling "
		    "started, re-checking\n", blocker_id);
		return pfs_rw_lease_check(mnt, nullptr);
	}
	blocker_gen = hr.hr_generation;
	initial_ts  = hr.hr_timestamp;

	pfs_itrace("rw_lease_wait: host %u (gen=%u ts=%llu) "
	    "holds live lease, polling every 1s (max %llds)\n",
	    blocker_id, blocker_gen, (unsigned long long)initial_ts,
	    (long long)paxos_lease_duration);

	for (;;) {
		sleep(1);
		clock_gettime(CLOCK_REALTIME, &now);

		rv = pfs_host_record_read(mnt, blocker_id, &hr);
		if (rv == 0) {
			/* Sector cleared — clean release by the holder. */
			pfs_itrace("rw_lease_wait: host %u released lease "
			    "cleanly\n", blocker_id);
			return pfs_rw_lease_check(mnt, nullptr);
		}
		if (rv < 0) {
			/* Unreadable — treat conservatively as expired. */
			pfs_etrace("rw_lease_wait: host %u sector unreadable "
			    "(rv=%d), treating as expired\n", blocker_id, rv);
			return pfs_rw_lease_check(mnt, nullptr);
		}

		/* rv == PFS_OK: valid record still present. */
		if (hr.hr_timestamp != initial_ts) {
			/* Timestamp advanced — holder is alive and renewing. */
			pfs_etrace("rw_lease_wait: host %u renewed lease "
			    "(ts %llu→%llu), holder is alive\n",
			    blocker_id,
			    (unsigned long long)initial_ts,
			    (unsigned long long)hr.hr_timestamp);
			return -EBUSY;
		}

		/*
		 * The blocker has the freshest timestamp among all live
		 * holders (selected by pfs_rw_lease_check).  Once its
		 * record expires, all other simultaneous crash records
		 * (with older timestamps) have also expired.
		 */
		uint64_t age = (uint64_t)now.tv_sec - initial_ts;
		if (age >= (uint64_t)paxos_lease_duration) {
			pfs_itrace("rw_lease_wait: host %u lease expired "
			    "(age=%llus), re-checking all hosts\n",
			    blocker_id, (unsigned long long)age);
			return pfs_rw_lease_check(mnt, nullptr);
		}

		pfs_itrace("rw_lease_wait: host %u lease age=%llus/%llds, "
		    "timestamp unchanged, continuing\n",
		    blocker_id, (unsigned long long)age,
		    (long long)paxos_lease_duration);
	}
}

int
pfs_rw_lease_acquire(pfs_mount_t *mnt)
{
	pfs_host_record_t hr;
	struct timespec now;
	int rv;

	clock_gettime(CLOCK_REALTIME, &now);

	memset(&hr, 0, sizeof(hr));
	hr.hr_magic      = PFS_HOST_MAGIC;
	hr.hr_flags      = PFS_HOST_FL_RW;
	hr.hr_host_id    = mnt->mnt_host_id;
	hr.hr_generation = (uint32_t)mnt->mnt_host_generation;
	hr.hr_timestamp  = (uint64_t)now.tv_sec;
	hr.hr_mbal       = mnt->mnt_current_ballot;
	hr.hr_bal        = mnt->mnt_current_ballot;
	hr.hr_checksum   = host_record_checksum(&hr);

	rv = pfs_host_record_write(mnt, &hr);
	if (rv < 0) {
		pfs_etrace("rw_lease_acquire: failed to write lease record "
		    "host_id=%u gen=%u ballot=%llu rv=%d\n",
		    mnt->mnt_host_id, hr.hr_generation,
		    (unsigned long long)mnt->mnt_current_ballot, rv);
	} else {
		pfs_itrace("rw_lease_acquire: acquired RW lease "
		    "host_id=%u gen=%u ballot=%llu ts=%llu\n",
		    mnt->mnt_host_id, hr.hr_generation,
		    (unsigned long long)mnt->mnt_current_ballot,
		    (unsigned long long)hr.hr_timestamp);
	}
	return rv;
}

/*
 * Write a fresh valid RW lease record for a host other than the current
 * mount owner.  Used by test helpers to plant simultaneous "crashed-host"
 * records without needing that host to actually mount.
 *
 * The caller holds its own RW lease (so mnt_paxos_file is open and writable).
 * This function writes to sector foreign_hostid, leaving mnt->mnt_host_id
 * and all other sectors untouched.
 */
int
pfs_rw_lease_write_foreign(pfs_mount_t *mnt, uint32_t foreign_hostid)
{
	pfs_host_record_t hr;
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);

	/*
	 * Foreign records simulate a crashed host.  Use generation=1 and
	 * compute a plausible ballot so the record looks like a real acquire.
	 */
	uint32_t gen = 1;
	uint64_t ballot = (uint64_t)gen * mnt->mnt_num_hosts + foreign_hostid;

	memset(&hr, 0, sizeof(hr));
	hr.hr_magic      = PFS_HOST_MAGIC;
	hr.hr_flags      = PFS_HOST_FL_RW;
	hr.hr_host_id    = foreign_hostid;
	hr.hr_generation = gen;
	hr.hr_timestamp  = (uint64_t)now.tv_sec;
	hr.hr_mbal       = ballot;
	hr.hr_bal        = ballot;
	hr.hr_checksum   = host_record_checksum(&hr);

	return pfs_host_record_write(mnt, &hr);
}

/*
 * Write a FL_PREPARE record for a foreign host.  Used by test helpers to
 * plant a "stale prepare" from a host that crashed after prepare but before
 * acquire.  The generation parameter controls the ballot so tests can create
 * prepares with specific ballot ordering.
 */
int
pfs_rw_lease_write_foreign_prepare(pfs_mount_t *mnt,
    uint32_t foreign_hostid, uint32_t generation)
{
	pfs_host_record_t hr;
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);

	uint64_t ballot = (uint64_t)generation * mnt->mnt_num_hosts +
	    foreign_hostid;

	memset(&hr, 0, sizeof(hr));
	hr.hr_magic      = PFS_HOST_MAGIC;
	hr.hr_flags      = PFS_HOST_FL_PREPARE;
	hr.hr_host_id    = foreign_hostid;
	hr.hr_generation = generation;
	hr.hr_timestamp  = (uint64_t)now.tv_sec;
	hr.hr_mbal       = ballot;
	hr.hr_bal        = 0;
	hr.hr_checksum   = host_record_checksum(&hr);

	return pfs_host_record_write(mnt, &hr);
}

int
pfs_rw_lease_renew(pfs_mount_t *mnt)
{
	pfs_host_record_t hr;
	struct timespec now;
	int rv;

	clock_gettime(CLOCK_REALTIME, &now);

	memset(&hr, 0, sizeof(hr));
	hr.hr_magic      = PFS_HOST_MAGIC;
	hr.hr_flags      = PFS_HOST_FL_RW;
	hr.hr_host_id    = mnt->mnt_host_id;
	hr.hr_generation = (uint32_t)mnt->mnt_host_generation;
	hr.hr_timestamp  = (uint64_t)now.tv_sec;
	hr.hr_mbal       = mnt->mnt_current_ballot;
	hr.hr_bal        = mnt->mnt_current_ballot;
	hr.hr_checksum   = host_record_checksum(&hr);

	rv = pfs_host_record_write(mnt, &hr);
	if (rv < 0) {
		pfs_etrace("rw_lease_renew: failed to write renewal "
		    "host_id=%u gen=%u rv=%d\n",
		    mnt->mnt_host_id, hr.hr_generation, rv);
	} else {
		pfs_dbgtrace("rw_lease_renew: renewed host_id=%u gen=%u "
		    "ts=%llu\n",
		    mnt->mnt_host_id, hr.hr_generation,
		    (unsigned long long)hr.hr_timestamp);
	}
	return rv;
}

void
pfs_rw_lease_release(pfs_mount_t *mnt)
{
	pfs_itrace("rw_lease_release: releasing RW lease host_id=%u gen=%llu\n",
	    mnt->mnt_host_id, (unsigned long long)mnt->mnt_host_generation);
	int rv = pfs_host_record_clear(mnt, mnt->mnt_host_id);
	if (rv < 0)
		pfs_etrace("rw_lease_release: failed to clear host_id=%u "
		    "record rv=%d\n", mnt->mnt_host_id, rv);
	else
		pfs_itrace("rw_lease_release: RW lease released host_id=%u\n",
		    mnt->mnt_host_id);
}

/* ========== Lease watchdog: timer-kill (mandatory) + /dev/watchdog (optional) ========== */

/*
 * Timer-kill watchdog: arms a POSIX per-process timer on lease acquire.
 * If the log thread stops petting it (hang, deadlock, runaway), the timer
 * fires SIGUSR2 after paxos_lease_duration seconds.  The signal handler
 * prints a diagnostic (async-signal-safe) and raises SIGKILL to guarantee
 * termination even if the process is otherwise wedged.
 *
 * /dev/watchdog (optional, paxos_watchdog_enable=1): arms the kernel
 * hardware/software watchdog as a last resort.  If the entire kernel is
 * stuck in uninterruptible sleep (D state), SIGKILL won't be delivered
 * and only a machine reboot via /dev/watchdog can fence the host.
 */

static void
paxos_kill_handler(int sig)
{
	/*
	 * Async-signal-safe only: write() and raise() are safe.
	 * Do not use pfs_etrace, printf, malloc, etc.
	 */
	const char msg[] = "fatal: RW lease watchdog expired — "
	    "lease renewal stopped, killing process to prevent "
	    "split-brain (SIGKILL)\n";
	(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
	raise(SIGKILL);
	_exit(134);  /* fallback if SIGKILL somehow doesn't terminate */
}

void
paxos_watchdog_open(pfs_mount_t *mnt)
{
	/* Timer-kill watchdog (mandatory) */
	struct sigaction sa = {};
	sa.sa_handler = paxos_kill_handler;
	sa.sa_flags = SA_RESETHAND;  /* one-shot: restore default after firing */
	sigaction(SIGUSR2, &sa, NULL);

	struct sigevent sev = {};
	sev.sigev_notify = SIGEV_SIGNAL;
	sev.sigev_signo = SIGUSR2;
	if (timer_create(CLOCK_MONOTONIC, &sev, &mnt->mnt_kill_timer) == 0) {
		struct itimerspec its = {};
		its.it_value.tv_sec = paxos_lease_duration;
		timer_settime(mnt->mnt_kill_timer, 0, &its, NULL);
		mnt->mnt_kill_timer_armed = true;
		pfs_itrace("watchdog_open: timer-kill armed "
		    "(SIGUSR2 in %llds) pbd=%s\n",
		    (long long)paxos_lease_duration, mnt->mnt_pbdname);
	} else {
		pfs_etrace("watchdog_open: timer_create failed errno=%d, "
		    "falling back to log-thread self-fence only\n", errno);
	}

	/* /dev/watchdog (optional) */
	if (!paxos_watchdog_enable)
		return;
	mnt->mnt_wdog_fd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
	if (mnt->mnt_wdog_fd < 0)
		pfs_etrace("watchdog_open: cannot open /dev/watchdog errno=%d\n",
		    errno);
	else
		pfs_itrace("watchdog_open: /dev/watchdog armed (fd=%d)\n",
		    mnt->mnt_wdog_fd);
}

void
paxos_watchdog_pet(pfs_mount_t *mnt)
{
	/* Reset timer-kill countdown */
	if (mnt->mnt_kill_timer_armed) {
		struct itimerspec its = {};
		its.it_value.tv_sec = paxos_lease_duration;
		timer_settime(mnt->mnt_kill_timer, 0, &its, NULL);
	}

	/* Pet /dev/watchdog */
	if (mnt->mnt_wdog_fd >= 0)
		(void)write(mnt->mnt_wdog_fd, "1", 1);
}

void
paxos_watchdog_close(pfs_mount_t *mnt)
{
	/* Disarm timer-kill */
	if (mnt->mnt_kill_timer_armed) {
		struct itimerspec its = {};  /* zero = disarm */
		timer_settime(mnt->mnt_kill_timer, 0, &its, NULL);
		timer_delete(mnt->mnt_kill_timer);
		mnt->mnt_kill_timer_armed = false;
		pfs_itrace("watchdog_close: timer-kill disarmed\n");
	}

	/* Disarm /dev/watchdog */
	if (mnt->mnt_wdog_fd >= 0) {
		/* Write magic 'V' to disarm the watchdog on clean shutdown */
		(void)write(mnt->mnt_wdog_fd, "V", 1);
		(void)close(mnt->mnt_wdog_fd);
		mnt->mnt_wdog_fd = -1;
		pfs_itrace("watchdog_close: /dev/watchdog disarmed\n");
	}
}
