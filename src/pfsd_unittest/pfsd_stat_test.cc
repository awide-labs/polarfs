/*
 * pfsd_stat_test — verify that devstat (device-level I/O counters)
 * and mountstat (mount-level per-file-type I/O counters) increment
 * when file I/O is performed through the pfsd SDK.
 *
 * pfsadm iostat semantics (devstat):
 *
 * The daemon stores ABSOLUTE monotonic counters (ds_start_count,
 * ds_end_count, ds_ops[type], ds_bytes[type], ds_local_busy_time, …)
 * that grow from process start.  pfsadm fetches a snapshot, computes
 * the delta from the previous snapshot, and normalizes by elapsed
 * time.
 *
 * With "-c 1" there is only one snapshot: last_stat is zero and
 * elapsed defaults to 1.0, so every printed column is the raw
 * absolute value.  We call "pfsadm iostat -c 1" twice (two separate
 * processes) and subtract the absolute values ourselves.
 *
 * pfsadm mountstat semantics (mountstat):
 *
 * mountstat stores per-second historical buckets.  A maintenance
 * thread drains hot shards to cold slots, and pfs_mntstat_snap
 * applies a 2-second lookback, so data is visible ~2 s after I/O.
 * We query "pfsadm mountstat -c 1 -b <t_before> -r 5"; ops are
 * summed across the range, latency/io_size from any second with
 * ops > 0.
 *
 * The test file is under /<pbd>/data/pg_wal/ → "redo_log".
 *
 * Usage: pfsd_stat_test <hostid> <cluster> <pbdname>
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "pfsd_testenv.h"
#include "pfsd_sdk.h"

using std::string;

static const char PFSADM[] = "./bin/pfsadm";

struct IostatSnapshot {
	std::string	device_name;	/* device column                     */
	int64_t		read_ops;	/* r/s  — absolute ds_ops[READ]      */
	int64_t		write_ops;	/* w/s  — absolute ds_ops[WRITE]     */
	double		read_mbytes;	/* Mr/s — absolute ds_bytes[RD] / MB */
	double		write_mbytes;	/* Mw/s — absolute ds_bytes[WR] / MB */
	double		avg_read_us;	/* us/r — avg read duration (µs)     */
	double		avg_write_us;	/* us/w — avg write duration (µs)    */
	double		avg_other_us;	/* us/o — placeholder, always 0.0    */
	double		avg_total_us;	/* us/t — avg total duration (µs)    */
	int64_t		qlen;		/* in-flight: start_count - end_count*/
	double		util_pct;	/* %util — absolute busy %           */
};

/*
 * Run "pfsadm iostat -c 1 <pbd>" and parse the first data line into
 * *snap.  Returns false (with *err set) on failure.
 */
static bool
take_snapshot(const string &pbdname, IostatSnapshot *snap, string *err)
{
	string cmd = string(PFSADM) + " iostat -c 1 " + pbdname + " 2>&1";

	FILE *fp = popen(cmd.c_str(), "r");
	if (fp == nullptr) {
		*err = "popen failed";
		return false;
	}

	string out;
	char   buf[1024];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
		out.append(buf, n);
	pclose(fp);

	/* pfsadm may emit NUL bytes in the device name field. */
	for (char &c : out)
		if (c == '\0') c = ' ';

	if (out.empty()) {
		*err = "pfsadm iostat produced no output";
		return false;
	}

	size_t pos = 0;
	while (pos < out.size()) {
		size_t eol = out.find('\n', pos);
		string line = (eol == string::npos)
		    ? out.substr(pos)
		    : out.substr(pos, eol - pos);
		pos = (eol == string::npos) ? out.size() : eol + 1;

		if (line.empty() || line.rfind("device", 0) == 0)
			continue;

		char	devname[256];
		long long r_ops = 0, w_ops = 0;
		double	mrs, mws, usr, usw, uso, ust, qlen, util;
		int n = std::sscanf(line.c_str(),
		    "%255s %lld %lld %lf %lf %lf %lf %lf %lf %lf %lf",
		    devname, &r_ops, &w_ops,
		    &mrs, &mws, &usr, &usw, &uso, &ust, &qlen, &util);
		if (n == 11) {
			snap->device_name  = devname;
			snap->read_ops     = r_ops;
			snap->write_ops    = w_ops;
			snap->read_mbytes  = mrs;
			snap->write_mbytes = mws;
			snap->avg_read_us  = usr;
			snap->avg_write_us = usw;
			snap->avg_other_us = uso;
			snap->avg_total_us = ust;
			snap->qlen         = static_cast<int64_t>(qlen);
			snap->util_pct     = util;
			return true;
		}
	}

	*err = "could not parse iostat output:\n" + out;
	return false;
}

