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

#ifndef _PFSD_WORKER_H_
#define _PFSD_WORKER_H_

#include <pthread.h>
#include "pfsd_proto.h"
#include "pfsd_common.h"
#include "ipc/proto.h"
#include "ipc/server.h"

struct pfsd_iochannel;

extern volatile bool g_stop;

/*A worker thread is dedicated to a shm */
typedef struct worker {
	pthread_t w_tid;
	int w_idx;
	int w_nch;
	struct pfsd_iochannel *w_channels[PFSD_SHM_MAX  *PFSD_WORKER_MAX];
	sem_t w_sem; /*for sync start thread */
	pfsd_cpu_record_t *w_cr; /*if it set affinity */
} worker_t;

extern worker_t *g_workers;
extern int g_nworkers;

extern pfsd_cpu_record_t *g_cpufile;
extern int g_ncpu;

int pfsd_worker_handle_request(ipc::Server *ipc_server, uint64_t connId,
			       ipc::Request *r);

void pfsd_worker_handle_growfs(ipc::Server *ipc_server, uint64_t connId,
			       ipc::Request *r, const growfs_request_t *req,
			       growfs_response_t *rsp);
void pfsd_worker_handle_rename(ipc::Server *ipc_server, uint64_t connId,
			       ipc::Request *r, const rename_request_t *req,
			       rename_response_t *rsp);
void pfsd_worker_handle_open(ipc::Server *ipc_server, uint64_t connId,
			     ipc::Request *r, const open_request_t *req,
			     open_response_t *rsp);
void pfsd_worker_handle_read(ipc::Server *ipc_server, uint64_t connId,
			     ipc::Request *r, const read_request_t *req,
			     read_response_t *rsp);
void pfsd_worker_handle_write(ipc::Server *ipc_server, uint64_t connId,
			      ipc::Request *r, const write_request_t *req,
			      write_response_t *rsp);
void pfsd_worker_handle_truncate(ipc::Server *ipc_server, uint64_t connId,
				 ipc::Request *r, const truncate_request_t *req,
				 truncate_response_t *rsp);
void pfsd_worker_handle_ftruncate(ipc::Server *ipc_server, uint64_t connId,
				  ipc::Request *r,
				  const ftruncate_request_t *req,
				  ftruncate_response_t *rsp);
void pfsd_worker_handle_unlink(ipc::Server *ipc_server, uint64_t connId,
			       ipc::Request *r, const unlink_request_t *req,
			       unlink_response_t *rsp);
void pfsd_worker_handle_stat(ipc::Server *ipc_server, uint64_t connId,
			     ipc::Request *r, const stat_request_t *req,
			     stat_response_t *rsp);
void pfsd_worker_handle_fstat(ipc::Server *ipc_server, uint64_t connId,
			      ipc::Request *r, const fstat_request_t *req,
			      fstat_response_t *rsp);
void pfsd_worker_handle_fallocate(ipc::Server *ipc_server, uint64_t connId,
				  ipc::Request *r,
				  const fallocate_request_t *req,
				  fallocate_response_t *rsp);
void pfsd_worker_handle_chdir(ipc::Server *ipc_server, uint64_t connId,
			      ipc::Request *r, const chdir_request_t *req,
			      chdir_response_t *rsp);
void pfsd_worker_handle_mkdir(ipc::Server *ipc_server, uint64_t connId,
			      ipc::Request *r, const mkdir_request_t *req,
			      mkdir_response_t *rsp);
void pfsd_worker_handle_rmdir(ipc::Server *ipc_server, uint64_t connId,
			      ipc::Request *r, const rmdir_request_t *req,
			      rmdir_response_t *rsp);
void pfsd_worker_handle_opendir(ipc::Server *ipc_server, uint64_t connId,
				ipc::Request *r, const opendir_request_t *req,
				opendir_response_t *rsp);
void pfsd_worker_handle_readdir(ipc::Server *ipc_server, uint64_t connId,
				ipc::Request *r, const readdir_request_t *req,
				readdir_response_t *rsp);
void pfsd_worker_handle_access(ipc::Server *ipc_server, uint64_t connId,
			       ipc::Request *r, const access_request_t *req,
			       access_response_t *rsp);
void pfsd_worker_handle_lseek(ipc::Server *ipc_server, uint64_t connId,
			      ipc::Request *r, const lseek_request_t *req,
			      lseek_response_t *rsp);

/*for debug : return current processing request's pid  */
pid_t pfsd_worker_current_processing_pid();

#endif

