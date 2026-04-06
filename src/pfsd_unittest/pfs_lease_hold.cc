/*
 * pfs_lease_hold — test helper for cross-host RW lease tests.
 *
 * Usage:
 *   pfs_lease_hold <cluster> <pbdname> <hostid> [rw|ro|promote]
 *   pfs_lease_hold <cluster> <pbdname> <rw_hostid> demote <ro_hostid>
 *   pfs_lease_hold <cluster> <pbdname> <writer_hostid> corrupt-sector <target_hostid> <badmagic|badchecksum>
 *   pfs_lease_hold <cluster> <pbdname> <hostid> self-corrupt-renew
 *   pfs_lease_hold <cluster> <pbdname> <writer_hostid> write-two-records <target1> <target2>
 *   pfs_lease_hold <cluster> <pbdname> <writer_hostid> write-prepare-record <target_hostid> <generation>
 *   pfs_lease_hold <cluster> <pbdname> <hostid> watchdog-stall
 *   pfs_lease_hold <cluster> <pbdname> <hostA> refcount-multi <hostB>
 *   pfs_lease_hold <cluster> <pbdname> <hostA> refcount-ro-only <hostB>
 *   pfs_lease_hold <cluster> <pbdname> <hostA> refcount-reject
 *   pfs_lease_hold <cluster> <pbdname> <hostA> refcount-three-client <hostB> <hostC>
 *
 * Modes:
 *   rw      — mount RW, print "MOUNTED", block until SIGTERM, unmount.
 *   ro      — mount RO, print "MOUNTED", block until SIGTERM, unmount.
 *   promote — mount RO, print "MOUNTED_RO", block until SIGUSR1; then call
 *             pfs_remount() to promote to RW in-place.  On success prints
 *             "MOUNTED_RW"; on failure prints "REMOUNT_FAILED:<errno>".
 *             Then blocks until SIGTERM and unmounts.
 *   self-corrupt-renew — mount RW as hostid, print "MOUNTED", wait for
 *             SIGUSR1; on SIGUSR1 corrupt own paxos sector (bad checksum),
 *             print "CORRUPTED", sleep 3s for the log thread renewal to
 *             self-heal, then print "SELF_HEALED" or "SELF_HEAL_FAILED:<rv>".
 *             Blocks until SIGTERM and unmounts.  Used by Group J tests.
 *   corrupt-sector — mount RW as writer_hostid, then write a deliberately
 *             corrupt pfs_host_record into target_hostid's paxos sector via
 *             pfs_write_raw_host_sector (same I/O path as pfs_rw_lease_acquire).
 *             Prints "CORRUPT_WRITTEN" on success and exits.  Used by Group I
 *             tests to verify that pfs_host_record_read returns an error for
 *             corrupt sectors and that pfs_rw_lease_check skips them instead
 *             of treating them as live holders.
 *   demote  — simulates a pfsd server with one RW client (rw_hostid) and one
 *             RO client (ro_hostid, passed as argv[5]).  Mounts via
 *             pfs_mount_acquire so both clients are tracked in the pfsd
 *             refcount table.  Prints "MOUNTED_RW", then blocks until SIGUSR1;
 *             on SIGUSR1 releases the RW client via pfs_mount_release(), which
 *             triggers pfs_remount_ro() internally (demotes to RO).  On
 *             success prints "DEMOTED_RO"; on process exit (pfs_remount_ro
 *             calls exit(EIO) on failure) the test detects process death.
 *             Then blocks until SIGTERM, releases the RO client and unmounts.
 *   write-two-records — mount RW as writer_hostid (no other live holders),
 *             write fresh valid pfs_host_record_t entries for target1 and
 *             target2 via pfs_rw_lease_write_foreign, then unmount cleanly
 *             (zeroes writer_hostid sector; target sectors remain).
 *             Prints "WRITTEN" on success.  Used by Group L tests to plant
 *             simultaneous stale blocker records for multi-blocker scenarios.
 *   write-prepare-record — mount RW as writer_hostid, write a FL_PREPARE
 *             record for target_hostid with the given generation (determines
 *             ballot), then unmount cleanly.  Prints "PREPARE_WRITTEN" on
 *             success.  Used by Group M tests to plant stale prepare records.
 *   watchdog-stall — mount RW, print "MOUNTED", wait for SIGUSR1; then
 *             call pfs_log_suspend() to stop the log thread's renewals
 *             WITHOUT calling pfs_leader_unload() (so the timer-kill
 *             watchdog stays armed).  Prints "STALLED".  After
 *             paxos_lease_duration seconds without petting the watchdog
 *             fires SIGUSR2 → SIGKILL, killing this process.  Used by
 *             Group N watchdog tests.
 *   refcount-multi — self-checking mode: acquires RW as hostA, RO as hostB,
 *             releases A (triggers demote), verifies host_id transfer to B,
 *             releases B (triggers umount).  Checks refcounts at each step.
 *             Prints PHASE_N_OK/FAIL per phase, then ALL_PHASES_OK or
 *             SOME_PHASES_FAILED.
 *   refcount-ro-only — like refcount-multi but both clients are RO.
 *   refcount-reject — tests pfs_host_incref rejection cases: repeat RW
 *             same host, RO on existing RW same host, RW on existing RO
 *             same host.
 *   refcount-three-client — three clients (A=RW, B=RO, C=RO) with chained
 *             release/demote/umount verifying host_id transfer at each step.
 *
 * Exit codes:
 *   0  — clean lifecycle
 *   1  — bad usage
 *   2  — pfs_mount() failed
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <time.h>

#include "pfs_api.h"
#include "pfs_mount.h"
#include "pfs_paxos.h"

static volatile sig_atomic_t g_stop    = 0;
static volatile sig_atomic_t g_promote = 0;

static void sighandler(int)  { g_stop    = 1; }
static void sigusr1handler(int) { g_promote = 1; }

#define RC_CHECK(phase, cond, fmt, ...) do { \
	if (!(cond)) { \
		printf("PHASE%d_FAIL: " fmt "\n", phase, ##__VA_ARGS__); \
		fflush(stdout); fails++; \
	} else { \
		printf("PHASE%d_OK\n", phase); \
		fflush(stdout); \
	} \
} while(0)

/* ------------------------------------------------------------------ */

