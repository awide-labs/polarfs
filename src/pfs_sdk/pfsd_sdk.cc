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

#include <bit>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

#include "pfsd_common.h"
#include "pfsd_proto.h"
#include "pfsd_sdk_file.h"
#include "pfsd_sdk.h"
#include "pfsd_sdk_mount.h"

#include "ipc/client.h"

/* init once */
static pthread_mutex_t s_init_mtx = PTHREAD_MUTEX_INITIALIZER;
static int s_inited = 0;
static int s_n_custom_mem_pools = 0;

/* connect id */
static int s_connid = -1;

/* about hostid */
static void *s_mount_local_info = NULL;

static char s_pbdname[PFS_MAX_NAMELEN];
static int s_mnt_flags = 0;
static int s_mnt_hostid = -1;
int s_mount_epoch = 0;

static int s_mode = PFSD_SDK_PROCESS;
static char s_svraddr[PFS_MAX_PATHLEN];
static int s_timeout_ms = 20 * 1000;
static int s_remount_timeout_ms = 2000 * 1000;

#define RESET_CONN() do { \
	s_connid = -1;\
	s_inited = 0;\
	s_pbdname[0] = 0;\
	s_mnt_flags = 0;\
	s_mount_epoch = 0;\
	s_mnt_hostid = -1;\
} while (0)

/* Don't check it for multi process.
 * After remount, s_mnt_flags will be modified.
 * But it's not shared.
 */
#define CHECK_WRITABLE() do { \
	if (s_mode == PFSD_SDK_THREADS && !pfsd_writable(s_mnt_flags)) { \
		errno = EROFS; \
		return -1; \
	} \
} while(0)

#define CHECK_MOUNT(pbdname) do { \
	if (strncmp(s_pbdname, pbdname, sizeof(s_pbdname)) != 0) { \
		PFSD_CLIENT_ELOG("No such device %s, exists %s", pbdname, s_pbdname);\
		errno = ENODEV; \
		return -1; \
	} \
} while(0)

void
pfsd_set_mode(int mode)
{
	if (mode == PFSD_SDK_THREADS || mode == PFSD_SDK_PROCESS)
		s_mode = mode;
	else
		PFSD_CLIENT_ELOG("Wrong mode %d, expect 0(threads), 1(processes)", mode);
}

void
pfsd_set_svr_addr(const char *svraddr, size_t len)
{
	if (len >= PFS_MAX_PATHLEN) {
		PFSD_CLIENT_ELOG("Too long path %s", svraddr);
		return;
	}

	strncpy(s_svraddr, svraddr, len);
}

void
pfsd_set_connect_timeout(int timeout_ms)
{
	if (timeout_ms <= 0)
		return;
	if (timeout_ms > 24 * 3600 * 1000)
		return;

	s_timeout_ms = timeout_ms;
}

static ipc::Session *client = nullptr;

static void
pfsd_mount_atfork_child_init()
{
	pfs_mount_atfork_child(s_mount_local_info);
	pfsd_getpid_slow();
}

/* when child process is ready */
void
pfsd_atfork_child_post()
{
	client->atforkChild();

	/* init rand seed for each process */

	struct timeval now;
	gettimeofday(&now, NULL);
	srand((unsigned)((now.tv_sec + now.tv_usec) ^ pfs_getpid()));

	pfsd_sdk_file_init();
	pfsd_mount_atfork_child_init();
}

struct MemPoolParams {
  size_t size;
  size_t capacity;
  size_t numLocalLists;
  size_t localListLimit;
};

struct req_and_buf_info {
	using alloc_result = ipc::SharedMemoryPools::AllocResult;
	ipc::RequestPtr rp;
	alloc_result req_alloc_result;
	alloc_result buf_alloc_result;
	void *buf;
};

static inline int
pfsd_alloc_req_and_buf(req_and_buf_info &r, size_t buflen, ipc::Request **req,
		       void **buf)
{
	client->allocRequest(r.req_alloc_result);

	r.rp.memBufId = r.req_alloc_result.bufferId;
	r.rp.offset = r.req_alloc_result.offset;
	r.rp.size = sizeof(ipc::Request);

	*req = (ipc::Request *)r.req_alloc_result.ptr;

	if (buflen > 0) {
		if (!client->alloc(buflen, r.buf_alloc_result)) {
			client->freeRequest(r.req_alloc_result);
			return ENOMEM;
		}

		(*req)->memBufId = r.buf_alloc_result.bufferId;
		(*req)->offset = r.buf_alloc_result.offset;
		(*req)->size = r.buf_alloc_result.size;

		*buf = r.buf_alloc_result.ptr;
	} else {
		r.buf_alloc_result.ptr = nullptr;
	}

	return 0;
}

static inline void
pfsd_free_req_and_buf(req_and_buf_info &r)
{
	if (r.buf_alloc_result.ptr != nullptr) {
		client->free(r.buf_alloc_result);
	}
	client->freeRequest(r.req_alloc_result);
}

int
pfsd_sdk_init(int mode, const char *svraddr, int timeout_ms,
	      const char *cluster, const char *pbdname, int host_id, int flags)
{
	std::vector<MemPoolParams> defaultMemPoolParams = {
		{ 4, 16384, 128, 8 }, { 8, 8192, 64, 8 },  { 16, 4096, 64, 4 },
		{ 64, 1024, 32, 4 },  { 256, 256, 32, 4 }, { 1024, 64, 8, 2 },
		{ 4096, 16, 8, 2 }
	};

	int conn_id;
	void *mp = NULL;

	if (cluster == NULL)
		cluster = "polarstore";

	pthread_mutex_lock(&s_init_mtx);
	if (s_inited == 1) {
		PFSD_CLIENT_LOG("sdk may be init by other threads");
		pthread_mutex_unlock(&s_init_mtx);
		return 0;
	}

	if (flags & MNTFLG_TOOL) {
		char logfile[1024] = "";
		(void)snprintf(logfile, sizeof(logfile), "/var/log/pfs-%s.log", pbdname);
		int fd = open(logfile, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0666);
		if (fd < 0) {
			fprintf(stderr, "cant open logfile %s\n", logfile);
		} else  {
			if (dup2(fd, STDERR_FILENO) < 0) {
				fprintf(stderr, "cant dup fd %d to stderr\n", fd);
				close(fd);
				fd = -1;
			}
			chmod(logfile, 0666);
			close(fd);
		}
	}

	s_pbdname[0] = '\0';
	s_mnt_flags = 0;
	pfsd_sdk_file_init();

	if (client == nullptr) {
		client = new ipc::Session;
	}
	client->setPbdname(pbdname);

	if (s_svraddr[0] == '\0') {
		strncpy(s_svraddr, PFSD_USER_PID_DIR, PFS_MAX_PATHLEN);
	}

	srand(time(NULL));

	/* local hostid lock */
	errno = 0;
	mp = pfs_mount_prepare(cluster, pbdname, host_id, flags);
	if (mp == NULL && errno != 0) {
		PFSD_CLIENT_ELOG("pfs_mount_prepare failed, maybe hostid %d used, err %s", host_id, strerror(errno));
		goto failed;
	}

	if (s_n_custom_mem_pools == 0) {
		for (auto param : defaultMemPoolParams) {
			auto mempool =
				std::make_unique<ipc::SharedIndexedMemPool >(
					std::string("mempool-") +
						std::to_string(param.size),
					param.size * 1024, param.capacity,
					param.numLocalLists,
					param.localListLimit);
			if (!client->registerMemPool(std::move(mempool),
						     false)) {
				client->shutdown();
				return -1;
			}
		}
	}

	{
		auto mempool = std::make_unique<ipc::SharedIndexedMemPool >(
			std::string("mempool-req"), sizeof(ipc::Request), 2048,
			32, 32);
		mempool->zeroInit();
		if (!client->registerMemPool(std::move(mempool), true)) {
			client->shutdown();
			return -1;
		}
	}

  if (!client->start(cluster, host_id, flags, timeout_ms)) {
	  client->shutdown();
	  return -1;
  }

	strncpy(s_pbdname, pbdname, sizeof(s_pbdname));
	s_mnt_flags = flags;
	s_mnt_hostid = host_id;

	conn_id = client->connectionId();
	s_connid = conn_id;
	s_mount_local_info = mp;

	if (mode == PFSD_SDK_PROCESS) {
		static bool registered_at_fork = false;
		if (!registered_at_fork) {
			pthread_atfork(NULL, NULL, pfsd_atfork_child_post);
			registered_at_fork = true;
		}
	}

	if (mp)
		pfs_mount_post(mp, 0);

	s_inited = 1;
	pthread_mutex_unlock(&s_init_mtx);
	return 0;

failed:
	if (mp)
		pfs_mount_post(mp, -1);

	s_mount_local_info = NULL;
	RESET_CONN();

	pthread_mutex_unlock(&s_init_mtx);
	return -1;
}

