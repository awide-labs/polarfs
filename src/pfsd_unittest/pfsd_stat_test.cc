/*
 * pfsd_stat_test - verify that devstat (device-level I/O counters)
 * and mountstat (mount-level per-file-type I/O counters) increment
 * when file I/O is performed through the pfsd SDK.
 *
 * The test file is created under /<pbd>/data/pg_wal/ and
 * accounted by mountstat as the "redo_log" file type.
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "pfsd_stat_infra.h"
#include "pfsd_testenv.h"
#include "pfsd_sdk.h"

static const int CHUNK  = 4 * 1024;
static const int NCHUNK = 32;
static const int WORK_BYTES = CHUNK * NCHUNK;
/* iostat rounds byte columns; this covers the error. */
static const int GAP_BYTES = 16 * 1024;

static void
do_file_io(const std::string &pbdpath)
{
	int fd = pfsd_creat(pbdpath.c_str(), 0);
	ASSERT_GE(fd, 0) << strerror(errno);

	std::vector<char> wbuf(CHUNK, 'A');
	for (int i = 0; i < NCHUNK; i++) {
		ASSERT_EQ(wbuf.size(), pfsd_write(fd, wbuf.data(), wbuf.size())) << strerror(errno);
	}

	ASSERT_EQ(0, pfsd_fsync(fd)) << strerror(errno);

	std::vector<char> rbuf(CHUNK);
	ASSERT_EQ(0, pfsd_lseek(fd, 0, SEEK_SET)) << strerror(errno);
	for (int i = 0; i < NCHUNK; i++) {
		ASSERT_EQ(rbuf.size(), pfsd_read(fd, rbuf.data(), rbuf.size())) << strerror(errno);
	}

	ASSERT_EQ(0, pfsd_close(fd)) << strerror(errno);
}

class DevstatTest : public testing::Test {
protected:
	void SetUp() override {
		const std::string &pbdname = g_testenv->pbdname_;

		std::string datadir = "/" + pbdname + "/data";
		std::string waldir = datadir + "/pg_wal";
		pfsd_mkdir(datadir.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
		pfsd_mkdir(waldir.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);

		pbdpath_ = waldir + "/devstat_test.bin";

		ASSERT_TRUE(set_pfsd_config(pbdname, "devstat_enable", 1)) << "failed to enable devstat_enable";
		ASSERT_TRUE(set_pfsd_config(pbdname, "mountstat_enable", 1)) << "failed to enable mountstat_enable";
	}

	void TearDown() override {
		const std::string &pbdname = g_testenv->pbdname_;

		pfsd_unlink(pbdpath_.c_str());
		pfsd_rmdir(("/" + pbdname + "/data/pg_wal").c_str());

		set_pfsd_config(pbdname, "devstat_enable", 0);
		set_pfsd_config(pbdname, "mountstat_enable", 0);
	}

	std::string pbdpath_;
};

TEST_F(DevstatTest, CountersIncrementAfterIO)
{
	std::string err;

	RunResult res;
	ASSERT_TRUE(run_io_window(g_testenv->pbdname_, [this]() { do_file_io(pbdpath_); },
	    true, true, &res, &err)) << err;

	const DevstatDelta &dv = res.devstat;

	EXPECT_EQ(dv.device_name, g_testenv->pbdname_) << "iostat returned data for the wrong device";

	EXPECT_GT(dv.write_ops, 0) << "write ops did not increase (delta=" << dv.write_ops << ")";
	EXPECT_GE(dv.write_bytes, WORK_BYTES - GAP_BYTES)
	    << "write bytes did not increase enough (delta=" << dv.write_bytes << " B)";

	EXPECT_GT(dv.read_ops, 0) << "read ops did not increase (delta=" << dv.read_ops << ")";
	EXPECT_GE(dv.read_bytes, WORK_BYTES - GAP_BYTES)
	    << "read bytes did not increase enough (delta=" << dv.read_bytes << " B)";

	const MountstatData &ms = res.mountstat;

	expect_row(ms.get("redo_log", "open"), "redo_log open", 1);
	expect_row_io(ms.get("redo_log", "pwrite"), "redo_log pwrite", NCHUNK, WORK_BYTES);
	expect_row_io(ms.get("redo_log", "pread"), "redo_log pread", NCHUNK, WORK_BYTES);
	expect_row(ms.get("redo_log", "fsync"), "redo_log fsync", 1);

	expect_row_min(ms.get_sum("creat"), "summarize creat", 1);
	expect_row_min(ms.get_sum("write+pwrite"), "summarize write+pwrite", NCHUNK);
	expect_row_min(ms.get_sum("read+pread"), "summarize read+pread", NCHUNK);
}