static int
mode_rw_ro(const char *cluster, const char *pbdname, int hostid,
    const char *mode)
{
	int flags = (strcmp(mode, "ro") == 0) ? PFS_RD : PFS_RDWR;
	int r = pfs_mount(cluster, pbdname, hostid, flags);
	if (r != 0) {
		fprintf(stderr, "pfs_mount failed: %d\n", r);
		return 2;
	}

	printf("MOUNTED\n");
	fflush(stdout);

	while (!g_stop)
		sleep(1);

	pfs_umount(pbdname);
	return 0;
}

/* ------------------------------------------------------------------ */

static int
mode_promote(const char *cluster, const char *pbdname, int hostid)
{
	/*
	 * pfs_remount() is the pfsd-layer promotion API: it requires
	 * the per-host refcount in pfsd_mount_shared_infos to be
	 * initialised, which only pfs_mount_acquire() does.
	 * Using pfs_mount() directly would leave ms_ref_count == 0
	 * and cause pfs_remount() to fail with EINVAL.
	 *
	 * Phase 1: mount RO via the pfsd acquire path.
	 */
	int r = pfs_mount_acquire(cluster, pbdname, hostid, PFS_RD);
	if (r != 0) {
		fprintf(stderr, "pfs_mount_acquire(RO) failed: %d\n", r);
		return 2;
	}
	printf("MOUNTED_RO\n");
	fflush(stdout);

	/* Wait for SIGUSR1 (promote trigger) or SIGTERM (abort) */
	while (!g_stop && !g_promote)
		sleep(1);

	if (!g_stop) {
		/* Phase 2: promote RO→RW via pfs_remount */
		r = pfs_remount(cluster, pbdname, hostid, PFS_RDWR);
		if (r == 0) {
			printf("MOUNTED_RW\n");
			fflush(stdout);
		} else {
			printf("REMOUNT_FAILED:%d\n", errno);
			fflush(stdout);

			/*
			 * Verify the RO mount is still usable after
			 * the failed promotion.  Stat .pfs-paxos to
			 * force a namecache lookup (the root "/" is
			 * resolved without one).  Before the
			 * pfs_notify_inited fix this would crash with
			 * PFS_ASSERT(pfs_meta_islocked(mnt)) in
			 * pfs_inode_dir_find.
			 */
			char rootpath[PFS_MAX_PBDLEN + 32];
			snprintf(rootpath, sizeof(rootpath),
			    "/%s/.pfs-paxos", pbdname);
			struct stat st;
			if (pfs_stat(rootpath, &st) == 0) {
				printf("MOUNT_USABLE\n");
			} else {
				printf("MOUNT_UNUSABLE:%d\n", errno);
			}

			/*
			 * Verify refcounts are back to the pre-promote
			 * state: the host should still be RO (is_rwmnt
			 * false), wrref should be 0, and ref_count
			 * should be 1 (this single RO client).
			 */
			if (pfsd_get_mnt_ref_count() == 1 &&
			    pfsd_get_mnt_wrref_count() == 0 &&
			    pfsd_get_host_ref_count(hostid) == 1 &&
			    pfsd_get_host_is_rwmnt(hostid) == false) {
				printf("REFCOUNT_OK\n");
			} else {
				printf("REFCOUNT_BAD:ref=%d,wrref=%d,"
				    "host_ref=%d,host_rw=%d\n",
				    pfsd_get_mnt_ref_count(),
				    pfsd_get_mnt_wrref_count(),
				    pfsd_get_host_ref_count(hostid),
				    pfsd_get_host_is_rwmnt(hostid));
			}
			fflush(stdout);
		}

		/* Wait for SIGTERM regardless of promote outcome */
		while (!g_stop)
			sleep(1);
	}

	pfs_mount_release(pbdname, hostid);
	return 0;
}

/* ------------------------------------------------------------------ */

