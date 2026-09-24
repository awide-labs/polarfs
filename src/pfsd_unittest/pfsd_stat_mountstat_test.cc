#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "pfsd_sdk.h"
#include "pfsd_stat_infra.h"
#include "pfsd_testenv.h"

static const size_t	CHUNK = 4 * 1024;

class MountstatTest : public testing::Test {
protected:
	void SetUp() override {
		const std::string &pbdname = g_testenv->pbdname_;

		std::string datadir = "/" + pbdname + "/data";
		waldir_ = datadir + "/pg_wal";
		pfsd_mkdir(datadir.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
		pfsd_mkdir(waldir_.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);

		file_ = waldir_ + "/mountstat_data.bin";
		renamed_ = waldir_ + "/mountstat_renamed.bin";
		subdir_ = waldir_ + "/mountstat_subdir";
		unlink_ = waldir_ + "/mountstat_unlink_helper.bin";

		ASSERT_TRUE(set_pfsd_config(pbdname, "mountstat_enable", 1)) << "failed to enable mountstat_enable";
	}

	void TearDown() override {
		pfsd_unlink(file_.c_str());
		pfsd_unlink(renamed_.c_str());
		pfsd_unlink(unlink_.c_str());
		pfsd_rmdir(subdir_.c_str());
		pfsd_rmdir(waldir_.c_str());

		set_pfsd_config(g_testenv->pbdname_, "mountstat_enable", 0);
	}

	std::string waldir_;
	std::string file_;
	std::string renamed_;
	std::string subdir_;
	std::string unlink_;
};

static void
do_data_workload(const std::string &path)
{
	std::vector<char> wbuf(CHUNK, 'A');
	std::vector<char> rbuf(CHUNK);

	int fd = pfsd_creat(path.c_str(), 0);
	ASSERT_GE(fd, 0) << strerror(errno);

	for (int i = 0; i < 8; i++) {
		ASSERT_EQ(wbuf.size(), pfsd_write(fd, wbuf.data(), wbuf.size())) << strerror(errno);
	}

	/* SEEK_END reaches pfsd, the SEEK_SET not. */
	ASSERT_EQ(CHUNK * 8, pfsd_lseek(fd, 0, SEEK_END)) << strerror(errno);
	ASSERT_EQ(0, pfsd_lseek(fd, 0, SEEK_SET)) << strerror(errno);

	for (int i = 0; i < 8; i++) {
		ASSERT_EQ(rbuf.size(), pfsd_read(fd, rbuf.data(), rbuf.size())) << strerror(errno);
	}

	ASSERT_EQ(0, pfsd_close(fd)) << strerror(errno);
}

TEST_F(MountstatTest, ReadWriteOps)
{
	std::string	err;
	RunResult	res;

	ASSERT_TRUE(run_io_window(g_testenv->pbdname_,
	    [this]() { do_data_workload(file_); }, false, true, &res, &err)) << err;

	const MountstatData	&ms = res.mountstat;

	expect_row_io(ms.get("redo_log", "pwrite"), "redo_log pwrite", 8, 8 * CHUNK);
	expect_row_min(ms.get_sum("write+pwrite"), "summarize write+pwrite", 8);

	expect_row_io(ms.get("redo_log", "pread"), "redo_log pread", 8, 8 * CHUNK);
	expect_row_min(ms.get_sum("read+pread"), "summarize read+pread", 8);

	expect_row(ms.get("redo_log", "lseek"), "redo_log lseek", 1);
	expect_row_min(ms.get_sum("lseek"), "summarize lseek", 1);

	expect_row_min(ms.get_sum("journal_write"), "summarize journal_write", 1);
	expect_row_min(ms.get_sum("dev_read"), "summarize dev_read", 1);
	expect_row_min(ms.get_sum("dev_write"), "summarize dev_write", 1);
}

static void
do_filestate_workload(const std::string &path)
{
	std::vector<char> wbuf(CHUNK, 'A');

	int fd = pfsd_creat(path.c_str(), 0);
	ASSERT_GE(fd, 0) << strerror(errno);

	int fd2 = pfsd_open(path.c_str(), O_RDWR, 0);
	ASSERT_GE(fd2, 0) << strerror(errno);

	ASSERT_EQ(wbuf.size(), pfsd_write(fd, wbuf.data(), wbuf.size())) << strerror(errno);

	struct stat st;
	ASSERT_EQ(0, pfsd_fstat(fd, &st)) << strerror(errno);
	ASSERT_EQ(0, pfsd_stat(path.c_str(), &st)) << strerror(errno);
	ASSERT_EQ(0, pfsd_posix_fallocate(fd2, 0, CHUNK)) << strerror(errno);
	ASSERT_EQ(0, pfsd_ftruncate(fd, CHUNK)) << strerror(errno);
	ASSERT_EQ(0, pfsd_truncate(path.c_str(), CHUNK / 2)) << strerror(errno);

	ASSERT_EQ(0, pfsd_close(fd2)) << strerror(errno);
	ASSERT_EQ(0, pfsd_close(fd)) << strerror(errno);
}

TEST_F(MountstatTest, FileStateOps)
{
	std::string	err;
	RunResult	res;

	ASSERT_TRUE(run_io_window(g_testenv->pbdname_,
	    [this]() { do_filestate_workload(file_); }, false, true, &res, &err)) << err;

	const MountstatData	&ms = res.mountstat;

	/* creat and open share the per-type open row, but the
	 * Summarize layer counts them apart. */
	expect_row(ms.get("redo_log", "open"), "redo_log open", 2);
	expect_row_min(ms.get_sum("creat"), "summarize creat", 1);
	expect_row_min(ms.get_sum("open"), "summarize open", 1);

	expect_row(ms.get("redo_log", "fstat"), "redo_log fstat", 1);
	expect_row(ms.get("redo_log", "stat"), "redo_log stat", 1);
	expect_row_min(ms.get_sum("stat"), "summarize stat", 2);

	expect_row(ms.get("redo_log", "fallocate"), "redo_log fallocate", 1);
	expect_row_min(ms.get_sum("fallocate"), "summarize fallocate", 1);

	/* ftruncate and truncate: separate per-type rows, one shared
	 * Summarize truncate row. */
	expect_row(ms.get("redo_log", "ftruncate"), "redo_log ftruncate", 1);
	expect_row(ms.get("redo_log", "truncate"), "redo_log truncate", 1);
	expect_row_min(ms.get_sum("truncate"), "summarize truncate", 2);
}

static void
do_namespace_workload(const std::string &waldir, const std::string &file,
    const std::string &renamed, const std::string &subdir, const std::string &unlink)
{
	ASSERT_EQ(0, pfsd_rename(file.c_str(), renamed.c_str())) << strerror(errno);

	DIR *d = pfsd_opendir(waldir.c_str());
	ASSERT_NE(nullptr, d) << strerror(errno);
	for (int scanned = 0; scanned < 64; scanned++) {
		struct dirent de, *res = nullptr;

		ASSERT_EQ(0, pfsd_readdir_r(d, &de, &res)) << strerror(errno);
		if (res == nullptr) {
			ASSERT_GT(scanned, 0) << "pfsd_readdir_r returned no entries";
			break;
		}
	}
	ASSERT_EQ(0, pfsd_closedir(d)) << strerror(errno);

	ASSERT_EQ(0, pfsd_mkdir(subdir.c_str(), S_IRWXU | S_IRWXG | S_IRWXO)) << strerror(errno);
	ASSERT_EQ(0, pfsd_rmdir(subdir.c_str())) << strerror(errno);
	ASSERT_EQ(0, pfsd_unlink(unlink.c_str())) << strerror(errno);
}

TEST_F(MountstatTest, NamespaceOps)
{
	std::string	err;
	RunResult	res;

	/* created before the window: their rows land outside -b t0. */
	int fd = pfsd_creat(file_.c_str(), 0);
	ASSERT_GE(fd, 0) << strerror(errno);
	ASSERT_EQ(0, pfsd_close(fd)) << strerror(errno);
	fd = pfsd_creat(unlink_.c_str(), 0);
	ASSERT_GE(fd, 0) << strerror(errno);
	ASSERT_EQ(0, pfsd_close(fd)) << strerror(errno);

	ASSERT_TRUE(run_io_window(g_testenv->pbdname_,
	    [this]() { do_namespace_workload(waldir_, file_, renamed_, subdir_, unlink_); },
	    false, true, &res, &err)) << err;

	const MountstatData	&ms = res.mountstat;

	expect_row_min(ms.get_sum("rename"), "summarize rename", 1);
	expect_row_min(ms.get_sum("opendir"), "summarize opendir", 1);
	expect_row_min(ms.get_sum("readdir"), "summarize readdir", 1);
	expect_row_min(ms.get_sum("rmdir"), "summarize rmdir", 1);
	expect_row_min(ms.get_sum("unlink"), "summarize unlink", 1);

	expect_no_row(ms.get_sum("mkdir"), "summarize mkdir");
	expect_row(ms.get("redo_log", "unlink"), "redo_log unlink", 1);
	expect_no_row(ms.get("redo_log", "rename"), "redo_log rename");
	expect_no_row(ms.get("redo_log", "rmdir"), "redo_log rmdir");
}

static void
do_sync_workload(const std::string &path)
{
	static const int	NWRITES = 4;

	std::vector<char> wbuf(CHUNK, 'A');

	int fd = pfsd_creat(path.c_str(), 0);
	ASSERT_GE(fd, 0) << strerror(errno);

	for (int i = 0; i < NWRITES; i++) {
		ASSERT_EQ(wbuf.size(), pfsd_write(fd, wbuf.data(), wbuf.size())) << strerror(errno);
	}

	ASSERT_EQ(0, pfsd_fsync(fd)) << strerror(errno);
	ASSERT_EQ(0, pfsd_close(fd)) << strerror(errno);
}

TEST_F(MountstatTest, FsyncOps)
{
	std::string	err;
	RunResult	res;

	ASSERT_TRUE(run_io_window(g_testenv->pbdname_,
	    [this]() { do_sync_workload(file_); }, false, true, &res, &err)) << err;

	const MountstatData	&ms = res.mountstat;

	expect_row(ms.get("redo_log", "fsync"), "redo_log fsync", 1);
	expect_row(ms.get_sum("dev_flush"), "summarize dev_flush", 1);
}
