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

#include <signal.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "pfs_api.h"
#include "pfs_inode.h"
#include "pfsd_api.h"
#include "pfs_mount.h"

#include "pfsd_option.h"

#include "pfsd_zlog.h"
#include "pfsd_worker.h"

volatile bool g_stop = false;

worker_t *g_workers = NULL;
int g_nworkers = 0;

pfsd_cpu_record_t *g_cpufile = NULL;
int g_ncpu = 0;

/* current processing request's pid */
__thread pid_t g_currentPid;

pid_t
pfsd_worker_current_processing_pid()
{
	return g_currentPid;
}

int
pfsd_worker_handle_request(ipc::Server *server, uint64_t connId,
			   ipc::Request *r)
{
	g_currentPid = r->req.common.owner;
	switch (r->type) {
	case PFSD_REQUEST_GROWFS:
		pfsd_worker_handle_growfs(server, connId, r, &r->req.g_req,
					  &r->rsp.g_rsp);
		return 0;

	case PFSD_REQUEST_RENAME:
		pfsd_worker_handle_rename(server, connId, r, &r->req.re_req,
					  &r->rsp.re_rsp);
		return 0;

	case PFSD_REQUEST_OPEN: {
		MNT_STAT_API_BEGIN(MNT_STAT_API_OPEN);
		pfsd_worker_handle_open(server, connId, r, &r->req.o_req,
					&r->rsp.o_rsp);
		MNT_STAT_API_END(MNT_STAT_API_OPEN);
		return 0;
	}

	case PFSD_REQUEST_READ: {
		MNT_STAT_API_BEGIN(MNT_STAT_API_PREAD);
		pfs_mntstat_set_file_type(r->file_type);
		pfsd_worker_handle_read(server, connId, r, &r->req.r_req,
					&r->rsp.r_rsp);
		MNT_STAT_API_END_BANDWIDTH(MNT_STAT_API_PREAD,
					   r->req.r_req.r_len);
		return 0;
	}

	case PFSD_REQUEST_WRITE: {
		MNT_STAT_API_BEGIN(MNT_STAT_API_PWRITE);
		pfs_mntstat_set_file_type(r->file_type);
		pfsd_worker_handle_write(server, connId, r, &r->req.w_req,
					 &r->rsp.w_rsp);
		MNT_STAT_API_END_BANDWIDTH(MNT_STAT_API_PWRITE,
					   r->req.w_req.w_len);
		return 0;
	}

	case PFSD_REQUEST_TRUNCATE: {
		MNT_STAT_API_BEGIN(MNT_STAT_API_TRUNCATE);
		pfsd_worker_handle_truncate(server, connId, r, &r->req.t_req,
					    &r->rsp.t_rsp);
		MNT_STAT_API_END(MNT_STAT_API_TRUNCATE);
		return 0;
	}

	case PFSD_REQUEST_FTRUNCATE: {
		MNT_STAT_API_BEGIN(MNT_STAT_API_FTRUNCATE);
		pfs_mntstat_set_file_type(r->file_type);
		pfsd_worker_handle_ftruncate(server, connId, r, &r->req.ft_req,
					     &r->rsp.ft_rsp);
		MNT_STAT_API_END(MNT_STAT_API_FTRUNCATE);
		return 0;
	}

	case PFSD_REQUEST_UNLINK:
		pfsd_worker_handle_unlink(server, connId, r, &r->req.un_req,
					  &r->rsp.un_rsp);
		return 0;

	case PFSD_REQUEST_STAT: {
		MNT_STAT_API_BEGIN(MNT_STAT_API_STAT);
		pfsd_worker_handle_stat(server, connId, r, &r->req.s_req,
					&r->rsp.s_rsp);
		MNT_STAT_API_END(MNT_STAT_API_STAT);
		return 0;
	}

	case PFSD_REQUEST_FSTAT: {
		MNT_STAT_API_BEGIN(MNT_STAT_API_FSTAT);
		pfs_mntstat_set_file_type(r->file_type);
		pfsd_worker_handle_fstat(server, connId, r, &r->req.f_req,
					 &r->rsp.f_rsp);
		MNT_STAT_API_END(MNT_STAT_API_FSTAT);
		return 0;
	}

	case PFSD_REQUEST_FALLOCATE: {
		MNT_STAT_API_BEGIN(MNT_STAT_API_FALLOCATE);
		pfs_mntstat_set_file_type(r->file_type);
		pfsd_worker_handle_fallocate(server, connId, r, &r->req.fa_req,
					     &r->rsp.fa_rsp);
		MNT_STAT_API_END(MNT_STAT_API_FALLOCATE);
		return 0;
	}

	case PFSD_REQUEST_CHDIR:
		pfsd_worker_handle_chdir(server, connId, r, &r->req.cd_req,
					 &r->rsp.cd_rsp);
		return 0;

	case PFSD_REQUEST_MKDIR:
		pfsd_worker_handle_mkdir(server, connId, r, &r->req.mk_req,
					 &r->rsp.mk_rsp);
		return 0;

	case PFSD_REQUEST_RMDIR:
		pfsd_worker_handle_rmdir(server, connId, r, &r->req.rm_req,
					 &r->rsp.rm_rsp);
		return 0;

	case PFSD_REQUEST_OPENDIR:
		pfsd_worker_handle_opendir(server, connId, r, &r->req.od_req,
					   &r->rsp.od_rsp);
		return 0;

	case PFSD_REQUEST_READDIR:
		pfsd_worker_handle_readdir(server, connId, r, &r->req.rd_req,
					   &r->rsp.rd_rsp);
		return 0;

	case PFSD_REQUEST_ACCESS:
		pfsd_worker_handle_access(server, connId, r, &r->req.a_req,
					  &r->rsp.a_rsp);
		return 0;

	case PFSD_REQUEST_LSEEK: {
		MNT_STAT_API_BEGIN(MNT_STAT_API_LSEEK);
		pfs_mntstat_set_file_type(r->file_type);
		pfsd_worker_handle_lseek(server, connId, r, &r->req.l_req,
					 &r->rsp.l_rsp);
		MNT_STAT_API_END(MNT_STAT_API_LSEEK);
		return 0;
	}

	case PFSD_REQUEST_FSYNC: {
		MNT_STAT_API_BEGIN(MNT_STAT_API_FSYNC);
		pfsd_worker_handle_fsync(server, connId, r, &r->req.fc_req,
					 &r->rsp.fc_rsp);
		MNT_STAT_API_END(MNT_STAT_API_FSYNC);
		return 0;
	}

	default:
		pfsd_error("worker: unknown request %d", r->type);
		return -1;
	}

	return 0;
}