static int
mode_demote(const char *cluster, const char *pbdname, int hostid,
    int ro_hostid)
{
	/*
	 * Simulate a pfsd server demoting from RW to RO.
	 *
	 * pfs_mount_release() triggers pfs_remount_ro() when:
	 *   - the releasing host_id is the current RW mount's host_id, AND
	 *   - pfsd_mnt_wrref_count drops to 0, AND
	 *   - pfsd_mnt_ref_count > 0 (RO clients still registered).
	 *
	 * We register both clients via pfs_mount_acquire() so the pfsd
	 * refcount table is properly initialised.
	 */

	/* Phase 1: acquire RW mount (creates the underlying pfs_mount) */
	int r = pfs_mount_acquire(cluster, pbdname, hostid, PFS_RDWR);
	if (r != 0) {
		fprintf(stderr,
		    "pfs_mount_acquire(RW, hostid=%d) failed: %d\n",
		    hostid, r);
		return 2;
	}

	/*
	 * Phase 2: register the RO client on the already-open mount.
	 * pfs_mount_acquire() with PFS_RD on an existing mount just
	 * calls pfs_host_incref() — no re-mount, no disk I/O.
	 */
	r = pfs_mount_acquire(cluster, pbdname, ro_hostid, PFS_RD);
	if (r != 0) {
		fprintf(stderr,
		    "pfs_mount_acquire(RO, hostid=%d) failed: %d\n",
		    ro_hostid, r);
		pfs_mount_release(pbdname, hostid);
		return 2;
	}

	printf("MOUNTED_RW\n");
	fflush(stdout);

	/* Wait for SIGUSR1 (demote trigger) or SIGTERM (abort) */
	while (!g_stop && !g_promote)
		sleep(1);

	if (!g_stop) {
		/*
		 * Phase 3: release the RW client.
		 * Since pfsd_mnt_wrref_count becomes 0 and the RO
		 * client is still registered, pfs_mount_release()
		 * internally calls pfs_remount_ro(mnt, ro_hostid).
		 * If pfs_remount_ro() fails it calls exit(EIO), so
		 * returning 0 here means demotion succeeded.
		 */
		r = pfs_mount_release(pbdname, hostid);
		if (r == 0) {
			printf("DEMOTED_RO\n");
			fflush(stdout);
		} else {
			printf("DEMOTE_FAILED:%d\n", errno);
			fflush(stdout);
		}

		/* Wait for SIGTERM */
		while (!g_stop)
			sleep(1);
	} else {
		/* Aborted before demote: release RW client first */
		pfs_mount_release(pbdname, hostid);
	}

	/* Phase 4: release the RO client → triggers umount */
	pfs_mount_release(pbdname, ro_hostid);
	return 0;
}

/* ------------------------------------------------------------------ */

static int
mode_self_corrupt_renew(const char *cluster, const char *pbdname, int hostid)
{
	/*
	 * Test helper for Group J: pfs_rw_lease_renew self-heal.
	 *
	 * Mounts RW as hostid (which writes sector hostid via
	 * pfs_rw_lease_acquire).  On SIGUSR1, corrupts the own sector
	 * by writing a bad-checksum record directly via the already-open
	 * mnt_paxos_file handle (same handle the log thread uses for
	 * renewal I/O).  Then sleeps 3s to let the log thread's next
	 * renewal tick fire, which should call pfs_rw_lease_acquire to
	 * re-write a valid record (the self-heal path).  Finally reads
	 * the sector back via pfs_check_host_sector and reports:
	 *   "SELF_HEALED"          — sector valid after renewal
	 *   "SELF_HEAL_FAILED:<rv>"— sector still bad/empty after 3s
	 * Then blocks until SIGTERM and unmounts cleanly.
	 */
	int r = pfs_mount(cluster, pbdname, hostid, PFS_RDWR);
	if (r != 0) {
		fprintf(stderr,
		    "pfs_mount(RW, hostid=%d) failed: %d\n",
		    hostid, r);
		return 2;
	}

	printf("MOUNTED\n");
	fflush(stdout);

	while (!g_stop && !g_promote)
		sleep(1);

	if (!g_stop) {
		pfs_mount_t *mnt = pfs_get_mount(pbdname);
		if (!mnt) {
			fprintf(stderr,
			    "pfs_get_mount(%s) failed\n", pbdname);
			pfs_umount(pbdname);
			return 2;
		}

		/*
		 * Build a record with valid magic + fresh timestamp
		 * but a deliberately wrong checksum (0xdeadbeef).
		 * This passes pfs_host_record_read's magic check but
		 * fails the checksum check → rv = PFS_HOST_ECHECKSUM.
		 * pfs_rw_lease_renew treats rv != PFS_OK as corrupt
		 * and calls pfs_rw_lease_acquire to self-heal.
		 */
		uint8_t buf[512];
		memset(buf, 0, sizeof(buf));
		uint32_t good_magic  = PFS_HOST_MAGIC;
		uint32_t flags_rw    = PFS_HOST_FL_RW;
		uint32_t hid         = (uint32_t)hostid;
		uint32_t gen         = 1;
		uint64_t ts          = (uint64_t)time(NULL);
		uint32_t bad_cksum   = 0xdeadbeef;
		memcpy(buf +   0, &good_magic, 4);
		memcpy(buf +   4, &flags_rw,   4);
		memcpy(buf +   8, &hid,        4);
		memcpy(buf +  12, &gen,        4);
		memcpy(buf +  16, &ts,         8);
		memcpy(buf + 508, &bad_cksum,  4);

		r = pfs_write_raw_host_sector(mnt, (uint32_t)hostid,
		    buf, sizeof(buf));
		pfs_put_mount(mnt);

		if (r != 0) {
			fprintf(stderr,
			    "corrupt write failed: %d\n", r);
			pfs_umount(pbdname);
			return 2;
		}

		printf("CORRUPTED\n");
		fflush(stdout);

		/*
		 * Give the log thread at least 3 renewal ticks to
		 * detect and self-heal the corrupt record.
		 */
		sleep(3);

		mnt = pfs_get_mount(pbdname);
		if (!mnt) {
			fprintf(stderr,
			    "pfs_get_mount(%s) failed\n", pbdname);
			pfs_umount(pbdname);
			return 2;
		}
		int rv = pfs_check_host_sector(mnt, (uint32_t)hostid);
		pfs_put_mount(mnt);

		if (rv == PFS_OK) {
			printf("SELF_HEALED\n");
		} else {
			printf("SELF_HEAL_FAILED:%d\n", rv);
		}
		fflush(stdout);

		while (!g_stop)
			sleep(1);
	}

	pfs_umount(pbdname);
	return 0;
}