int
pfsd_mount(const char *cluster, const char *pbdname, int hostid, int flags)
{
	return pfsd_sdk_init(s_mode, s_svraddr, s_timeout_ms, cluster, pbdname, hostid, flags);
}

int
pfsd_umount_force(const char *pbdname)
{
	PFSD_CLIENT_LOG("pbdname %s", pbdname);
	CHECK_MOUNT(pbdname);

	if (s_mount_local_info)
		pfs_umount_prepare(pbdname, s_mount_local_info);

	bool success = client->shutdown();
	if (success) {
		RESET_CONN();

		if (s_mount_local_info) {
			pfs_umount_post(pbdname, s_mount_local_info);
			s_mount_local_info = NULL;
		}
		PFSD_CLIENT_LOG("umount success for %s", pbdname);
	} else {
		PFSD_CLIENT_ELOG("umount failed for %s", pbdname);
	}

	pfsd_sdk_file_destroy();

	return 0;
}

int
pfsd_umount(const char *pbdname)
{
	PFSD_CLIENT_LOG("pbdname %s", pbdname);
	CHECK_MOUNT(pbdname);

	if (s_mount_local_info)
		pfs_umount_prepare(pbdname, s_mount_local_info);

	bool success = client->shutdown();
	if (success) {
		RESET_CONN();

		if (s_mount_local_info) {
			pfs_umount_post(pbdname, s_mount_local_info);
			s_mount_local_info = NULL;
		}
		PFSD_CLIENT_LOG("umount success for %s", pbdname);
	} else {
		PFSD_CLIENT_ELOG("umount failed for %s", pbdname);
		return -1;
	}

	pfsd_sdk_file_destroy();

	return 0;
}

int
pfsd_remount(const char *cluster, const char *pbdname, int hostid, int flags)
{
	void *mp;
	int res;

	CHECK_MOUNT(pbdname);

	if (hostid != s_mnt_hostid) {
		PFSD_CLIENT_ELOG("pfs_remount with diff hostid %d, expect %d", hostid, s_mnt_hostid);
		errno = EINVAL;
		return -1;
	}

	if (s_mnt_flags & MNTFLG_WR) {
		PFSD_CLIENT_ELOG("pfs_remount no need, already rw mount: %#x", s_mnt_flags);
		errno = EINVAL;
		return -1;
	}

	if (cluster == NULL)
		cluster = "polarstore";

	errno = 0;
	mp = pfs_remount_prepare(cluster, pbdname, hostid, flags);
	if (mp == NULL && errno != 0) {
		PFSD_CLIENT_ELOG("pfs_remount_prepare failed, maybe hostid %d used, err %s", hostid, strerror(errno));
		goto failed;
	}

	if (client->remount(cluster, hostid, flags, s_remount_timeout_ms) == 0) {
		s_mnt_flags = flags;
		free(s_mount_local_info);
		s_mount_local_info = mp;
	} else {
		goto failed;
	}

	if (mp)
		pfs_remount_post(mp, 0);

	return 0;

failed:
	if (mp)
		pfs_remount_post(mp, -1);

	return -1;
}

int
pfsd_mount_growfs(const char *pbdname)
{
	CHECK_MOUNT(pbdname);

	int err = 0;

	req_and_buf_info r;
	ipc::Request *req;

	if ((err = pfsd_alloc_req_and_buf(r, 0, &req, nullptr)) != 0) {
		errno = err;
		return -1;
	}

	/* fill request */
	req->type = PFSD_REQUEST_GROWFS;
	strncpy(req->req.g_req.g_pbd, pbdname, PFS_MAX_PBDLEN);

	client->executeRequest(r.rp, req);

	if (req->rsp.g_rsp.error != 0) {
		errno = req->rsp.g_rsp.error;
		err = -1;
	}

	pfsd_free_req_and_buf(r);

	return err;
}

int
pfsd_rename(const char *oldpbdpath, const char *newpbdpath)
{
	if (oldpbdpath == NULL || newpbdpath == NULL) {
		errno = EINVAL;
		PFSD_CLIENT_ELOG("NULL args");
		return -1;
	}

	char oldpath[PFS_MAX_PATHLEN], newpath[PFS_MAX_PATHLEN];

	oldpbdpath = pfsd_name_init(oldpbdpath, oldpath, sizeof oldpath);
	if (oldpbdpath == NULL) {
		PFSD_CLIENT_ELOG("wrong oldpbdpath %s", oldpbdpath);
		return -1;
	}

	newpbdpath = pfsd_name_init(newpbdpath, newpath, sizeof newpath);
	if (newpbdpath == NULL) {
		PFSD_CLIENT_ELOG("wrong newpbdpath %s", oldpbdpath);
		return -1;
	}

	int err = 0;

	char oldpbd[PFS_MAX_NAMELEN];
	char newpbd[PFS_MAX_NAMELEN];
	if (pfsd_sdk_pbdname(oldpbdpath, oldpbd) != 0 ||
		pfsd_sdk_pbdname(newpbdpath, newpbd) != 0) {
		PFSD_CLIENT_ELOG("wrong pbdpath:  old %s, new %s", oldpbdpath, newpbdpath);
		errno = EINVAL;
		return -1;
	}

	/* Don't support rename between different PBD */
	if (strncmp(oldpbd, newpbd, PFS_MAX_NAMELEN) != 0) {
		PFSD_CLIENT_ELOG("Rename must in same pbd: [%s] != [%s]", oldpbd, newpbd);
		errno = EXDEV;
		return -1;
	}

	CHECK_MOUNT(newpbd);
	CHECK_WRITABLE();

	req_and_buf_info r;
	ipc::Request *req;
	char *buf;

	if ((err = pfsd_alloc_req_and_buf(r, 2 * PFS_MAX_PATHLEN, &req,
					  (void **)&buf)) != 0) {
		errno = err;
		return -1;
	}

	/* fill request */
	req->type = PFSD_REQUEST_RENAME;
	/* copy pbdpath to iobuf */
	strncpy((char*)buf, oldpath, PFS_MAX_PATHLEN);
	strncpy((char*)buf+PFS_MAX_PATHLEN, newpath, PFS_MAX_PATHLEN);

	client->executeRequest(r.rp, req);

	if (req->rsp.re_rsp.error != 0) {
		PFSD_CLIENT_ELOG("rename %s -> %s error: %d", oldpbdpath, newpbdpath, req->rsp.re_rsp.error);
		errno = req->rsp.re_rsp.error;
		err = -1;
	}

	pfsd_free_req_and_buf(r);

	return err;
}