#define CHECK_RSP_ERROR(rsp) do {\
	if (rsp->error != 0) { \
		return; \
	} \
} while(0)

void
pfsd_worker_handle_growfs(ipc::Server *server, uint64_t connId, ipc::Request *r,
			  const growfs_request_t *req, growfs_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_GROWFS;
	rsp->err = -1;

	CHECK_RSP_ERROR(rsp);

	rsp->err = pfs_mount_growfs(req->g_pbd);

	if (rsp->err == -1) {
		rsp->error = errno;
		pfsd_error("growfs %s error %d", req->g_pbd, errno);
	} else
		pfsd_info("growfs %s success", req->g_pbd);
}

void
pfsd_worker_handle_rename(ipc::Server *server, uint64_t connId, ipc::Request *r,
			  const rename_request_t *req, rename_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_RENAME;
	rsp->r_res = -1;

	CHECK_RSP_ERROR(rsp);

	auto oldpath =
		server->getPtr<const char>(connId, r->memBufId, r->offset);
	const char *newpath = (const char*)oldpath + PFS_MAX_PATHLEN;

	pfsd_info("pid %d %s -> %s", g_currentPid, oldpath, newpath);

	rsp->r_res = pfs_rename(oldpath, newpath);
	if (rsp->r_res < 0) {
		rsp->error = errno;
		pfsd_error("rename %s -> %s error: %d", oldpath, newpath, errno);
	} else
		pfsd_info("rename %s -> %s success", oldpath, newpath);
}