/*
 * Per-operation mountstat counters parsed from "pfsadm mountstat".
 */
struct MountstatOp {
	int64_t	ops = 0;
	double	latency = 0;	/* microseconds	*/
	double	io_size = 0;	/* bytes (0 for non-I/O ops like open/fsync) */
};

/* Parsed mountstat snapshot for the redo_log file type.  */
struct MountstatSnapshot {
	/* Per-file-type (redo_log) — API-level operations. */
	MountstatOp ft_open;		/* pfsd_creat → "open"		*/
	MountstatOp ft_pwrite;		/* pfsd_write → "pwrite"	*/
	MountstatOp ft_pread;		/* pfsd_read  → "pread"		*/
	MountstatOp ft_fsync;		/* pfsd_fsync → "fsync"		*/

	/* Summarize (all) — file-level operations.  Only 2 fields: the
	 * pread and fsync summarize paths are not instrumented in the
	 * daemon, so they have no sm_ counterpart here.  */
	MountstatOp sm_creat;		/* → "creat"			*/
	MountstatOp sm_write_pwrite;	/* → "write+pwrite"		*/
};

/*
 * Run "pfsadm mountstat <pbd> -c 1 -b <begin> -r <range>" and parse
 * the output into *snap.  For each target operation, ops are summed
 * across all seconds in the range; latency and io_size are taken from
 * the first second where ops > 0.  Returns false (with *err set) on
 * failure.
 */
static bool
take_mountstat_snapshot(const string &pbdname, int64_t begin_time,
    int range, MountstatSnapshot *snap, string *err)
{
	char cmd[512];
	std::snprintf(cmd, sizeof(cmd),
	    "%s mountstat %s -c 1 -b %ld -r %d 2>&1",
	    PFSADM, pbdname.c_str(), (long)begin_time, range);

	FILE *fp = popen(cmd, "r");
	if (fp == nullptr) {
		*err = "popen failed";
		return false;
	}

	string out;
	char   buf[4096];
	while (fgets(buf, sizeof(buf), fp) != nullptr)
		out += buf;
	pclose(fp);

	if (out.empty()) {
		*err = "pfsadm mountstat produced no output";
		return false;
	}

	/* Line: "Sep  1 12:34:56  <ftype>  <op>  <ops>  <lat>  <iosize>"
	 * Timestamp has spaces, skipped as 3 fields.  n==5 → file_type
	 * line (iosize is a number); n==4 → Summarize line (iosize is
	 * "N/A"); n<4 → header/blank — skip.  */
	size_t pos = 0;
	while (pos < out.size()) {
		size_t eol = out.find('\n', pos);
		string line = (eol == string::npos)
		    ? out.substr(pos)
		    : out.substr(pos, eol - pos);
		pos = (eol == string::npos) ? out.size() : eol + 1;

		if (line.empty())
			continue;

		char	ftype[32];
		char	opname[32];
		long long ops = 0;
		double	lat = 0, iosize = 0;
		int n = std::sscanf(line.c_str(),
		    "%*s %*d %*s %31s %31s %lld %lf %lf",
		    ftype, opname, &ops, &lat, &iosize);
		if (n < 4)
			continue;

		bool is_filetype = (n == 5);	/* io_size present	*/
		bool is_redo = (std::strcmp(ftype, "redo_log") == 0);
		bool is_all  = (std::strcmp(ftype, "all") == 0);

		/* Accumulate ops; pick latency & io_size from first non-zero. */
		auto accumulate = [ops, lat, iosize](MountstatOp *o) {
			o->ops += ops;
			if (ops > 0 && o->latency == 0) {
				o->latency = lat;
				o->io_size = iosize;
			}
		};

		if (is_filetype && is_redo) {
			if (std::strcmp(opname, "open") == 0)
				accumulate(&snap->ft_open);
			else if (std::strcmp(opname, "pwrite") == 0)
				accumulate(&snap->ft_pwrite);
			else if (std::strcmp(opname, "pread") == 0)
				accumulate(&snap->ft_pread);
			else if (std::strcmp(opname, "fsync") == 0)
				accumulate(&snap->ft_fsync);
		} else if (is_all && !is_filetype) {
			if (std::strcmp(opname, "creat") == 0)
				accumulate(&snap->sm_creat);
			else if (std::strcmp(opname, "write+pwrite") == 0)
				accumulate(&snap->sm_write_pwrite);
		}
	}

	return true;
}

