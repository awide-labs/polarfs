#include <stdio.h>
#include <time.h>

#include "cmd_impl.h"
#include "pfs_impl.h"
#include "pfs_mount.h"
#include "pfs_paxos.h"

static void
usage_lease()
{
	printf("pfs -C cluster lease /pbdname\n"
	    "	Show RW lease status for all hosts.\n");
}

static int
getopt_lease(int argc, char *argv[], cmd_opts_t *co)
{
	int opt;

	optind = 1;
	while ((opt = getopt(argc, argv, "h")) != -1) {
		switch (opt) {
		case 'h':
		default:
			return -1;
		}
	}
	return optind;
}

static const char *
flags_str(uint32_t flags, char *buf, size_t bufsz)
{
	if (flags == 0) {
		snprintf(buf, bufsz, "none");
		return buf;
	}

	buf[0] = '\0';
	if (flags & PFS_HOST_FL_RW)
		strncat(buf, "RW", bufsz - strlen(buf) - 1);
	if (flags & PFS_HOST_FL_PREPARE) {
		if (buf[0] != '\0')
			strncat(buf, "|", bufsz - strlen(buf) - 1);
		strncat(buf, "PREPARE", bufsz - strlen(buf) - 1);
	}
	return buf;
}

/* "%Y-%m-%d %H:%M:%S" = 19 chars + NUL */
#define FMT_TIME_BUFSZ	32
/* "RW|PREPARE" = 10 chars + NUL */
#define FLAGS_BUFSZ	16

static const char *
fmt_time(uint64_t epoch, char *buf, size_t bufsz)
{
	time_t t = (time_t)epoch;
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(buf, bufsz, "%Y-%m-%d %H:%M:%S", &tm);
	return buf;
}

static int
cmd_lease(int argc, char *argv[], cmd_opts_t *co)
{
	const char *pbdname;
	pfs_mount_t *mnt;
	pfs_leader_record_t lr;
	uint32_t checksum;
	struct timespec now;
	int64_t lease_dur;

	if (argc < 1)
		return -1;

	pbdname = argv[0];

	mnt = pfs_get_mount(pbdname);
	if (mnt == NULL) {
		pfs_etrace("cant get mount %s\n", pbdname);
		ERR_RETVAL(ENODEV);
	}

	if (read_leader(mnt, &lr, &checksum) < 0) {
		printf("ERROR: cannot read leader record\n");
		pfs_put_mount(mnt);
		return -1;
	}

	lease_dur = pfs_paxos_lease_duration();
	clock_gettime(CLOCK_REALTIME, &now);

	char tbuf[FMT_TIME_BUFSZ];

	printf("=== RW Lease Status for %s ===\n", pbdname);
	printf("lease_duration: %llds\n", (long long)lease_dur);
	printf("current_time:   %lld (%s)\n", (long long)now.tv_sec,
	    fmt_time(now.tv_sec, tbuf, sizeof(tbuf)));
	printf("num_hosts:      %llu\n", (unsigned long long)lr.num_hosts);
	printf("max_hosts:      %llu\n\n", (unsigned long long)lr.max_hosts);

	uint32_t scan_limit = (uint32_t)lr.max_hosts;
	bool found_holder = false;
	bool found_any = false;

	for (uint32_t i = 1; i <= scan_limit; i++) {
		pfs_host_record_t hr;
		int rv = pfs_host_record_read(mnt, i, &hr);

		if (rv < 0) {
			printf("host %-3u  ERROR reading sector (rv=%d)\n",
			    i, rv);
			found_any = true;
			continue;
		}

		/* empty sector — skip silently */
		if (hr.hr_magic == 0)
			continue;

		found_any = true;
		char fbuf[FLAGS_BUFSZ];
		int64_t age = now.tv_sec - (int64_t)hr.hr_timestamp;
		int64_t expires_in = lease_dur - age;

		printf("host %-3u  flags=%-11s gen=%-5u ballot=%-12llu\n"
		    "          timestamp=%llu (%s)  age=%llds\n",
		    hr.hr_host_id,
		    flags_str(hr.hr_flags, fbuf, sizeof(fbuf)),
		    hr.hr_generation,
		    (unsigned long long)hr.hr_bal,
		    (unsigned long long)hr.hr_timestamp,
		    fmt_time(hr.hr_timestamp, tbuf, sizeof(tbuf)),
		    (long long)age);

		if (hr.hr_flags & PFS_HOST_FL_RW) {
			if (expires_in > 0) {
				printf("          STATUS: ** ACTIVE RW HOLDER "
				    "(expires in %llds) **\n",
				    (long long)expires_in);
				found_holder = true;
			} else {
				printf("          STATUS: expired %llds ago\n",
				    (long long)(-expires_in));
			}
		} else if (hr.hr_flags & PFS_HOST_FL_PREPARE) {
			printf("          STATUS: prepare phase "
			    "(mbal=%llu)\n",
			    (unsigned long long)hr.hr_mbal);
		}
		printf("\n");
	}

	if (!found_any)
		printf("All host sectors empty.\n");
	else if (!found_holder)
		printf("No active RW lease holder.\n");

	pfs_put_mount(mnt);
	return 0;
}

PFSCMD_INFO(lease, CMDF_MOUNT, PFS_RD,
    getopt_lease, cmd_lease, usage_lease,
    "show RW lease status");