/* ------------------------------------------------------------------ */

static int
mode_corrupt_sector(const char *cluster, const char *pbdname, int hostid,
    uint32_t target_hostid, const char *corrupt_type)
{
	/*
	 * Write a deliberately corrupt pfs_host_record into a target
	 * host's paxos sector using the same I/O path (pfs_write_paxos_
	 * sector via mnt_paxos_file) that pfs_rw_lease_acquire uses.
	 * This ensures pfs_host_record_read will see the corrupt bytes
	 * when it later reads the sector.
	 */
	int r = pfs_mount(cluster, pbdname, hostid, PFS_RDWR);
	if (r != 0) {
		fprintf(stderr,
		    "pfs_mount(RW, hostid=%d) failed: %d\n",
		    hostid, r);
		return 2;
	}

	pfs_mount_t *mnt = pfs_get_mount(pbdname);
	if (!mnt) {
		fprintf(stderr, "pfs_get_mount(%s) failed\n", pbdname);
		pfs_umount(pbdname);
		return 2;
	}

	/*
	 * Build a 512-byte corrupt host record.
	 * Layout (all little-endian):
	 *   offset   0 : hr_magic      (uint32)
	 *   offset   4 : hr_flags      (uint32)  PFS_HOST_FL_RW = 0x1
	 *   offset   8 : hr_host_id    (uint32)
	 *   offset  12 : hr_generation (uint32)
	 *   offset  16 : hr_timestamp  (uint64)  CLOCK_REALTIME seconds
	 *   offset 508 : hr_checksum   (uint32)
	 */
	uint8_t buf[512];
	memset(buf, 0, sizeof(buf));

	uint32_t now_ts = (uint32_t)time(NULL);
	uint32_t flags_rw = PFS_HOST_FL_RW;

	if (strcmp(corrupt_type, "badmagic") == 0) {
		/* Bad magic — magic check fires before checksum check */
		uint32_t bad_magic = 0xdeadbeef;
		memcpy(buf +   0, &bad_magic,    4);
		memcpy(buf +   4, &flags_rw,     4);
		memcpy(buf +   8, &target_hostid, 4);
		uint32_t gen = 1;
		memcpy(buf +  12, &gen,           4);
		uint64_t ts = (uint64_t)now_ts;
		memcpy(buf +  16, &ts,            8);
		/* no checksum needed — magic check fires first */
	} else {
		/* badchecksum: valid magic + fresh ts + wrong checksum */
		uint32_t good_magic = PFS_HOST_MAGIC;
		memcpy(buf +   0, &good_magic,    4);
		memcpy(buf +   4, &flags_rw,      4);
		memcpy(buf +   8, &target_hostid, 4);
		uint32_t gen = 1;
		memcpy(buf +  12, &gen,           4);
		uint64_t ts = (uint64_t)now_ts;
		memcpy(buf +  16, &ts,            8);
		uint32_t bad_cksum = 0xdeadbeef;
		memcpy(buf + 508, &bad_cksum,     4);
	}

	r = pfs_write_raw_host_sector(mnt, target_hostid,
	    buf, sizeof(buf));
	pfs_put_mount(mnt);
	pfs_umount(pbdname);

	if (r != 0) {
		fprintf(stderr,
		    "pfs_write_raw_host_sector(host=%u type=%s)"
		    " failed: %d\n", target_hostid, corrupt_type, r);
		return 2;
	}
	printf("CORRUPT_WRITTEN\n");
	fflush(stdout);
	return 0;
}

/* ------------------------------------------------------------------ */

static int
mode_write_prepare_record(const char *cluster, const char *pbdname,
    int hostid, uint32_t target_hostid, uint32_t gen)
{
	/*
	 * Write a FL_PREPARE record for a foreign host (simulates a
	 * host that crashed after prepare but before acquire).
	 */
	int r = pfs_mount(cluster, pbdname, hostid, PFS_RDWR);
	if (r != 0) {
		fprintf(stderr,
		    "pfs_mount(RW, hostid=%d) failed: %d\n",
		    hostid, r);
		return 2;
	}

	pfs_mount_t *mnt = pfs_get_mount(pbdname);
	if (!mnt) {
		fprintf(stderr, "pfs_get_mount(%s) failed\n", pbdname);
		pfs_umount(pbdname);
		return 2;
	}

	r = pfs_rw_lease_write_foreign_prepare(mnt,
	    target_hostid, gen);
	pfs_put_mount(mnt);
	pfs_umount(pbdname);

	if (r != 0) {
		fprintf(stderr,
		    "write-prepare-record(host=%u gen=%u)"
		    " failed: %d\n", target_hostid, gen, r);
		return 2;
	}
	printf("PREPARE_WRITTEN\n");
	fflush(stdout);
	return 0;
}

/* ------------------------------------------------------------------ */

static int
mode_write_two_records(const char *cluster, const char *pbdname,
    int hostid, uint32_t target1, uint32_t target2)
{
	/*
	 * Write fresh valid RW lease records for two foreign host IDs
	 * from within a single mount session, then unmount cleanly.
	 * The writer's own sector is zeroed by the clean unmount;
	 * the two target sectors remain with a fresh timestamp.
	 *
	 * Used by Group L tests to plant simultaneous "crashed-host"
	 * records so that pfs_rw_lease_check sees multiple live or
	 * multiple stale blockers without any of them actually holding
	 * the RW lease.
	 */
	int r = pfs_mount(cluster, pbdname, hostid, PFS_RDWR);
	if (r != 0) {
		fprintf(stderr,
		    "pfs_mount(RW, hostid=%d) failed: %d\n",
		    hostid, r);
		return 2;
	}

	pfs_mount_t *mnt = pfs_get_mount(pbdname);
	if (!mnt) {
		fprintf(stderr, "pfs_get_mount(%s) failed\n", pbdname);
		pfs_umount(pbdname);
		return 2;
	}

	int r1 = pfs_rw_lease_write_foreign(mnt, target1);
	int r2 = pfs_rw_lease_write_foreign(mnt, target2);
	pfs_put_mount(mnt);
	pfs_umount(pbdname);	/* zeroes own sector (writer_hostid) */

	if (r1 != 0 || r2 != 0) {
		fprintf(stderr,
		    "write-two-records failed: r1=%d r2=%d\n",
		    r1, r2);
		return 2;
	}
	printf("WRITTEN\n");
	fflush(stdout);
	return 0;
}