/*
 * Generate device-level I/O: create a file, write 128 KB in 32
 * 4 KB chunks, fsync it, read it back in 32 4 KB chunks, then close.
 *
 * Produces at least 32 write + 1 flush + 32 read device I/O operations
 * plus metadata I/O from creat/close.
 */
static void
do_file_io(const string &pbdpath)
{
	static const size_t CHUNK  = 4 * 1024;
	static const int    NCHUNK = 32;	/* 32 × 4 KB = 128 KB	*/

	int fd = pfsd_creat(pbdpath.c_str(), 0);
	ASSERT_GE(fd, 0) << "pfsd_creat failed: " << strerror(errno);

	std::vector<char> wbuf(CHUNK, 'A');
	for (int i = 0; i < NCHUNK; i++) {
		ssize_t wr = pfsd_write(fd, wbuf.data(), wbuf.size());
		ASSERT_EQ(static_cast<ssize_t>(wbuf.size()), wr)
		    << "pfsd_write failed at chunk " << i
		    << ": " << strerror(errno);
	}

	ASSERT_EQ(pfsd_fsync(fd), 0)
	    << "pfsd_fsync failed: " << strerror(errno);

	std::vector<char> rbuf(CHUNK);
	pfsd_lseek(fd, 0, SEEK_SET);
	for (int i = 0; i < NCHUNK; i++) {
		ssize_t rd = pfsd_read(fd, rbuf.data(), rbuf.size());
		ASSERT_EQ(static_cast<ssize_t>(rbuf.size()), rd)
		    << "pfsd_read failed at chunk " << i
		    << ": " << strerror(errno);
	}

	pfsd_close(fd);
}

static bool
set_config(const string &pbdname, const char *name, int value)
{
	string cmd = string(PFSADM) + " config -s " + pbdname + " " +
	    		 name + " " + std::to_string(value) + " 2>&1";

	int rc = system(cmd.c_str());
	return rc == 0;
}