void
pfsd_worker_handle_open(ipc::Server *server, uint64_t connId, ipc::Request *r,
			const open_request_t *req, open_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_OPEN;
	rsp->o_ino = -1;
	rsp->o_off = 0;

	CHECK_RSP_ERROR(rsp);

	auto pbdpath =
		server->getPtr<const char>(connId, r->memBufId, r->offset);

	memset(&rsp->common_pl_rsp, 0, sizeof(rsp->common_pl_rsp));
	rsp->o_ino = pfsd_open_svr(pbdpath, req->o_flags, req->o_mode,
	    &rsp->common_pl_rsp.pl_btime,
	    &rsp->common_pl_rsp.pl_file_type);
	rsp->o_off = 0;
	if (rsp->o_ino < 0)
		rsp->error = errno;
}

#define PFSD_GET_MOUNT(mntid, rsp) do {\
	mnt = pfs_get_mount_byid(mntid); \
	if (mnt == NULL) { \
		pfsd_error("Cant find mntid %d", mntid); \
		rsp->error = ENODEV; \
		return; \
	} \
} while(0)

#define PFSD_PUT_MOUNT(mnt) do {\
	if (mnt) { \
		pfs_put_mount(mnt); \
		mnt = NULL; \
	} \
} while(0)

extern pfs_inode_t *pfs_inode_get_and_load(pfs_mount_t *mnt, pfs_ino_t ino);

#define PFSD_GET_MOUNT_AND_INODE(mntid, ino, rsp) do {\
	mnt = pfs_get_mount_byid(mntid); \
	if (mnt == NULL) { \
		pfsd_error("Cant find mntid %d", mntid); \
		rsp->error = ENODEV; \
		return; \
	} \
	inode = pfs_inode_get_and_load(mnt, ino); \
	if (inode == NULL) { \
		pfs_put_mount(mnt); \
		rsp->error = EBADF; \
		return; \
	} \
} while(0)

/*
 * LRU inode-list
 */
#define PFSD_PUT_MOUNT_AND_INODE(mnt, in) do {\
	if (mnt) { \
		pfs_put_inode(mnt, in); \
		pfs_put_mount(mnt); \
		mnt = NULL; \
	} \
} while(0)

void
pfsd_worker_handle_read(ipc::Server *server, uint64_t connId, ipc::Request *r,
			const read_request_t *req, read_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_READ;
	rsp->r_len = -1;

	CHECK_RSP_ERROR(rsp);

	rsp->r_ino = req->r_ino;
	size_t read_len = req->r_len;

	auto rbuf =
		server->getPtr<unsigned char>(connId, r->memBufId, r->offset,
		    read_len);
	if (rbuf == NULL) {
		pfsd_error("pid %d read buf out of range: buf %d off %ld len %zu",
		    g_currentPid, r->memBufId, (long)r->offset, read_len);
		rsp->error = EINVAL;
		return;
	}

	if (req->r_off < 0 || req->r_ino < 0) {
		pfsd_error("pid %d read invalid ino %ld or offset %lu", g_currentPid,
		    req->r_ino, req->r_off);
		errno = EINVAL;
		return;
	}
	pfs_mount_t *mnt = NULL;
	pfs_inode_t *inode = NULL;
	PFSD_GET_MOUNT_AND_INODE(req->mntid, req->r_ino, rsp);

	rsp->r_len = pfsd_pread_svr(mnt, inode, rbuf, read_len, req->r_off,
	    req->common_pl_req.pl_btime);

	PFSD_PUT_MOUNT_AND_INODE(mnt, inode);

	if (rsp->r_len < 0) {
		pfsd_error("read ino %ld failed %d", req->r_ino, errno);
		rsp->error = errno;
	} else
		pfsd_debug("read ino %ld return %ld bytes", req->r_ino, rsp->r_len);
}