int
pfsd_open(const char *pbdpath, int flags, mode_t mode)
{
	char abspath[PFS_MAX_PATHLEN];
	pbdpath = pfsd_name_init(pbdpath, abspath, sizeof abspath);
	if (pbdpath == NULL)
		return -1;

	char pbdname[PFS_MAX_NAMELEN];
	if (pfsd_sdk_pbdname(pbdpath, pbdname) != 0) {
		errno = EINVAL;
		return -1;
	}

	CHECK_MOUNT(pbdname);

	if (flags & (O_CREAT | O_TRUNC)) {
		CHECK_WRITABLE();
	}

	pfsd_file_t *file = pfsd_alloc_file();
	if (file == NULL) {
		errno = ENOMEM;
		return -1;
	}

	int fd = pfsd_alloc_fd(file);
	if (fd == -1) {
		errno = EMFILE;
		pfsd_free_file(file);
		return -1;
	}
	file->f_flags = flags;

	int err;
	req_and_buf_info r;
	ipc::Request *req;
	char *buf;

	if ((err = pfsd_alloc_req_and_buf(r, PFS_MAX_PATHLEN, &req,
					  (void **)&buf)) != 0) {
		errno = err;
		pfsd_free_file(file);
		return -1;
	}

	/* fill request */
	req->type = PFSD_REQUEST_OPEN;
	req->req.o_req.o_flags = flags;
	req->req.o_req.o_mode = mode;
	/* copy pbdpath to iobuf */
	strncpy((char*)buf, pbdpath, PFS_MAX_PATHLEN);

	client->executeRequest(r.rp, req);

	file->f_inode = req->rsp.o_rsp.o_ino;
	file->f_common_pl = req->rsp.o_rsp.common_pl_rsp;
	if (file->f_inode == -1) {
		pfsd_close_file(file);
		errno = req->rsp.o_rsp.error;
		fd = -1;
		if (errno != ENOENT)
			PFSD_CLIENT_ELOG("open %s failed %s", pbdpath,
			    strerror(errno));
	} else {
		file->f_offset = req->rsp.o_rsp.o_off;

		if (flags & O_CREAT)
			PFSD_CLIENT_LOG("open %s with inode %ld, fd %d",
			    pbdpath, file->f_inode, fd);
	}

	pfsd_free_req_and_buf(r);

	if (fd < 0)
		return -1;

	return PFSD_FD_MAKE(fd);
}

int
pfsd_creat(const char *pbdpath, mode_t mode)
{
	return pfsd_open(pbdpath, O_CREAT | O_TRUNC | O_WRONLY, mode);
}

#define PFSD_SDK_GET_FILE(fd) do {\
	if (!PFSD_FD_ISVALID(fd)) {\
		errno = EBADF; \
		return -1; \
	}\
	fd = PFSD_FD_RAW(fd); \
	file = pfsd_get_file(fd, false); \
	if (file == NULL) { \
		PFSD_CLIENT_ELOG("bad fd %d", fd);\
		errno = EBADF; \
		return -1; \
	} \
} while(0)

#define PFSD_SDK_GET_FILE_WR(fd) do {\
	if (!PFSD_FD_ISVALID(fd)) {\
		errno = EBADF; \
		return -1; \
	}\
	fd = PFSD_FD_RAW(fd); \
	file = pfsd_get_file(fd, true); \
	if (file == NULL) { \
		PFSD_CLIENT_ELOG("bad fd %d", fd);\
		errno = EBADF; \
		return -1; \
	} \
} while(0)


#define OFFSET_FILE_POS     (-1)    /* offset is current file position */
#define OFFSET_FILE_SIZE    (-2)    /* offset is file size */

ssize_t
pfsd_read(int fd, void *buf, size_t len)
{
	return pfsd_pread(fd, buf, len, OFFSET_FILE_POS);
}