class DevstatTest : public testing::Test {
protected:
    void SetUp() override {
        const string &pbdname = g_testenv->pbdname_;

        /*
         * Create /<pbd>/data/pg_wal/ so the test file is classified
         * as FILE_REDO_LOG ("redo_log") by pfs_get_file_type, which
         * skips the first path component (data dir) and matches
         * "/pg_wal/".  pfsd_mkdir does not generate mountstat
         * counters, so this does not pollute the snapshot.
         * Ignore errors if the directories already exist.
         */
        string datadir   = "/" + pbdname + "/data";
        string pg_waldir = "/" + pbdname + "/data/pg_wal";
        pfsd_mkdir(datadir.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
        pfsd_mkdir(pg_waldir.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);

        pbdpath_ = "/" + pbdname + "/data/pg_wal/devstat_test.bin";

        /* Enable devstat and mountstat (runtime options, no restart). */
        ASSERT_TRUE(set_config(pbdname, "devstat_enable", 1))
            << "failed to enable devstat_enable";
        ASSERT_TRUE(set_config(pbdname, "mountstat_enable", 1))
            << "failed to enable mountstat_enable";
    }

    void TearDown() override {
        pfsd_unlink(pbdpath_.c_str());

        /* Disable devstat and mountstat. */
        const string &pbdname = g_testenv->pbdname_;
        set_config(pbdname, "devstat_enable", 0);
        set_config(pbdname, "mountstat_enable", 0);
    }

    string pbdpath_;
};

/*
 * Take a baseline devstat snapshot, do some file I/O,
 * take a final snapshot, and assert the devstat counters moved.
 */
TEST_F(DevstatTest, CountersIncrementAfterIO)
{
	const string &pbdname = g_testenv->pbdname_;
	string err;

	/* 1. Baseline snapshot — absolute counters before our I/O. */
	IostatSnapshot base{};
	ASSERT_TRUE(take_snapshot(pbdname, &base, &err)) << err;

	/*
	 * 2. Wait 3 s so leftover statistics from previous tests
	 *    don't pollute this one.
	 */
	sleep(3);
	time_t t_before = time(nullptr);

	/* 3. Generate device-level read/write I/O. */
	ASSERT_NO_FATAL_FAILURE(do_file_io(pbdpath_));

	/* 4. Final snapshot — absolute counters after our I/O. */
	IostatSnapshot final_snap{};
	ASSERT_TRUE(take_snapshot(pbdname, &final_snap, &err)) << err;

	/* iostat returned data for the requested device. */
	EXPECT_EQ(final_snap.device_name, pbdname)
	    << "iostat returned data for the wrong device";

	/* Write ops increased: 32 × pfsd_write(4 KB) + metadata from creat. */
	EXPECT_GT(final_snap.write_ops - base.write_ops, 0)
	    << "write ops did not increase (base=" << base.write_ops
	    << ", final=" << final_snap.write_ops << ")";

	/* Write bytes increased by at least 128 KB. */
	{
		double wbytes_delta =
		    (final_snap.write_mbytes - base.write_mbytes) * (1 << 20);
		EXPECT_GE(wbytes_delta, 128 * 1024)
		    << "write bytes did not increase enough (base="
		    << base.write_mbytes << " MB, final="
		    << final_snap.write_mbytes << " MB, delta="
		    << wbytes_delta << " B)";
	}

	/* Read ops increased: 32 reads. */
	EXPECT_GT(final_snap.read_ops - base.read_ops, 0)
	    << "read ops did not increase (base=" << base.read_ops
	    << ", final=" << final_snap.read_ops << ")";

	/* Read bytes increased: 128 KB from device */
	{
		double rbytes_delta =
		    (final_snap.read_mbytes - base.read_mbytes) * (1 << 20);
		EXPECT_GE(rbytes_delta, 128 * 1024)
		    << "read bytes did not increase enough (base="
		    << base.read_mbytes << " MB, final="
		    << final_snap.read_mbytes << " MB, delta="
		    << rbytes_delta << " B)";
	}

	EXPECT_GT(final_snap.avg_read_us, 0)
	    << "avg read duration is zero (final avg_read_us="
	    << final_snap.avg_read_us << ")";
	EXPECT_GT(final_snap.avg_write_us, 0)
	    << "avg write duration is zero (final avg_write_us="
	    << final_snap.avg_write_us << ")";

	int64_t base_total_ops  = base.read_ops + base.write_ops;
	int64_t final_total_ops = final_snap.read_ops + final_snap.write_ops;
	EXPECT_GT(final_total_ops - base_total_ops, 0)
	    << "total ops did not increase (base=" << base_total_ops
	    << ", final=" << final_total_ops << ")";

	/* mountstat uses per-second buckets with a 2 s lookback, so we
	 * wait 5 s after I/O. */

	sleep(5);

	int64_t begin_time = (int64_t)t_before;
	MountstatSnapshot msnap{};
	ASSERT_TRUE(take_mountstat_snapshot(pbdname, begin_time, 5,
	    &msnap, &err)) << err;

	/* File_type section — redo_log, per-operation.  io_size is the
	 * average per-op size, so 32 × 4 KB → 4096, not 131072.  */

	EXPECT_GE(msnap.ft_open.ops, 1)
	    << "mountstat redo_log open ops is zero";
	EXPECT_GT(msnap.ft_open.latency, 0)
	    << "mountstat redo_log open latency is zero";

	EXPECT_GE(msnap.ft_pwrite.ops, 32)
	    << "mountstat redo_log pwrite ops too low";
	EXPECT_GT(msnap.ft_pwrite.latency, 0)
	    << "mountstat redo_log pwrite latency is zero";
	EXPECT_GE(msnap.ft_pwrite.io_size, 4 * 1024)
	    << "mountstat redo_log pwrite io_size too small (got "
	    << msnap.ft_pwrite.io_size << ")";

	EXPECT_GE(msnap.ft_pread.ops, 32)
	    << "mountstat redo_log pread ops too low";
	EXPECT_GT(msnap.ft_pread.latency, 0)
	    << "mountstat redo_log pread latency is zero";
	EXPECT_GE(msnap.ft_pread.io_size, 4 * 1024)
	    << "mountstat redo_log pread io_size too small (got "
	    << msnap.ft_pread.io_size << ")";

	EXPECT_GE(msnap.ft_fsync.ops, 1)
	    << "mountstat redo_log fsync ops is zero";
	EXPECT_GT(msnap.ft_fsync.latency, 0)
	    << "mountstat redo_log fsync latency is zero";

	EXPECT_GE(msnap.sm_creat.ops, 1)
	    << "mountstat summarize creat ops is zero";
	EXPECT_GE(msnap.sm_write_pwrite.ops, 32)
	    << "mountstat summarize write+pwrite ops too low";
}
