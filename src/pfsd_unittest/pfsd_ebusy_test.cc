/*
 * pfsd_ebusy_test — end-to-end test for EBUSY recovery in pfsdaemon.
 *
 * Each mode is a separate process lifecycle (mount -> action -> unmount)
 * so the pfsd SDK initialises cleanly each time.
 *
 * Usage:
 *   pfsd_ebusy_test <cluster> <pbdname> hold-ro <hostid>
 *   pfsd_ebusy_test <cluster> <pbdname> promote-rw <hostid>
 *   pfsd_ebusy_test <cluster> <pbdname> promote-crash <hostid>
 *   pfsd_ebusy_test <cluster> <pbdname> stat <hostid> <path>
 *
 * Modes:
 *   hold-ro    — mount RO, print "MOUNTED", block until SIGTERM, unmount.
 *   promote-rw — mount RO, then pfsd_remount to RW.  Prints
 *                "PROMOTE_OK" or "PROMOTE_FAILED:<errno>", then unmount.
 *   promote-crash — like promote-rw but exits WITHOUT unmounting after
 *                a failed promote (simulates client crash / connection
 *                drop).  Used to test that pfsdaemon correctly releases
 *                the client's refcount via the disconnect handler.
 *   stat       — mount RO, stat <path>.  Prints "STAT_OK" (file found),
 *                "STAT_ENOENT" (not found, pfsdaemon alive), or
 *                "STAT_FAIL:<errno>" (pfsdaemon may have crashed).
 *                Then unmount.
 *
 * Exit codes:
 *   0  — success (mode-specific)
 *   1  — bad usage
 *   2  — setup failure (mount failed)
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "pfsd_sdk.h"
#include "pfs_mount.h"

static volatile sig_atomic_t g_stop = 0;
static void sighandler(int) { g_stop = 1; }

static int
mode_hold_ro(const char *cluster, const char *pbdname, int hostid)
{
	int r = pfsd_mount(cluster, pbdname, hostid,
	    MNTFLG_RD | MNTFLG_LOG);
	if (r < 0) {
		fprintf(stderr, "pfsd_mount(RO, hid=%d) failed: %s\n",
		    hostid, strerror(errno));
		return 2;
	}
	printf("MOUNTED\n");
	fflush(stdout);

	signal(SIGTERM, sighandler);
	signal(SIGHUP, sighandler);
	while (!g_stop)
		sleep(1);

	pfsd_umount_force(pbdname);
	return 0;
}

static int
mode_promote_rw(const char *cluster, const char *pbdname, int hostid)
{
	int r = pfsd_mount(cluster, pbdname, hostid,
	    MNTFLG_RD | MNTFLG_LOG);
	if (r < 0) {
		fprintf(stderr, "pfsd_mount(RO, hid=%d) failed: %s\n",
		    hostid, strerror(errno));
		return 2;
	}

	r = pfsd_remount(cluster, pbdname, hostid,
	    MNTFLG_RD | MNTFLG_WR | MNTFLG_LOG);
	if (r == 0) {
		printf("PROMOTE_OK\n");
	} else {
		printf("PROMOTE_FAILED:%d\n", errno);
	}
	fflush(stdout);

	pfsd_umount_force(pbdname);
	return 0;
}

static int
mode_promote_crash(const char *cluster, const char *pbdname, int hostid)
{
	int r = pfsd_mount(cluster, pbdname, hostid,
	    MNTFLG_RD | MNTFLG_LOG);
	if (r < 0) {
		fprintf(stderr, "pfsd_mount(RO, hid=%d) failed: %s\n",
		    hostid, strerror(errno));
		return 2;
	}

	r = pfsd_remount(cluster, pbdname, hostid,
	    MNTFLG_RD | MNTFLG_WR | MNTFLG_LOG);
	if (r == 0) {
		printf("PROMOTE_OK\n");
	} else {
		printf("PROMOTE_FAILED:%d\n", errno);
	}
	fflush(stdout);

	/*
	 * Exit without calling pfsd_umount_force.  The pfsdaemon sees
	 * a client disconnect (readEOF) and must release the mount via
	 * pfs_mount_release.  If the server incorrectly clears
	 * host_id_ on failed remount, the release is skipped and
	 * refcounts leak.
	 */
	_exit(0);
}

static int
mode_stat(const char *cluster, const char *pbdname, int hostid,
    const char *path)
{
	int r = pfsd_mount(cluster, pbdname, hostid,
	    MNTFLG_RD | MNTFLG_LOG);
	if (r < 0) {
		fprintf(stderr, "pfsd_mount(RO, hid=%d) failed: %s\n",
		    hostid, strerror(errno));
		return 2;
	}

	struct stat st;
	r = pfsd_stat(path, &st);
	if (r == 0) {
		printf("STAT_OK\n");
	} else if (errno == ENOENT) {
		printf("STAT_ENOENT\n");
	} else {
		printf("STAT_FAIL:%d\n", errno);
	}
	fflush(stdout);

	pfsd_umount_force(pbdname);
	return 0;
}

int
main(int argc, char *argv[])
{
	if (argc < 5) {
		fprintf(stderr,
		    "usage: pfsd_ebusy_test <cluster> <pbdname>"
		    " <hold-ro|promote-rw|stat> <hostid> [path]\n");
		return 1;
	}

	const char *cluster = argv[1];
	const char *pbdname = argv[2];
	const char *mode    = argv[3];
	int hostid          = atoi(argv[4]);

	if (strcmp(mode, "hold-ro") == 0)
		return mode_hold_ro(cluster, pbdname, hostid);

	if (strcmp(mode, "promote-rw") == 0)
		return mode_promote_rw(cluster, pbdname, hostid);

	if (strcmp(mode, "promote-crash") == 0)
		return mode_promote_crash(cluster, pbdname, hostid);

	if (strcmp(mode, "stat") == 0) {
		if (argc < 6) {
			fprintf(stderr,
			    "usage: pfsd_ebusy_test <cluster> <pbdname>"
			    " stat <hostid> <path>\n");
			return 1;
		}
		return mode_stat(cluster, pbdname, hostid, argv[5]);
	}

	fprintf(stderr, "unknown mode: %s\n", mode);
	return 1;
}