/* ------------------------------------------------------------------ */

static int
mode_watchdog_stall(const char *cluster, const char *pbdname, int hostid)
{
	/*
	 * Test helper for Group N: timer-kill watchdog.
	 *
	 * Mounts RW (which arms the timer-kill watchdog via
	 * paxos_watchdog_open).  On SIGUSR1, calls pfs_log_suspend()
	 * to stop the log thread's renewal loop — crucially WITHOUT
	 * calling pfs_leader_unload(), so the watchdog timer stays
	 * armed.  With no thread petting the watchdog, the POSIX
	 * timer fires SIGUSR2 after paxos_lease_duration seconds,
	 * and the signal handler raises SIGKILL.
	 *
	 * The test verifies that:
	 *   1. This process is killed (exit by signal, status 137).
	 *   2. A second host can subsequently mount RW after
	 *      wait_and_check detects the stale lease record.
	 */
	int r = pfs_mount(cluster, pbdname, hostid, PFS_RDWR);
	if (r != 0) {
		fprintf(stderr,
		    "pfs_mount(RW, hostid=%d) failed: %d\n",
		    hostid, r);
		return 2;
	}

	printf("MOUNTED\n");
	fflush(stdout);

	/* Wait for SIGUSR1 (stall trigger) or SIGTERM (abort) */
	while (!g_stop && !g_promote)
		sleep(1);

	if (!g_stop) {
		pfs_mount_t *mnt = pfs_get_mount(pbdname);
		if (!mnt) {
			fprintf(stderr,
			    "pfs_get_mount(%s) failed\n", pbdname);
			pfs_umount(pbdname);
			return 2;
		}

		/*
		 * Suspend the log thread: stops lease renewals and
		 * watchdog petting.  The timer-kill countdown begins.
		 */
		pfs_log_suspend(&mnt->mnt_log);
		pfs_put_mount(mnt);

		printf("STALLED\n");
		fflush(stdout);

		/*
		 * Block until the watchdog kills us.  We should never
		 * reach pfs_umount — the timer fires SIGUSR2 → SIGKILL
		 * after paxos_lease_duration seconds.  The infinite
		 * sleep ensures we don't exit cleanly before that.
		 */
		while (1)
			sleep(1);
	}

	pfs_umount(pbdname);
	return 0;
}

/* ------------------------------------------------------------------ */