void
pfsd_worker_handle_write(ipc::Server *server, uint64_t connId, ipc::Request *r,
			 const write_request_t *req, write_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_WRITE;
	rsp->w_ino = req->w_ino;
	rsp->w_len = -1;

	CHECK_RSP_ERROR(rsp);

	auto wbuf =
		server->getPtr<unsigned char>(connId, r->memBufId, r->offset,
		    req->w_len);
	if (wbuf == NULL) {
		pfsd_error("pid %d write buf out of range: buf %d off %ld len %zu",
		    g_currentPid, r->memBufId, (long)r->offset, req->w_len);
		rsp->error = EINVAL;
		return;
	}
	if (req->w_ino < 0) {
		pfsd_error("pid %d error inode %ld, offset %lu", g_currentPid,
		    req->w_ino, req->w_off);
		rsp->error = EINVAL;
		return;
	}

	pfs_mount_t* mnt = NULL;
	pfs_inode_t* inode = NULL;
	PFSD_GET_MOUNT_AND_INODE(req->mntid, req->w_ino, rsp);

	rsp->w_len = pfsd_pwrite_svr(mnt, inode, req->w_flags, wbuf, req->w_len,
	    req->w_off, &rsp->w_file_size, req->common_pl_req.pl_btime);

	PFSD_PUT_MOUNT_AND_INODE(mnt, inode);

	if (rsp->w_len < 0) {
		rsp->error = errno;
		pfsd_error("pid %d write ino %ld failed: %d", g_currentPid, req->w_ino, errno);
	} else
		pfsd_debug("write ino %ld return %ld bytes", req->w_ino, rsp->w_len);
}

void
pfsd_worker_handle_truncate(ipc::Server *server, uint64_t connId,
			    ipc::Request *r, const truncate_request_t *req,
			    truncate_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_TRUNCATE;
	rsp->t_res = -1;
	CHECK_RSP_ERROR(rsp);

	auto pbdpath =
		server->getPtr<const char>(connId, r->memBufId, r->offset);

	rsp->t_res = pfsd_truncate_svr(pbdpath, req->t_len);
	if (rsp->t_res < 0) {
		rsp->error = errno;
		pfsd_error("pid %d truncate %s to len %ld error: %d",
		    g_currentPid, pbdpath, req->t_len, errno);
	} else
		pfsd_info("pid %d truncate %s to len %ld success", g_currentPid,
		    pbdpath, req->t_len);
}

void
pfsd_worker_handle_ftruncate(ipc::Server *server, uint64_t connId,
			     ipc::Request *r, const ftruncate_request_t *req,
			     ftruncate_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_FTRUNCATE;
	rsp->f_res = -1;

	CHECK_RSP_ERROR(rsp);

	pfs_mount_t *mnt = NULL;
	pfs_inode_t *inode = NULL;
	PFSD_GET_MOUNT_AND_INODE(req->mntid, req->f_ino, rsp);

	rsp->f_res = pfsd_ftruncate_svr(mnt, inode, req->f_len,
	    req->common_pl_req.pl_btime);

	PFSD_PUT_MOUNT_AND_INODE(mnt, inode);

	if (rsp->f_res < 0) {
		rsp->error = errno;
		pfsd_error("pid %d ftruncate ino %ld to len %ld err: %d",
		    g_currentPid, req->f_ino, req->f_len, errno);
	} else
		pfsd_info("pid %d ftruncate ino %ld to len %ld success",
		    g_currentPid, req->f_ino, req->f_len);
}