ssize_t
pfsd_pread(int fd, void *buf, size_t len, off_t off)
{
	if (buf == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (len > PFSD_MAX_IOSIZE) {
		/* may shorten read */
		PFSD_CLIENT_LOG("pread len %lu is too big for fd %d, cast to 4MB.", len, fd);
		len = PFSD_MAX_IOSIZE;
	}

	pfsd_file_t *file = NULL;

	PFSD_SDK_GET_FILE(fd);

	off_t off2 = off;
	if (off == OFFSET_FILE_POS)
		off2 = file->f_offset;

	if (off2 < 0) {
		errno = EINVAL;
		pfsd_put_file(file);
		return -1;
	}

	int err;
	req_and_buf_info r;
	ipc::Request *req;
	char *rbuf = nullptr;	/* only used when r_len > 0, set on buflen > 0 */

	if ((err = pfsd_alloc_req_and_buf(r, len, &req, (void **)&rbuf)) != 0) {
		errno = err;
		pfsd_put_file(file);
		return -1;
	}

	/* fill request */
	req->type = PFSD_REQUEST_READ;
	req->req.r_req.r_ino = file->f_inode;
	req->req.r_req.r_len = len;
	req->req.r_req.r_off = off2;
	req->req.r_req.common_pl_req = file->f_common_pl;

	client->executeRequest(r.rp, req);

	if (req->rsp.r_rsp.r_len > 0)
		memcpy(buf, rbuf, req->rsp.r_rsp.r_len);

	ssize_t ss = req->rsp.r_rsp.r_len;
	if (ss < 0) {
		errno = req->rsp.r_rsp.error;
		PFSD_CLIENT_ELOG("pread fd %d ino %ld error: %s", fd,
		    file->f_inode, strerror(errno));
	} else {
		if (off == -1)
			__sync_add_and_fetch(&file->f_offset, ss);
	}

	pfsd_put_file(file);

	pfsd_free_req_and_buf(r);

	return ss;
}

ssize_t
pfsd_write(int fd, const void *buf, size_t len)
{
	return pfsd_pwrite(fd, buf, len, OFFSET_FILE_POS);
}

ssize_t
pfsd_pwrite(int fd, const void *buf, size_t len, off_t off)
{
	if (buf == NULL) {
		errno = EINVAL;
		return -1;
	}

	pfsd_file_t *file = NULL;

	CHECK_WRITABLE();
	PFSD_SDK_GET_FILE(fd);

	if (len == 0) {
		pfsd_put_file(file);
		return 0;
	}

	if (len > PFSD_MAX_IOSIZE) {
		PFSD_CLIENT_ELOG("pwrite len %lu is too big for fd %d.", len, fd);
		errno = EFBIG;
		pfsd_put_file(file);
		return -1;
	}

	off_t off2 = off;
	if (file->f_flags & O_APPEND)
		off2 = OFFSET_FILE_SIZE;
	else if (off == OFFSET_FILE_POS)
		off2 = file->f_offset;

	if (off2 < 0 && off2 != OFFSET_FILE_SIZE) {
		PFSD_CLIENT_ELOG("pwrite wrong off2 %lu for fd %d.", off2, fd);
		pfsd_put_file(file);
		errno = EINVAL;
		return -1;
	}

	int err;
	req_and_buf_info r;
	ipc::Request *req;
	char *wbuf;

	if ((err = pfsd_alloc_req_and_buf(r, len, &req, (void **)&wbuf)) != 0) {
		errno = err;
		pfsd_put_file(file);
		return -1;
	}

	/* fill request */
	req->type = PFSD_REQUEST_WRITE;
	req->req.w_req.w_ino = file->f_inode;
	req->req.w_req.w_len = len;
	req->req.w_req.w_off = off2;
	req->req.w_req.w_flags = file->f_flags;
	req->req.w_req.common_pl_req = file->f_common_pl;

	memcpy(wbuf, buf, len);

	client->executeRequest(r.rp, req);

	ssize_t ss = req->rsp.w_rsp.w_len;
	if (ss < 0) {
		errno = req->rsp.w_rsp.error;
		PFSD_CLIENT_ELOG("pwrite fd %d ino %ld error: %s", fd,
		    file->f_inode, strerror(errno));
	} else {
		if (ss >= 0 && off == -1) {
			__sync_add_and_fetch(&file->f_offset, ss);
		}
		if ((file->f_flags & O_APPEND) != 0 && OFFSET_FILE_POS == off)
			file->f_offset = req->rsp.w_rsp.w_file_size;
	}

	pfsd_put_file(file);

	pfsd_free_req_and_buf(r);

	return ss;
}

/*
 * True if the connected daemon advertised zero-copy support in its handshake.
 * A pre-capability daemon reports no caps, so this is false and the zc entry
 * points below fail fast with ENOTSUP.
 */
static bool
pfsd_zc_supported()
{
	return client != nullptr &&
	    (client->serverCaps() & ipc::CAP_ZEROCOPY) != 0;
}

ssize_t pfsd_pread_zc(int fd, uint64_t buf_id, off_t buf_offs, size_t len, off_t off)
{
	if (!pfsd_zc_supported()) {
		PFSD_CLIENT_ELOG("daemon has no zero-copy support");
		errno = ENOTSUP;
		return -1;
	}

	if (len > PFSD_MAX_IOSIZE) {
		/* may shorten read */
		PFSD_CLIENT_LOG("pread len %lu is too big for fd %d, cast to 4MB.", len, fd);
		len = PFSD_MAX_IOSIZE;
	}

	pfsd_file_t *file = NULL;

	PFSD_SDK_GET_FILE(fd);

	off_t off2 = off;
	if (off == OFFSET_FILE_POS)
		off2 = file->f_offset;

	if (off2 < 0) {
		errno = EINVAL;
		pfsd_put_file(file);
		return -1;
	}

	int err;
	req_and_buf_info r;
	ipc::Request *req;

	if ((err = pfsd_alloc_req_and_buf(r, 0, &req, nullptr)) != 0) {
		errno = err;
		pfsd_put_file(file);
		return -1;
	}

	req->memBufId = buf_id;
	req->offset = buf_offs;
	req->size = len;

	/* fill request */
	req->type = PFSD_REQUEST_READ;
	req->req.r_req.r_ino = file->f_inode;
	req->req.r_req.r_len = len;
	req->req.r_req.r_off = off2;
	req->req.r_req.common_pl_req = file->f_common_pl;

	client->executeRequest(r.rp, req);

	ssize_t ss = req->rsp.r_rsp.r_len;
	if (ss < 0) {
		errno = req->rsp.r_rsp.error;
		PFSD_CLIENT_ELOG("pread fd %d ino %ld error: %s", fd,
		    file->f_inode, strerror(errno));
	} else {
		if (off == -1)
			__sync_add_and_fetch(&file->f_offset, ss);
	}

	pfsd_put_file(file);

	pfsd_free_req_and_buf(r);

	return ss;
}

ssize_t pfsd_pwrite_zc(int fd, uint64_t buf_id, off_t buf_offs, size_t len, off_t off)
{
	pfsd_file_t *file = NULL;

	if (!pfsd_zc_supported()) {
		PFSD_CLIENT_ELOG("daemon has no zero-copy support");
		errno = ENOTSUP;
		return -1;
	}

	CHECK_WRITABLE();
	PFSD_SDK_GET_FILE(fd);

	if (len == 0) {
		pfsd_put_file(file);
		return 0;
	}

	if (len > PFSD_MAX_IOSIZE) {
		PFSD_CLIENT_ELOG("pwrite len %lu is too big for fd %d.", len, fd);
		errno = EFBIG;
		pfsd_put_file(file);
		return -1;
	}

	off_t off2 = off;
	if (file->f_flags & O_APPEND)
		off2 = OFFSET_FILE_SIZE;
	else if (off == OFFSET_FILE_POS)
		off2 = file->f_offset;

	if (off2 < 0 && off2 != OFFSET_FILE_SIZE) {
		PFSD_CLIENT_ELOG("pwrite wrong off2 %lu for fd %d.", off2, fd);
		pfsd_put_file(file);
		errno = EINVAL;
		return -1;
	}

	int err;
	req_and_buf_info r;
	ipc::Request *req;

	if ((err = pfsd_alloc_req_and_buf(r, 0, &req, nullptr)) != 0) {
		errno = err;
		pfsd_put_file(file);
		return -1;
	}

	req->memBufId = buf_id;
	req->offset = buf_offs;
	req->size = len;

	/* fill request */
	req->type = PFSD_REQUEST_WRITE;
	req->req.w_req.w_ino = file->f_inode;
	req->req.w_req.w_len = len;
	req->req.w_req.w_off = off2;
	req->req.w_req.w_flags = file->f_flags;
	req->req.w_req.common_pl_req = file->f_common_pl;

	client->executeRequest(r.rp, req);

	ssize_t ss = req->rsp.w_rsp.w_len;
	if (ss < 0) {
		errno = req->rsp.w_rsp.error;
		PFSD_CLIENT_ELOG("pwrite fd %d ino %ld error: %s", fd,
		    file->f_inode, strerror(errno));
	} else {
		if (ss >= 0 && off == -1) {
			__sync_add_and_fetch(&file->f_offset, ss);
		}
		if ((file->f_flags & O_APPEND) != 0 && OFFSET_FILE_POS == off)
			file->f_offset = req->rsp.w_rsp.w_file_size;
	}

	pfsd_put_file(file);

	pfsd_free_req_and_buf(r);

	return ss;
}

int
pfsd_posix_fallocate(int fd, off_t offset, off_t len)
{
	return pfsd_fallocate(fd, 0, offset, len);
}

#define FALLOC_PFSFL_FIXED_OFFSET   0x0100  /* lower bits defined in falloc.h */

int
pfsd_fallocate(int fd, int mode, off_t offset, off_t len)
{
	if (fd < 0 || offset < 0 || len <= 0) {
		errno = (fd < 0) ? EBADF : EINVAL;
		return -1;
	}

	CHECK_WRITABLE();

	pfsd_file_t *file = NULL;
	PFSD_SDK_GET_FILE(fd);

	int rv = -1;

	int err;
	req_and_buf_info r;
	ipc::Request *req;

	if ((err = pfsd_alloc_req_and_buf(r, 0, &req, nullptr)) != 0) {
		errno = err;
		pfsd_put_file(file);
		return -1;
	}

	PFSD_CLIENT_LOG("fallocate ino %ld off %ld len %ld", file->f_inode, offset, len);
	/* fill request */
	req->type = PFSD_REQUEST_FALLOCATE;
	req->req.fa_req.f_ino = file->f_inode;
	req->req.fa_req.f_len = len;
	req->req.fa_req.f_off = offset;
	req->req.fa_req.f_mode = mode;
	req->req.fa_req.common_pl_req = file->f_common_pl;

	client->executeRequest(r.rp, req);

	rv = req->rsp.fa_rsp.f_res;
	if (rv != 0) {
		errno = req->rsp.fa_rsp.error;
		PFSD_CLIENT_ELOG("fallocate ino %ld error: %s", file->f_inode, strerror(errno));
	}

	pfsd_put_file(file);

	pfsd_free_req_and_buf(r);

	return rv;
}

int
pfsd_truncate(const char *pbdpath, off_t len)
{
	if (!pbdpath || len < 0) {
		errno = EINVAL;
		return -1;
	}

	char abspath[PFS_MAX_PATHLEN];
	pbdpath = pfsd_name_init(pbdpath, abspath, sizeof abspath);
	if (pbdpath == NULL)
		return -1;

	char pbdname[PFS_MAX_NAMELEN];
	if (pfsd_sdk_pbdname(pbdpath, pbdname) != 0) {
		errno = EINVAL;
		return -1;
	}

	CHECK_MOUNT(pbdname);
	CHECK_WRITABLE();

	int err;
	req_and_buf_info r;
	ipc::Request *req;
	char *buf;

	if ((err = pfsd_alloc_req_and_buf(r, PFS_MAX_PATHLEN, &req,
					  (void **)&buf)) != 0) {
		errno = err;
		return -1;
	}

	int rv = -1;

	PFSD_CLIENT_LOG("truncate %s len %ld", pbdpath, len);

	/* fill request */
	req->type = PFSD_REQUEST_TRUNCATE;
	req->req.t_req.t_len = len;
	/* copy pbdpath to iobuf */
	strncpy((char*)buf, pbdpath, PFS_MAX_PATHLEN);

	client->executeRequest(r.rp, req);

	rv = req->rsp.t_rsp.t_res;
	if (rv != 0) {
		errno = req->rsp.t_rsp.error;
		PFSD_CLIENT_ELOG("truncate %s len %ld error: %s", pbdpath, len, strerror(errno));
	}

	pfsd_free_req_and_buf(r);

	return rv;
}

int
pfsd_ftruncate(int fd, off_t len)
{
	if (fd < 0 || len < 0) {
		errno = (fd < 0) ? EBADF : EINVAL;
		return -1;
	}

	CHECK_WRITABLE();

	pfsd_file_t *file = NULL;
	PFSD_SDK_GET_FILE(fd);

	int err;
	req_and_buf_info r;
	ipc::Request *req;
	char *buf;

	if ((err = pfsd_alloc_req_and_buf(r, PFS_MAX_PATHLEN, &req,
					  (void **)&buf)) != 0) {
		errno = err;
		pfsd_put_file(file);
		return -1;
	}

	int rv = -1;

	PFSD_CLIENT_LOG("ftruncate ino %ld, len %lu", file->f_inode, len);

	/* fill request */
	req->type = PFSD_REQUEST_FTRUNCATE;
	req->req.ft_req.f_ino = file->f_inode;
	req->req.ft_req.f_len = len;
	req->req.ft_req.common_pl_req = file->f_common_pl;

	client->executeRequest(r.rp, req);

	rv = req->rsp.ft_rsp.f_res;
	if (rv != 0) {
		errno = req->rsp.ft_rsp.error;
		PFSD_CLIENT_ELOG("ftruncate ino %ld, len %lu: %s", file->f_inode, len, strerror(errno));
	}

	pfsd_put_file(file);

	pfsd_free_req_and_buf(r);

	return rv;
}

int
pfsd_unlink(const char *pbdpath)
{
	char abspath[PFS_MAX_PATHLEN];
	pbdpath = pfsd_name_init(pbdpath, abspath, sizeof abspath);
	if (pbdpath == NULL)
		return -1;

	char pbdname[PFS_MAX_NAMELEN];
	if (pfsd_sdk_pbdname(pbdpath, pbdname) != 0) {
		errno = EINVAL;
		return -1;
	}

	CHECK_MOUNT(pbdname);
	/* check writable */
	CHECK_WRITABLE();

	int err;
	req_and_buf_info r;
	ipc::Request *req;
	char *buf;

	if ((err = pfsd_alloc_req_and_buf(r, PFS_MAX_PATHLEN, &req,
					  (void **)&buf)) != 0) {
		errno = err;
		return -1;
	}

	int rv = -1;

	PFSD_CLIENT_LOG("unlink %s", pbdpath);
	/* fill request */
	req->type = PFSD_REQUEST_UNLINK;
	/* copy pbdpath to iobuf */
	strncpy((char*)buf, pbdpath, PFS_MAX_PATHLEN);

	client->executeRequest(r.rp, req);

	rv = req->rsp.un_rsp.u_res;
	if (rv != 0) {
		errno = req->rsp.un_rsp.error;
		if (errno != ENOENT)
			PFSD_CLIENT_ELOG("unlink %s: %s", pbdpath, strerror(errno));
	}

	pfsd_free_req_and_buf(r);

	return rv;
}

int
pfsd_stat(const char *pbdpath, struct stat *st)
{
	if (!pbdpath || !st) {
		errno = EINVAL;
		return -1;
	}

	char abspath[PFS_MAX_PATHLEN];
	pbdpath = pfsd_name_init(pbdpath, abspath, sizeof abspath);
	if (pbdpath == NULL)
		return -1;

	int err;
	req_and_buf_info r;
	ipc::Request *req;
	char *buf;

	if ((err = pfsd_alloc_req_and_buf(r, PFS_MAX_PATHLEN, &req,
					  (void **)&buf)) != 0) {
		errno = err;
		return -1;
	}

	int rv = -1;

	/* fill request */
	req->type = PFSD_REQUEST_STAT;
	/* copy pbdpath to iobuf */
	strncpy((char*)buf, pbdpath, PFS_MAX_PATHLEN);

	client->executeRequest(r.rp, req);

	rv = req->rsp.s_rsp.s_res;
	if (rv != 0) {
		errno = req->rsp.r_rsp.error;
		if (errno != ENOENT)
			PFSD_CLIENT_ELOG("stat %s: %s", pbdpath, strerror(errno));
	} else {
		memcpy(st, &req->rsp.s_rsp.s_st, sizeof(*st));
	}

	pfsd_free_req_and_buf(r);

	return rv;
}

int
pfsd_fstat(int fd, struct stat *st)
{
	if (fd < 0 || !st) {
		errno = (fd < 0) ? EBADF : EINVAL;
		return -1;
	}

	pfsd_file_t *file = NULL;
	PFSD_SDK_GET_FILE(fd);

	int err;
	req_and_buf_info r;
	ipc::Request *req;

	if ((err = pfsd_alloc_req_and_buf(r, 0, &req, nullptr)) != 0) {
		errno = err;
		return -1;
	}

	int rv = -1;

	/* fill request */
	req->type = PFSD_REQUEST_FSTAT;
	req->req.f_req.f_ino = file->f_inode;
	req->req.f_req.common_pl_req = file->f_common_pl;

	client->executeRequest(r.rp, req);

	rv = req->rsp.f_rsp.f_res;
	if (rv != 0) {
		errno = req->rsp.f_rsp.error;
		PFSD_CLIENT_ELOG("fstat %ld error: %s", file->f_inode, strerror(errno));
	} else {
		memcpy(st, &req->rsp.f_rsp.f_st, sizeof(*st));
	}

	pfsd_put_file(file);

	pfsd_free_req_and_buf(r);

	return rv;
}

static off_t
local_file_lseek(pfsd_file_t *file, off_t offset, int whence)
{
	off_t old_offset = file->f_offset;
	off_t new_offset;

	switch (whence) {
		case SEEK_SET:
			new_offset = offset;
			break;

		case SEEK_CUR:
			/* This built-in is fully supported in GCC & Clang */
			if (__builtin_add_overflow(old_offset, offset, &new_offset)) {
				errno = EOVERFLOW;
				return (off_t)-1;
			}
			break;

		case SEEK_END:
			errno = 0;
			return off_t(-1);

		default:
			errno = EINVAL;
			return off_t(-1);
	}

	if (new_offset < 0) {
		errno = EINVAL;
		return off_t(-1);
	} else {
		file->f_offset = new_offset;
		return file->f_offset;
	}
}

off_t
pfsd_lseek(int fd, off_t offset, int whence)
{
	if (fd < 0) {
		errno = EINVAL;
		return -1;
	}

	pfsd_file_t *file = NULL;
	PFSD_SDK_GET_FILE_WR(fd);

	off_t rv = -1;
	rv = local_file_lseek(file, offset, whence);
	if (rv >= 0) {
		pfsd_put_file(file);
		return rv;
	}
	if (rv == off_t(-1) && errno != 0) {
		pfsd_put_file(file);
		return rv;
	}

	int err;
	req_and_buf_info r;
	ipc::Request *req;

	if ((err = pfsd_alloc_req_and_buf(r, 0, &req, nullptr)) != 0) {
		errno = err;
		pfsd_put_file(file);
		return -1;
	}

	/* fill request */
	req->type = PFSD_REQUEST_LSEEK;
	req->req.l_req.l_ino = file->f_inode;
	req->req.l_req.l_offset = offset;
	req->req.l_req.l_whence = whence;
	req->req.l_req.common_pl_req = file->f_common_pl;
	assert (whence == SEEK_END); /* must be SEED_END */

	client->executeRequest(r.rp, req);

	if (req->rsp.l_rsp.l_offset < 0) {
		errno = req->rsp.l_rsp.error;
		rv = off_t(-1);
		PFSD_CLIENT_ELOG("lseek %ld off %ld error: %s", file->f_inode,
		    offset, strerror(errno));
	} else {
		file->f_offset = req->rsp.l_rsp.l_offset;
		rv = req->rsp.l_rsp.l_offset;
	}

	pfsd_put_file(file);

	pfsd_free_req_and_buf(r);

	return rv;
}

int
pfsd_close(int fd)
{
	pfsd_file_t *file = NULL;
	int err = -EAGAIN;
	bool fdok = PFSD_FD_ISVALID(fd);
	if (!fdok)
		err = -EBADF;

	fd = PFSD_FD_RAW(fd);

	while (err == -EAGAIN){
		file = pfsd_get_file(fd, true);
		if (file == NULL) {
			err = -EBADF;
			break;
		}

		err = pfsd_close_file(file);
		if (err != 0) {
			PFSD_CLIENT_ELOG("close fd %d failed, err:%d", fd, err);
			pfsd_put_file(file);
		}
	}
	if (err < 0) {
		errno = -err;
		return -1;
	}
	return 0;
}

int
pfsd_chdir(const char *pbdpath)
{
	char abspath[PFS_MAX_PATHLEN];
	pbdpath = pfsd_name_init(pbdpath, abspath, sizeof abspath);
	if (pbdpath == NULL)
		return -1;

	assert (pbdpath == abspath);

	if (!pfsd_chdir_begin())
		return -1;

	int err;
	req_and_buf_info r;
	ipc::Request *req;
	char *buf;

	if ((err = pfsd_alloc_req_and_buf(r, PFS_MAX_PATHLEN, &req,
					  (void **)&buf)) != 0) {
		errno = err;
		return -1;
	}

	int rv = -1;

	/* fill request */
	req->type = PFSD_REQUEST_CHDIR;
	/* copy pbdpath to iobuf */
	strncpy((char*)buf, pbdpath, PFS_MAX_PATHLEN);

	client->executeRequest(r.rp, req);

	rv = req->rsp.cd_rsp.c_res;
	if (rv != 0) {
		errno = req->rsp.cd_rsp.error;
		PFSD_CLIENT_ELOG("chdir %s error: %s", pbdpath, strerror(errno));
	} else {
		int err = pfsd_normalize_path(abspath);
		if (err == 0) {
			err = pfsd_dir_xsetwd(abspath, strlen(abspath));
		}
		if (err != 0) {
			errno = err;
			rv = -1;
		}
	}

	pfsd_chdir_end();

	pfsd_free_req_and_buf(r);

	return rv;
}

char *
pfsd_getwd(char *buf)
{
	return pfsd_getcwd(buf, PFS_MAX_PATHLEN);
}

char *
pfsd_getcwd(char *buf, size_t size)
{
	int err = -EAGAIN;

	if (!buf)
		err = -EINVAL;

	while (err == -EAGAIN) {
		err = pfsd_dir_xgetwd(buf, size);
	}

	if (err < 0) {
		errno = -err;
		PFSD_CLIENT_ELOG("getcwd error: %s", strerror(errno));
		return NULL;
	}

	return buf;
}

int
pfsd_mkdir(const char *pbdpath, mode_t mode)
{
	char abspath[PFS_MAX_PATHLEN];
	pbdpath = pfsd_name_init(pbdpath, abspath, sizeof abspath);
	if (pbdpath == NULL)
		return -1;

	char pbdname[PFS_MAX_NAMELEN];
	if (pfsd_sdk_pbdname(pbdpath, pbdname) != 0) {
		errno = EINVAL;
		return -1;
	}

	CHECK_MOUNT(pbdname);
	CHECK_WRITABLE();

	int err;
	req_and_buf_info r;
	ipc::Request *req;
	char *buf;

	if ((err = pfsd_alloc_req_and_buf(r, PFS_MAX_PATHLEN, &req,
					  (void **)&buf)) != 0) {
		errno = err;
		return -1;
	}

	PFSD_CLIENT_LOG("mkdir %s", pbdpath);
	/* fill request */
	req->type = PFSD_REQUEST_MKDIR;
	/* copy pbdpath to iobuf */
	strncpy((char*)buf, pbdpath, PFS_MAX_PATHLEN);

	client->executeRequest(r.rp, req);

	if (req->rsp.mk_rsp.m_res != 0) {
		err = -1;
		errno = req->rsp.mk_rsp.error;
		PFSD_CLIENT_ELOG("mkdir %s error: %s", pbdpath, strerror(errno));
	}

	pfsd_free_req_and_buf(r);

	return err;
}

int
pfsd_rmdir(const char *pbdpath)
{
	char abspath[PFS_MAX_PATHLEN];
	pbdpath = pfsd_name_init(pbdpath, abspath, sizeof abspath);
	if (pbdpath == NULL)
		return -1;

	char pbdname[PFS_MAX_NAMELEN];
	if (pfsd_sdk_pbdname(pbdpath, pbdname) != 0) {
		errno = EINVAL;
		return -1;
	}

	CHECK_MOUNT(pbdname);
	CHECK_WRITABLE();

	int err;
	req_and_buf_info r;
	ipc::Request *req;
	char *buf;

	if ((err = pfsd_alloc_req_and_buf(r, PFS_MAX_PATHLEN, &req,
					  (void **)&buf)) != 0) {
		errno = err;
		return -1;
	}

	PFSD_CLIENT_LOG("rmdir %s", pbdpath);
	/* fill request */
	req->type = PFSD_REQUEST_RMDIR;
	/* copy pbdpath to iobuf */
	strncpy((char*)buf, pbdpath, PFS_MAX_PATHLEN);

	client->executeRequest(r.rp, req);

	if (req->rsp.rm_rsp.r_res != 0) {
		err = -1;
		errno = req->rsp.rm_rsp.error;
		PFSD_CLIENT_ELOG("rmdir %s error: %s", pbdpath, strerror(errno));
	}

	pfsd_free_req_and_buf(r);

	return err;
}

DIR *
pfsd_opendir(const char *pbdpath)
{
	char abspath[PFS_MAX_PATHLEN];
	pbdpath = pfsd_name_init(pbdpath, abspath, sizeof abspath);
	if (pbdpath == NULL)
		return NULL;

	char pbdname[PFS_MAX_NAMELEN];
	if (pfsd_sdk_pbdname(pbdpath, pbdname) != 0) {
		errno = EINVAL;
		return NULL;
	}

	if (strncmp(s_pbdname, pbdname, sizeof(s_pbdname)) != 0) {
		PFSD_CLIENT_ELOG("No such device %s, exists %s", pbdname,
		    s_pbdname);
		errno = ENODEV;
		return NULL;
	}

	int err;
	req_and_buf_info r;
	ipc::Request *req;
	char *buf;

	if ((err = pfsd_alloc_req_and_buf(r, PFS_MAX_PATHLEN, &req,
					  (void **)&buf)) != 0) {
		errno = err;
		return NULL;
	}

	pfsd_dirstream_t *dir = NULL;

	/* fill request */
	req->type = PFSD_REQUEST_OPENDIR;
	/* copy pbdpath to iobuf */
	strncpy((char*)buf, pbdpath, PFS_MAX_PATHLEN);

	client->executeRequest(r.rp, req);

	if (req->rsp.od_rsp.o_res != 0) {
		dir = NULL;
		errno = req->rsp.od_rsp.error;
		PFSD_CLIENT_ELOG("opendir %s error: %s", pbdpath, strerror(errno));
	} else {
		dir = PFSD_MALLOC(pfsd_dirstream_t);
		if (dir == NULL) {
			errno = ENOMEM;
		} else {
			memset(dir, 0, sizeof(*dir));
			dir->d_ino = req->rsp.od_rsp.o_dino;
			dir->d_next_ino = req->rsp.od_rsp.o_first_ino;
		}
	}

	pfsd_free_req_and_buf(r);

	if (dir == NULL)
		return NULL;

	return PFSD_DIR_MAKE(dir);
}

struct dirent *
pfsd_readdir(DIR *dir)
{
	if (!PFSD_DIR_ISVALID(dir)) {
		errno = EINVAL;
		return NULL;
	}

	pfsd_dirstream_t *raw_dir = (pfsd_dirstream_t *)PFSD_DIR_RAW(dir);
	if (!raw_dir) {
		errno = EINVAL;
		return NULL;
	}

	struct dirent *ent = &raw_dir->d_sysde;
	struct dirent *sysent = NULL;
	int err = pfsd_readdir_r(dir, ent, &sysent);
	if (err != 0) {
		sysent = NULL;
	}

	return sysent;
}

int
pfsd_readdir_r(DIR *dir, struct dirent *entry, struct dirent **result)
{
	if (!PFSD_DIR_ISVALID(dir)) {
		errno = EINVAL;
		return -1;
	}

	pfsd_dirstream_t *raw = (pfsd_dirstream_t *)PFSD_DIR_RAW(dir);
	if (!raw || !entry || !result) {
		errno = EINVAL;
		return -1;
	}

	/* Try read from dirent buffer */
	if (raw->d_data_offset < raw->d_data_size) {
		*result = entry;
		memcpy(entry, &raw->d_data[raw->d_data_offset], sizeof(*entry));

		raw->d_data_offset += sizeof(struct dirent);
		assert (raw->d_data_offset <= raw->d_data_size);

		return 0;
	} else {
		raw->d_data_offset = 0;
		raw->d_data_size = 0;
	}

	if (raw->d_next_ino == 0) {
		*result = NULL;
		return 0;
	}

	int err;
	req_and_buf_info r;
	ipc::Request *req;
	char *dbuf;

	if ((err = pfsd_alloc_req_and_buf(r, PFSD_DIRENT_BUFFER_SIZE, &req,
					  (void **)&dbuf)) != 0) {
		errno = err;
		return -1;
	}

	/* fill request */
	req->type = PFSD_REQUEST_READDIR;
	req->req.rd_req.r_dino = raw->d_ino;
	req->req.rd_req.r_ino = raw->d_next_ino;
	req->req.rd_req.r_offset = raw->d_next_offset;

	client->executeRequest(r.rp, req);

	if (req->rsp.rd_rsp.r_res != 0) {
		*result = NULL;

		/* Dir EOF is not error */
		if (req->rsp.rd_rsp.r_res != PFSD_DIR_END) {
			err = -1;
			errno = req->rsp.rd_rsp.error;
		}
	} else {
		*result = entry;

		raw->d_data_size = req->rsp.rd_rsp.r_data_size;
		memcpy(raw->d_data, dbuf, raw->d_data_size);

		memcpy(entry, &raw->d_data[0], sizeof(*entry));
		raw->d_data_offset = sizeof(*entry);
		raw->d_next_ino = req->rsp.rd_rsp.r_ino;
		raw->d_next_offset = req->rsp.rd_rsp.r_offset;
	}

	pfsd_free_req_and_buf(r);

	return err;
}

int
pfsd_closedir(DIR *dir)
{
	if (!PFSD_DIR_ISVALID(dir)) {
		errno = EINVAL;
		return -1;
	}

	pfsd_dirstream_t *raw = (pfsd_dirstream_t *)PFSD_DIR_RAW(dir);
	if (!raw) {
		errno = EINVAL;
		return -1;
	}

	PFSD_FREE(raw);
	return 0;
}

int
pfsd_access(const char *pbdpath, int amode)
{
	if (amode != F_OK &&
		(amode & (R_OK | W_OK | X_OK)) == 0) {
		errno = EINVAL;
		return -1;
	}

	char abspath[PFS_MAX_PATHLEN];
	pbdpath = pfsd_name_init(pbdpath, abspath, sizeof abspath);
	if (pbdpath == NULL) {
		errno = EFAULT;
		return -1;
	}

	char pbdname[PFS_MAX_NAMELEN];
	if (pfsd_sdk_pbdname(pbdpath, pbdname) != 0) {
		errno = EINVAL;
		return -1;
	}

	CHECK_MOUNT(pbdname);

	if (amode & W_OK) {
		CHECK_WRITABLE();
	}

	int err;
	req_and_buf_info r;
	ipc::Request *req;
	char *buf;

	if ((err = pfsd_alloc_req_and_buf(r, PFS_MAX_PATHLEN, &req,
					  (void **)&buf)) != 0) {
		errno = err;
		return -1;
	}

	/* fill request */
	req->type = PFSD_REQUEST_ACCESS;
	req->req.a_req.a_mode = amode;
	/* copy pbdpath to iobuf */
	strncpy((char*)buf, pbdpath, PFS_MAX_PATHLEN);

	client->executeRequest(r.rp, req);

	if (req->rsp.a_rsp.a_res != 0) {
		err = -1;
		errno = req->rsp.a_rsp.error;
		if (errno != ENOENT)
			PFSD_CLIENT_ELOG("access %s: %s", pbdpath,
			    strerror(errno));
	}

	pfsd_free_req_and_buf(r);

	return err;
}

int
pfsd_fsync(int fd)
{
	pfsd_file_t *file = NULL;

	PFSD_SDK_GET_FILE(fd);

	int err;
	req_and_buf_info r;
	ipc::Request *req;

	if ((err = pfsd_alloc_req_and_buf(r, 0, &req, nullptr)) != 0) {
		errno = err;
		pfsd_put_file(file);
		return -1;
	}

	/* fill request */
	req->type = PFSD_REQUEST_FSYNC;
	req->req.fc_req.f_ino = file->f_inode;
	req->req.fc_req.common_pl_req = file->f_common_pl;

	client->executeRequest(r.rp, req);

	int result = req->rsp.fc_rsp.f_res;

	pfsd_put_file(file);

	pfsd_free_req_and_buf(r);

	return result;
}

ssize_t
pfsd_readlink(const char *pbdpath, char *buf, size_t bufsize)
{
	errno = EINVAL;
	return -1;
}

int
pfsd_chmod(const char *pbdpath, mode_t mode)
{
	return 0;
}

int
pfsd_fchmod(int fd, mode_t mode)
{
	return 0;
}

int
pfsd_chown(const char *pbdpath, uid_t owner, gid_t group)
{
	return 0;
}

int
pfsd_alloc_shared_mem_pool(const char *name, size_t elem_size, size_t capacity,
			   int num_local_lists, int local_list_limit)
{
	if (client == nullptr) {
		client = new ipc::Session;
	}

	const auto real_elem_size = std::bit_ceil(elem_size);
	auto pool = std::make_unique<ipc::SharedIndexedMemPool >(
		name, real_elem_size, capacity, num_local_lists,
		local_list_limit);
	if (client->registerMemPool(std::move(pool), false)) {
		s_n_custom_mem_pools++;
		return 0;
	}
	return -1;
}

/*
 * Register a caller-owned, fd-backed shared buffer (e.g. a memfd_create or
 * shm_open region) with the SDK and obtain a buffer id. The returned id,
 * paired with an offset, can then be passed to pfsd_pread_zc/pfsd_pwrite_zc
 * so that pfsd performs device I/O straight out of the caller's buffer with
 * no intermediate copy into the PFS shared-memory pools.
 *
 * May be called before or after pfsd_mount. Buffers registered before mount
 * are shipped to pfsd in the batched REGISTER_BUFFERS handshake that mount
 * performs; a buffer registered after mount is shipped to pfsd immediately
 * (and the local mapping is rolled back if that send fails).
 *
 * Buffer lifetime:
 *   - The id survives pfsd_remount (the RO->RW upgrade): remount keeps the
 *     connection and buffer table intact, so registrations carry over.
 *   - The id does NOT survive a pfsd_umount/pfsd_mount cycle: umount tears
 *     down the connection and clears the buffer table, so the caller must
 *     re-register after the new mount. Ids are monotonic and not reused, so
 *     a fresh registration returns a new id; any id cached across umount is
 *     stale.
 *   - pfsd_unregister_shared_buffer drops the buffer explicitly at any time
 *     after mount.
 *
 * The SDK dups `memfd`; the caller retains ownership of its own descriptor
 * and may close it once this call returns. Returns the buffer id on success,
 * or -1 on failure.
 */
int64_t
pfsd_register_shared_buffer(int memfd, size_t size)
{
	if (memfd < 0 || size == 0) {
		PFSD_CLIENT_ELOG("invalid shared buffer fd %d size %zu", memfd,
		    size);
		return -1;
	}

	/*
	 * Post-mount we know the daemon's capabilities, so reject up front if it
	 * lacks zero-copy support. Pre-mount the handshake has not happened yet;
	 * such buffers are validated when they are shipped in sendBuffers(), and
	 * any later zc IO is gated by pfsd_zc_supported() regardless.
	 */
	if (s_inited && !pfsd_zc_supported()) {
		PFSD_CLIENT_ELOG("daemon has no zero-copy support");
		return -1;
	}

	/*
	 * Dup so ownership is unambiguous: the MemFd we build owns the dup for
	 * the session lifetime, the caller keeps its original descriptor.
	 */
	int dupfd = dup(memfd);
	if (dupfd < 0) {
		PFSD_CLIENT_ELOG("dup shared buffer fd %d failed: %s", memfd,
		    strerror(errno));
		return -1;
	}

	if (client == nullptr) {
		client = new ipc::Session;
	}

	try {
		/* On mmap failure MemFd's ctor closes dupfd and throws. */
		auto id = client->registerMemBuffer(
		    std::make_unique<ipc::MemFd>(dupfd, size));
		if (!id) {
			PFSD_CLIENT_ELOG("register shared buffer failed");
			return -1;
		}

		/*
		 * Pre-mount buffers are shipped in a batch during mount; once
		 * mounted we must push this one to the server immediately. On
		 * failure roll back the local registration.
		 */
		if (s_inited && !client->sendBuffer(*id)) {
			PFSD_CLIENT_ELOG("send shared buffer %lu to pfsd failed",
			    *id);
			client->removeRawBuffer(*id);
			return -1;
		}
		return (int64_t)*id;
	} catch (const std::exception &ex) {
		PFSD_CLIENT_ELOG("register shared buffer failed: %s", ex.what());
		return -1;
	}
}

int
pfsd_unregister_shared_buffer(int64_t buf_id)
{
	if (!s_inited || client == nullptr) {
		PFSD_CLIENT_ELOG("unregister shared buffer before mount");
		return -1;
	}

	if (buf_id < 0) {
		PFSD_CLIENT_ELOG("invalid shared buffer id %ld", (long)buf_id);
		return -1;
	}

	if (!pfsd_zc_supported()) {
		PFSD_CLIENT_ELOG("daemon has no zero-copy support");
		return -1;
	}

	try {
		if (!client->sendUnregisterBuffer((uint64_t)buf_id)) {
			PFSD_CLIENT_ELOG("unregister shared buffer %ld failed",
			    (long)buf_id);
			return -1;
		}
		return 0;
	} catch (const std::exception &ex) {
		PFSD_CLIENT_ELOG("unregister shared buffer failed: %s",
		    ex.what());
		return -1;
	}
}

pfsd_buf pfsd_alloc(size_t total_mem)
{
	pfsd_buf rv{ (uint64_t)-1, -1, nullptr };
	ipc::SharedMemoryPools::AllocResult r;
	if (!client->alloc(total_mem, r)) {
		return rv;
	}
	rv.buf_id = r.bufferId;
	rv.offs = r.offset;
	rv.ptr = r.ptr;
	return rv;
}

void
pfsd_free(pfsd_buf buf)
{
	if (buf.ptr == nullptr) {
		return;
	}

	ipc::SharedMemoryPools::AllocResult r;
	r.bufferId = buf.buf_id;
	r.offset = buf.offs;
	r.ptr = buf.ptr;

	client->free(r);
}

static const uint64_t
pfsd_current_version = 2;

unsigned long
pfsd_meta_version_get() {
	return pfsd_current_version;
}

/* libpfs version, 'strings libpfs.a' can get this info */
#define _TOSTR(a)   #a
#define TOSTR(a)    _TOSTR(a)
char pfsd_build_version[] = "libpfs_version_" TOSTR(VERSION_DETAIL);
const char*
pfsd_build_version_get() {
	return pfsd_build_version;
}