static int
mode_refcount_multi(const char *cluster, const char *pbdname,
    int hostA, int hostB)
{
	int fails = 0;

	/* Phase 1: acquire RW as host A */
	int r = pfs_mount_acquire(cluster, pbdname, hostA, PFS_RDWR);
	if (r != 0) {
		printf("PHASE1_FAIL: mount_acquire(RW,A=%d) failed: %d\n",
		    hostA, r);
		fflush(stdout);
		return 2;
	}
	RC_CHECK(1,
	    pfsd_get_mnt_ref_count() == 1 &&
	    pfsd_get_mnt_wrref_count() == 1 &&
	    pfsd_get_host_ref_count(hostA) == 1 &&
	    pfsd_get_host_is_rwmnt(hostA) == true,
	    "ref=%d wrref=%d hostA_ref=%d hostA_rw=%d",
	    pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count(),
	    pfsd_get_host_ref_count(hostA), pfsd_get_host_is_rwmnt(hostA));

	/* Phase 2: acquire RO as host B */
	r = pfs_mount_acquire(cluster, pbdname, hostB, PFS_RD);
	if (r != 0) {
		printf("PHASE2_FAIL: mount_acquire(RO,B=%d) failed: %d\n",
		    hostB, r);
		fflush(stdout);
		pfs_mount_release(pbdname, hostA);
		return 2;
	}
	RC_CHECK(2,
	    pfsd_get_mnt_ref_count() == 2 &&
	    pfsd_get_mnt_wrref_count() == 1 &&
	    pfsd_get_host_ref_count(hostB) == 1 &&
	    pfsd_get_host_is_rwmnt(hostB) == false,
	    "ref=%d wrref=%d hostB_ref=%d hostB_rw=%d",
	    pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count(),
	    pfsd_get_host_ref_count(hostB), pfsd_get_host_is_rwmnt(hostB));

	/* Phase 3: release RW host A → triggers demote */
	r = pfs_mount_release(pbdname, hostA);
	RC_CHECK(3,
	    r == 0 &&
	    pfsd_get_mnt_ref_count() == 1 &&
	    pfsd_get_mnt_wrref_count() == 0 &&
	    pfsd_get_host_ref_count(hostA) == 0 &&
	    pfsd_get_host_is_rwmnt(hostA) == false,
	    "r=%d ref=%d wrref=%d hostA_ref=%d hostA_rw=%d",
	    r, pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count(),
	    pfsd_get_host_ref_count(hostA), pfsd_get_host_is_rwmnt(hostA));

	/* Phase 4: verify host_id transferred to B */
	pfs_mount_t *mnt = pfs_get_mount(pbdname);
	RC_CHECK(4,
	    mnt != NULL && mnt->mnt_host_id == (uint32_t)hostB,
	    "mnt_host_id=%u expected=%d",
	    mnt ? mnt->mnt_host_id : 0, hostB);
	if (mnt) pfs_put_mount(mnt);

	/* Phase 5: release RO host B → triggers umount */
	r = pfs_mount_release(pbdname, hostB);
	RC_CHECK(5,
	    r == 0 &&
	    pfsd_get_mnt_ref_count() == 0 &&
	    pfsd_get_mnt_wrref_count() == 0,
	    "r=%d ref=%d wrref=%d",
	    r, pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count());

	printf(fails == 0 ? "ALL_PHASES_OK\n" : "SOME_PHASES_FAILED\n");
	fflush(stdout);
	return fails == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */

static int
mode_refcount_ro_only(const char *cluster, const char *pbdname,
    int hostA, int hostB)
{
	int fails = 0;

	/* Phase 1: acquire RO as host A */
	int r = pfs_mount_acquire(cluster, pbdname, hostA, PFS_RD);
	if (r != 0) {
		printf("PHASE1_FAIL: mount_acquire(RO,A=%d) failed: %d\n",
		    hostA, r);
		fflush(stdout);
		return 2;
	}
	RC_CHECK(1,
	    pfsd_get_mnt_ref_count() == 1 &&
	    pfsd_get_mnt_wrref_count() == 0 &&
	    pfsd_get_host_ref_count(hostA) == 1 &&
	    pfsd_get_host_is_rwmnt(hostA) == false,
	    "ref=%d wrref=%d hostA_ref=%d hostA_rw=%d",
	    pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count(),
	    pfsd_get_host_ref_count(hostA), pfsd_get_host_is_rwmnt(hostA));

	/* Phase 2: acquire RO as host B */
	r = pfs_mount_acquire(cluster, pbdname, hostB, PFS_RD);
	if (r != 0) {
		printf("PHASE2_FAIL: mount_acquire(RO,B=%d) failed: %d\n",
		    hostB, r);
		fflush(stdout);
		pfs_mount_release(pbdname, hostA);
		return 2;
	}
	RC_CHECK(2,
	    pfsd_get_mnt_ref_count() == 2 &&
	    pfsd_get_mnt_wrref_count() == 0 &&
	    pfsd_get_host_ref_count(hostB) == 1 &&
	    pfsd_get_host_is_rwmnt(hostB) == false,
	    "ref=%d wrref=%d hostB_ref=%d hostB_rw=%d",
	    pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count(),
	    pfsd_get_host_ref_count(hostB), pfsd_get_host_is_rwmnt(hostB));

	/* Phase 3: release host A — identity goes to B (RO→RO, no demote) */
	r = pfs_mount_release(pbdname, hostA);
	pfs_mount_t *mnt = pfs_get_mount(pbdname);
	RC_CHECK(3,
	    r == 0 &&
	    pfsd_get_mnt_ref_count() == 1 &&
	    pfsd_get_host_ref_count(hostA) == 0 &&
	    mnt != NULL && mnt->mnt_host_id == (uint32_t)hostB,
	    "r=%d ref=%d hostA_ref=%d mnt_host_id=%u expected=%d",
	    r, pfsd_get_mnt_ref_count(), pfsd_get_host_ref_count(hostA),
	    mnt ? mnt->mnt_host_id : 0, hostB);
	if (mnt) pfs_put_mount(mnt);

	/* Phase 4: release host B → umount */
	r = pfs_mount_release(pbdname, hostB);
	RC_CHECK(4,
	    r == 0 &&
	    pfsd_get_mnt_ref_count() == 0 &&
	    pfsd_get_mnt_wrref_count() == 0,
	    "r=%d ref=%d wrref=%d",
	    r, pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count());

	printf(fails == 0 ? "ALL_PHASES_OK\n" : "SOME_PHASES_FAILED\n");
	fflush(stdout);
	return fails == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */

static int
mode_refcount_reject(const char *cluster, const char *pbdname, int hostA)
{
	int fails = 0;

	/* Phase 1: acquire RW as host A */
	int r = pfs_mount_acquire(cluster, pbdname, hostA, PFS_RDWR);
	if (r != 0) {
		printf("PHASE1_FAIL: mount_acquire(RW,A=%d) failed: %d\n",
		    hostA, r);
		fflush(stdout);
		return 2;
	}
	RC_CHECK(1,
	    pfsd_get_mnt_ref_count() == 1 &&
	    pfsd_get_mnt_wrref_count() == 1,
	    "ref=%d wrref=%d",
	    pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count());

	/* Phase 2: repeat RW same host → must be rejected */
	r = pfs_mount_acquire(cluster, pbdname, hostA, PFS_RDWR);
	RC_CHECK(2,
	    r != 0,
	    "expected rejection, got r=%d", r);

	/* Phase 3: verify refcounts unchanged after rejection */
	RC_CHECK(3,
	    pfsd_get_mnt_ref_count() == 1 &&
	    pfsd_get_mnt_wrref_count() == 1 &&
	    pfsd_get_host_ref_count(hostA) == 1,
	    "ref=%d wrref=%d hostA_ref=%d",
	    pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count(),
	    pfsd_get_host_ref_count(hostA));

	/* Phase 4: RO on existing RW same host → must be rejected */
	r = pfs_mount_acquire(cluster, pbdname, hostA, PFS_RD);
	RC_CHECK(4,
	    r != 0,
	    "expected rejection, got r=%d", r);

	/* Phase 5: verify refcounts unchanged */
	RC_CHECK(5,
	    pfsd_get_mnt_ref_count() == 1 &&
	    pfsd_get_mnt_wrref_count() == 1 &&
	    pfsd_get_host_ref_count(hostA) == 1,
	    "ref=%d wrref=%d hostA_ref=%d",
	    pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count(),
	    pfsd_get_host_ref_count(hostA));

	/* Phase 6: release RW */
	r = pfs_mount_release(pbdname, hostA);
	RC_CHECK(6,
	    r == 0 &&
	    pfsd_get_mnt_ref_count() == 0 &&
	    pfsd_get_mnt_wrref_count() == 0,
	    "r=%d ref=%d wrref=%d",
	    r, pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count());

	/* Phase 7: acquire RO as host A */
	r = pfs_mount_acquire(cluster, pbdname, hostA, PFS_RD);
	if (r != 0) {
		printf("PHASE7_FAIL: mount_acquire(RO,A=%d) failed: %d\n",
		    hostA, r);
		fflush(stdout);
		return 2;
	}
	RC_CHECK(7,
	    pfsd_get_mnt_ref_count() == 1 &&
	    pfsd_get_mnt_wrref_count() == 0,
	    "ref=%d wrref=%d",
	    pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count());

	/* Phase 8: RW on existing RO same host → must be rejected */
	r = pfs_mount_acquire(cluster, pbdname, hostA, PFS_RDWR);
	RC_CHECK(8,
	    r != 0,
	    "expected rejection, got r=%d", r);

	/* Phase 9: verify refcounts unchanged */
	RC_CHECK(9,
	    pfsd_get_mnt_ref_count() == 1 &&
	    pfsd_get_mnt_wrref_count() == 0 &&
	    pfsd_get_host_ref_count(hostA) == 1,
	    "ref=%d wrref=%d hostA_ref=%d",
	    pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count(),
	    pfsd_get_host_ref_count(hostA));

	/* Phase 10: release RO → umount */
	r = pfs_mount_release(pbdname, hostA);
	RC_CHECK(10,
	    r == 0 &&
	    pfsd_get_mnt_ref_count() == 0,
	    "r=%d ref=%d",
	    r, pfsd_get_mnt_ref_count());

	printf(fails == 0 ? "ALL_PHASES_OK\n" : "SOME_PHASES_FAILED\n");
	fflush(stdout);
	return fails == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */

static int
mode_refcount_three_client(const char *cluster, const char *pbdname,
    int hostA, int hostB, int hostC)
{
	int fails = 0;

	/* Phase 1: acquire RW as host A */
	int r = pfs_mount_acquire(cluster, pbdname, hostA, PFS_RDWR);
	if (r != 0) {
		printf("PHASE1_FAIL: mount_acquire(RW,A=%d) failed: %d\n",
		    hostA, r);
		fflush(stdout);
		return 2;
	}
	RC_CHECK(1,
	    pfsd_get_mnt_ref_count() == 1 &&
	    pfsd_get_mnt_wrref_count() == 1,
	    "ref=%d wrref=%d",
	    pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count());

	/* Phase 2: acquire RO as host B */
	r = pfs_mount_acquire(cluster, pbdname, hostB, PFS_RD);
	if (r != 0) {
		printf("PHASE2_FAIL: mount_acquire(RO,B=%d) failed: %d\n",
		    hostB, r);
		fflush(stdout);
		pfs_mount_release(pbdname, hostA);
		return 2;
	}
	RC_CHECK(2,
	    pfsd_get_mnt_ref_count() == 2,
	    "ref=%d", pfsd_get_mnt_ref_count());

	/* Phase 3: acquire RO as host C */
	r = pfs_mount_acquire(cluster, pbdname, hostC, PFS_RD);
	if (r != 0) {
		printf("PHASE3_FAIL: mount_acquire(RO,C=%d) failed: %d\n",
		    hostC, r);
		fflush(stdout);
		pfs_mount_release(pbdname, hostB);
		pfs_mount_release(pbdname, hostA);
		return 2;
	}
	RC_CHECK(3,
	    pfsd_get_mnt_ref_count() == 3 &&
	    pfsd_get_mnt_wrref_count() == 1,
	    "ref=%d wrref=%d",
	    pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count());

	/*
	 * Phase 4: release RW host A → triggers demote.
	 * pfs_host_get_another_id scans from host 1 upward, so identity
	 * goes to whichever of B,C has the lower host_id.
	 */
	r = pfs_mount_release(pbdname, hostA);
	int expected_id = (hostB < hostC) ? hostB : hostC;
	pfs_mount_t *mnt = pfs_get_mount(pbdname);
	RC_CHECK(4,
	    r == 0 &&
	    pfsd_get_mnt_ref_count() == 2 &&
	    pfsd_get_mnt_wrref_count() == 0 &&
	    mnt != NULL && mnt->mnt_host_id == (uint32_t)expected_id,
	    "r=%d ref=%d wrref=%d mnt_host_id=%u expected=%d",
	    r, pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count(),
	    mnt ? mnt->mnt_host_id : 0, expected_id);
	if (mnt) pfs_put_mount(mnt);

	/* Phase 5: release host with the lower id → identity to the other */
	int other_id = (hostB < hostC) ? hostC : hostB;
	r = pfs_mount_release(pbdname, expected_id);
	mnt = pfs_get_mount(pbdname);
	RC_CHECK(5,
	    r == 0 &&
	    pfsd_get_mnt_ref_count() == 1 &&
	    mnt != NULL && mnt->mnt_host_id == (uint32_t)other_id,
	    "r=%d ref=%d mnt_host_id=%u expected=%d",
	    r, pfsd_get_mnt_ref_count(),
	    mnt ? mnt->mnt_host_id : 0, other_id);
	if (mnt) pfs_put_mount(mnt);

	/* Phase 6: release last host → umount */
	r = pfs_mount_release(pbdname, other_id);
	RC_CHECK(6,
	    r == 0 &&
	    pfsd_get_mnt_ref_count() == 0 &&
	    pfsd_get_mnt_wrref_count() == 0,
	    "r=%d ref=%d wrref=%d",
	    r, pfsd_get_mnt_ref_count(), pfsd_get_mnt_wrref_count());

	printf(fails == 0 ? "ALL_PHASES_OK\n" : "SOME_PHASES_FAILED\n");
	fflush(stdout);
	return fails == 0 ? 0 : 1;
}

/* ================================================================== */

int
main(int argc, char *argv[])
{
	if (argc < 4) {
		fprintf(stderr,
		    "usage: pfs_lease_hold <cluster> <pbdname> <hostid>"
		    " [rw|ro|promote|self-corrupt-renew|watchdog-stall]\n"
		    "       pfs_lease_hold <cluster> <pbdname> <rw_hostid>"
		    " demote <ro_hostid>\n"
		    "       pfs_lease_hold <cluster> <pbdname> <writer_hostid>"
		    " corrupt-sector <target_hostid> <badmagic|badchecksum>\n");
		return 1;
	}

	const char *cluster = argv[1];
	const char *pbdname = argv[2];
	int hostid          = atoi(argv[3]);
	const char *mode    = (argc >= 5) ? argv[4] : "rw";

	signal(SIGTERM, sighandler);
	signal(SIGHUP,  sighandler);
	signal(SIGUSR1, sigusr1handler);

	if (strcmp(mode, "rw") == 0 || strcmp(mode, "ro") == 0)
		return mode_rw_ro(cluster, pbdname, hostid, mode);

	if (strcmp(mode, "promote") == 0)
		return mode_promote(cluster, pbdname, hostid);

	if (strcmp(mode, "demote") == 0) {
		if (argc < 6) {
			fprintf(stderr,
			    "usage: pfs_lease_hold <cluster> <pbdname>"
			    " <rw_hostid> demote <ro_hostid>\n");
			return 1;
		}
		return mode_demote(cluster, pbdname, hostid,
		    atoi(argv[5]));
	}

	if (strcmp(mode, "self-corrupt-renew") == 0)
		return mode_self_corrupt_renew(cluster, pbdname, hostid);

	if (strcmp(mode, "corrupt-sector") == 0) {
		if (argc < 7) {
			fprintf(stderr,
			    "usage: pfs_lease_hold <cluster> <pbdname>"
			    " <writer_hostid> corrupt-sector"
			    " <target_hostid> <badmagic|badchecksum>\n");
			return 1;
		}
		return mode_corrupt_sector(cluster, pbdname, hostid,
		    (uint32_t)atoi(argv[5]), argv[6]);
	}

	if (strcmp(mode, "write-prepare-record") == 0) {
		if (argc < 7) {
			fprintf(stderr,
			    "usage: pfs_lease_hold <cluster> <pbdname>"
			    " <writer_hostid> write-prepare-record"
			    " <target_hostid> <generation>\n");
			return 1;
		}
		return mode_write_prepare_record(cluster, pbdname, hostid,
		    (uint32_t)atoi(argv[5]), (uint32_t)atoi(argv[6]));
	}

	if (strcmp(mode, "write-two-records") == 0) {
		if (argc < 7) {
			fprintf(stderr,
			    "usage: pfs_lease_hold <cluster> <pbdname>"
			    " <writer_hostid> write-two-records"
			    " <target1> <target2>\n");
			return 1;
		}
		return mode_write_two_records(cluster, pbdname, hostid,
		    (uint32_t)atoi(argv[5]), (uint32_t)atoi(argv[6]));
	}

	if (strcmp(mode, "watchdog-stall") == 0)
		return mode_watchdog_stall(cluster, pbdname, hostid);

	if (strcmp(mode, "refcount-multi") == 0) {
		if (argc < 6) {
			fprintf(stderr,
			    "usage: pfs_lease_hold <cluster> <pbdname>"
			    " <hostA> refcount-multi <hostB>\n");
			return 1;
		}
		return mode_refcount_multi(cluster, pbdname, hostid,
		    atoi(argv[5]));
	}

	if (strcmp(mode, "refcount-ro-only") == 0) {
		if (argc < 6) {
			fprintf(stderr,
			    "usage: pfs_lease_hold <cluster> <pbdname>"
			    " <hostA> refcount-ro-only <hostB>\n");
			return 1;
		}
		return mode_refcount_ro_only(cluster, pbdname, hostid,
		    atoi(argv[5]));
	}

	if (strcmp(mode, "refcount-reject") == 0)
		return mode_refcount_reject(cluster, pbdname, hostid);

	if (strcmp(mode, "refcount-three-client") == 0) {
		if (argc < 7) {
			fprintf(stderr,
			    "usage: pfs_lease_hold <cluster> <pbdname>"
			    " <hostA> refcount-three-client <hostB> <hostC>\n");
			return 1;
		}
		return mode_refcount_three_client(cluster, pbdname, hostid,
		    atoi(argv[5]), atoi(argv[6]));
	}

	fprintf(stderr, "unknown mode: %s\n", mode);
	return 1;
}