void
pfsd_worker_handle_unlink(ipc::Server *server, uint64_t connId, ipc::Request *r,
			  const unlink_request_t *req, unlink_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_UNLINK;
	rsp->u_res = -1;

	CHECK_RSP_ERROR(rsp);

	auto pbdpath =
		server->getPtr<const char>(connId, r->memBufId, r->offset);
	rsp->u_res = pfsd_unlink_svr(pbdpath);
	if (rsp->u_res < 0) {
		rsp->error = errno;
		pfsd_error("pid %d unlink %s error %d", g_currentPid, pbdpath, rsp->error);
	} else
		pfsd_info("pid %d unlink %s success", g_currentPid, pbdpath);
}

void
pfsd_worker_handle_stat(ipc::Server *server, uint64_t connId, ipc::Request *r,
			const stat_request_t *req, stat_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_STAT;
	rsp->s_res = -1;

	CHECK_RSP_ERROR(rsp);

	auto pbdpath =
		server->getPtr<const char>(connId, r->memBufId, r->offset);
	rsp->s_res = pfsd_stat_svr(pbdpath, &rsp->s_st);
	if (rsp->s_res < 0) {
		rsp->error = errno;
		pfsd_error("pid %d stat %s error %d", g_currentPid, pbdpath, rsp->error);
	} else
		pfsd_debug("pid %d stat %s size is %lu", g_currentPid, pbdpath, rsp->s_st.st_size);
}

void
pfsd_worker_handle_fstat(ipc::Server *server, uint64_t connId, ipc::Request *r,
			 const fstat_request_t *req, fstat_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_FSTAT;
	rsp->f_res = -1;

	CHECK_RSP_ERROR(rsp);

	pfs_mount_t *mnt = NULL;
	pfs_inode_t *inode = NULL;
	PFSD_GET_MOUNT_AND_INODE(req->mntid, req->f_ino, rsp);

	rsp->f_res = pfsd_fstat_svr(mnt, inode, &rsp->f_st,
	    req->common_pl_req.pl_btime);

	PFSD_PUT_MOUNT_AND_INODE(mnt, inode);

	if (rsp->f_res < 0) {
		rsp->error = errno;
		pfsd_error("pid %d fstat ino %ld error %d", g_currentPid, req->f_ino, errno);
	} else
		pfsd_debug("pid %d fstat ino %ld success, size is %lu",
		    g_currentPid, req->f_ino, rsp->f_st.st_size);
}

void
pfsd_worker_handle_fallocate(ipc::Server *server, uint64_t connId,
			     ipc::Request *r, const fallocate_request_t *req,
			     fallocate_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_FALLOCATE;
	rsp->f_ino = req->f_ino;
	rsp->f_res = -1;

	CHECK_RSP_ERROR(rsp);

	pfs_mount_t *mnt = NULL;
	pfs_inode_t *inode = NULL;
	PFSD_GET_MOUNT_AND_INODE(req->mntid, req->f_ino, rsp);

	rsp->f_res = pfsd_fallocate_svr(mnt, inode, req->f_off, req->f_len, req->f_mode,
	    req->common_pl_req.pl_btime);

	PFSD_PUT_MOUNT_AND_INODE(mnt, inode);

	if (rsp->f_res < 0) {
		rsp->error = errno;
		rsp->f_res = -1;
		pfsd_error("pid %d fallocate ino %ld, off %ld len %ld error: %d",
		    g_currentPid, req->f_ino, req->f_off, req->f_len, errno);
	} else
		pfsd_info("pid %d fallocate ino %ld, off %ld len %ld success",
		    g_currentPid, req->f_ino, req->f_off, req->f_len);
}

void
pfsd_worker_handle_chdir(ipc::Server *server, uint64_t connId, ipc::Request *r,
			 const chdir_request_t *req, chdir_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_CHDIR;
	rsp->c_res = -1;

	CHECK_RSP_ERROR(rsp);

	auto pbdpath =
		server->getPtr<const char>(connId, r->memBufId, r->offset);
	rsp->c_res = pfsd_chdir_svr(pbdpath);
	if (rsp->c_res < 0) {
		rsp->error = errno;
		pfsd_error("pid %d chdir to %s error %d", g_currentPid, pbdpath, errno);
	} else
		pfsd_info("pid %d chdir to %s success", g_currentPid, pbdpath);
}

void
pfsd_worker_handle_mkdir(ipc::Server *server, uint64_t connId, ipc::Request *r,
			 const mkdir_request_t *req, mkdir_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_MKDIR;
	rsp->m_res = -1;

	CHECK_RSP_ERROR(rsp);

	auto pbdpath =
		server->getPtr<const char>(connId, r->memBufId, r->offset);

	rsp->m_res = pfs_mkdir(pbdpath, req->m_mode);
	if (rsp->m_res < 0) {
		rsp->error = errno;
		pfsd_error("pid %d mkdir %s error: %d", g_currentPid, pbdpath, errno);
	} else
		pfsd_info("pid %d mkdir %s success", g_currentPid, pbdpath);
}

void
pfsd_worker_handle_rmdir(ipc::Server *server, uint64_t connId, ipc::Request *r,
			 const rmdir_request_t *req, rmdir_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_RMDIR;
	rsp->r_res = -1;

	CHECK_RSP_ERROR(rsp);

	auto pbdpath =
		server->getPtr<const char>(connId, r->memBufId, r->offset);

	rsp->r_res = pfs_rmdir(pbdpath);
	if (rsp->r_res < 0) {
		rsp->error = errno;
		pfsd_error("pid %d rmdir %s error: %d", g_currentPid, pbdpath, errno);
	} else
		pfsd_info("pid %d rmdir %s success", g_currentPid, pbdpath);
}

void
pfsd_worker_handle_opendir(ipc::Server *server, uint64_t connId,
			   ipc::Request *r, const opendir_request_t *req,
			   opendir_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_OPENDIR;
	rsp->o_res = -1;

	CHECK_RSP_ERROR(rsp);

	auto pbdpath =
		server->getPtr<const char>(connId, r->memBufId, r->offset);
	pfsd_debug("pid %d, opendir %s", g_currentPid, pbdpath);

	rsp->o_res = pfsd_opendir_svr(pbdpath, &rsp->o_dino, &rsp->o_first_ino);
	if (rsp->o_res != 0) {
		rsp->error = errno;
		pfsd_error("pid %d opendir %s error: %d", g_currentPid, pbdpath, rsp->error);
	}
	/* pfsd_opendir_svr will print detail logs */
}

void
pfsd_worker_handle_readdir(ipc::Server *server, uint64_t connId,
			   ipc::Request *r, const readdir_request_t *req,
			   readdir_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_READDIR;
	rsp->r_res = -1;

	CHECK_RSP_ERROR(rsp);

	if (r->size < PFSD_DIRENT_BUFFER_SIZE) {
		rsp->error = EFAULT;
		rsp->r_res = -1;
		return;
	}

	pfs_mount_t *mnt = NULL;
	PFSD_GET_MOUNT(req->mntid, rsp);

	rsp->r_res = 0;

	auto rbuf =
		server->getPtr<unsigned char>(connId, r->memBufId, r->offset);

	int64_t cur_ino = req->r_ino;
	uint64_t cur_offset = req->r_offset;
	int64_t next_ino = 0;
	uint64_t data_size = 0;
	while (cur_ino != 0 && data_size + sizeof(struct dirent) <= PFSD_DIRENT_BUFFER_SIZE) {
		int err = pfsd_readdir_svr(mnt, req->r_dino, cur_ino, cur_offset,
		    (struct dirent*)&rbuf[data_size], &next_ino);
		if (err != 0) {
			if (data_size == 0) {
				rsp->r_res = err;
				rsp->error = errno;
				rsp->r_data_size = 0;
				rsp->r_ino = 0;
			}

			if (err == PFSD_DIR_END)
				rsp->r_ino = 0; /* Dir EOF */

			break;
		} else {
			data_size += sizeof(struct dirent);
			pfsd_debug("got ino %ld at offset %ld", cur_ino, cur_offset);
			cur_ino = next_ino;
			++cur_offset;

			rsp->r_data_size = data_size;
			rsp->r_ino = next_ino;
			rsp->r_offset = cur_offset;
		}
	}
	pfsd_debug("pid %d, readdir err %d, req offset %ld rsp offset %ld",
	    g_currentPid, rsp->error, req->r_offset, rsp->r_offset);

	PFSD_PUT_MOUNT(mnt);
}

void
pfsd_worker_handle_access(ipc::Server *server, uint64_t connId, ipc::Request *r,
			  const access_request_t *req, access_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_ACCESS;
	rsp->a_res = -1;

	CHECK_RSP_ERROR(rsp);

	auto pbdpath =
		server->getPtr<const char>(connId, r->memBufId, r->offset);
	rsp->a_res = pfs_access(pbdpath, req->a_mode);
	if (rsp->a_res < 0) {
		rsp->error = errno;
		pfsd_error("pid %d access %s mode %u error: %d", g_currentPid,
		    pbdpath, req->a_mode, errno);
	} else
		pfsd_debug("pid %d access %s mode %u success", g_currentPid,
		    pbdpath, req->a_mode);
}

void
pfsd_worker_handle_lseek(ipc::Server *server, uint64_t connId, ipc::Request *r,
			 const lseek_request_t *req, lseek_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_LSEEK;
	rsp->l_offset = off_t(-1);

	CHECK_RSP_ERROR(rsp);

	int64_t ino = req->l_ino;
	off_t off = req->l_offset;
	if (req->l_whence != SEEK_END) {
		rsp->error = EINVAL;
		pfsd_error("pid %d lseek not SEEK_END: whence is %d",
		    g_currentPid, req->l_whence);
		return;
	}

	pfs_mount_t *mnt = NULL;
	pfs_inode_t *inode = NULL;
	PFSD_GET_MOUNT_AND_INODE(req->mntid, ino, rsp);

	rsp->l_offset = pfsd_lseek_end_svr(mnt, inode, off,
	    req->common_pl_req.pl_btime);

	PFSD_PUT_MOUNT_AND_INODE(mnt, inode);

	if (rsp->l_offset < 0) {
		rsp->error = errno;
		pfsd_error("pid %d lseek error %d, ino %ld, off %ld",
		    g_currentPid, rsp->error, ino, off);
	} else
		pfsd_debug("pid %d lseek ino %ld, off %ld", g_currentPid, ino, off);
}

void
pfsd_worker_handle_fsync(ipc::Server *server, uint64_t connId, ipc::Request *r,
    const fsync_request_t *req, fsync_response_t *rsp)
{
	rsp->type = PFSD_RESPONSE_FSYNC;

	CHECK_RSP_ERROR(rsp);

	int64_t ino = req->f_ino;

	pfs_mount_t *mnt = NULL;
	pfs_inode_t *inode = NULL;
	PFSD_GET_MOUNT_AND_INODE(req->mntid, ino, rsp);

	rsp->f_res = pfsd_fsync_svr(mnt, inode, req->common_pl_req.pl_btime);

	PFSD_PUT_MOUNT_AND_INODE(mnt, inode);

	if (rsp->f_res < 0) {
		rsp->error = errno;
		pfsd_error("pid %d fsync error %d, ino %ld",
		    g_currentPid, rsp->error, ino);
	} else
		pfsd_debug("pid %d fsync ino %ld", g_currentPid, ino);
}


