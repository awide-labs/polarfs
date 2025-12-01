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

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>

#include "lib/dclcrwlock.h"
#include "pfsd_common.h"
#include "pfsd_chnl.h"
#include "pfsd_chnl_impl.h"

static pthread_mutex_t pfsd_connect_mutex = PTHREAD_MUTEX_INITIALIZER;
static DCLCRWLock pfsd_connect_rwlocks[CHNL_MAX_CONN];

static pfsd_connect_entry_t pfsd_connect_data[CHNL_MAX_CONN];

void pfsd_chnl_init()
{
	for (auto &rwlock : pfsd_connect_rwlocks) {
		rwlock.init();
	}
}

void pfsd_chnl_destroy()
{
	for (auto &rwlock : pfsd_connect_rwlocks) {
		rwlock.destroy();
	}
}

static pfsd_connect_entry_t *
pfsd_connect_get_entry(int32_t connect_id)
{
	pfsd_connect_entry_t *result = NULL;
	if (!pfsd_is_valid_connid(connect_id)) {
		errno = EINVAL;
		return result;
	}
	pfsd_connect_rwlocks[connect_id].lock_shared();

	if(pfsd_connect_data[connect_id].connect_id == connect_id) {
		result = &pfsd_connect_data[connect_id];
	}

	return  result;
}

static pfsd_connect_entry_t *
pfsd_connect_put_entry(int32_t connect_id)
{
	pfsd_connect_entry_t *result = NULL;
	if (!pfsd_is_valid_connid(connect_id)) {
		errno = EINVAL;
		return result;
	}
	if(pfsd_connect_data[connect_id].connect_id == connect_id) {
		result = &pfsd_connect_data[connect_id];
	}
	pfsd_connect_rwlocks[connect_id].unlock_shared();
	return  result;
}

/* Both sides */
static void
pfsd_chnl_ctx_create(const char *name, void **ctx, pfsd_chnl_op_t **op, bool is_svr)
{
	extern pfsd_chnl_op_t *__start__pfsd_chnl[];
	extern pfsd_chnl_op_t *__stop__pfsd_chnl[];
	pfsd_chnl_op_t **ci;

	*ctx = NULL;
	*op = NULL;

	for (ci = __start__pfsd_chnl; ci != __stop__pfsd_chnl; ci++) {
		void *result = (*ci)->chnl_ctx_create(name, is_svr);
		if (result != NULL) {
			*ctx = result; /* chnl_ctx_shm_t type for SHM */
			*op = *ci;
			break;
		}
	}

	return;
}

/* server side */
int
pfsd_chnl_get_logic_id(int32_t connect_id)
{
	pfsd_connect_entry_t *result = pfsd_connect_get_entry(connect_id);
	if (result == NULL) {
		errno = EINVAL;
		return -1;
	}

	int id = result->connect_mntid;
	pfsd_connect_put_entry(connect_id);

	return id;
}

/* server side */
int
pfsd_chnl_listen(const char *svr_addr, const char *pbdname, int nworkers,
    void *arg1, void *arg2)
{
	/* chnl_ctx_shm_t type for SHM, used by listen thread */
	void *ctx = NULL;
	int32_t result = -1;
	pfsd_chnl_op *opt = NULL;

	if (!svr_addr || !pbdname || nworkers <= 0) {
		fprintf(stderr,
		    "wrong args svr_addr(%p) pbdname(%p) nworkers(%d)\n",
		    svr_addr, pbdname, nworkers);

		errno = EINVAL;
		return -1;
	}

	char full_svr_addr[PFSD_MAX_SVR_ADDR_SIZE] = "";
	snprintf(full_svr_addr, PFSD_MAX_SVR_ADDR_SIZE, "%s/%s", svr_addr,
	    pbdname);
	svr_addr = full_svr_addr;
	if (mkdir(svr_addr, 0777) != 0 && errno != EEXIST) {
		fprintf(stderr, "mkdir %s failed %s\n", svr_addr,
		    strerror(errno));
		return -1;
	}
	chmod(svr_addr, 0777);

	pfsd_chnl_ctx_create(svr_addr, &ctx, &opt, true);
	if (opt == NULL) {
		errno = EPROTONOSUPPORT;
		return result;
	}

	result = opt->chnl_prepare(pbdname, nworkers, arg2);
	if (result != 0) {
		fprintf(stderr, "chnl_prepare failed %s\n", strerror(errno));
		opt->chnl_ctx_destroy(ctx);
		return result;
	}

	result = opt->chnl_listen(ctx, opt, svr_addr, arg1, arg2);
	if (result < 0) {
		fprintf(stderr, "chnl_listen failed %s\n", strerror(errno));
		opt->chnl_ctx_destroy(ctx);
		return result;
	}

	result = opt->chnl_recover(ctx, opt, svr_addr, nworkers, NULL);
	if (result != 0) {
		fprintf(stderr, "chnl_recover failed %s\n", strerror(errno));
		opt->chnl_ctx_destroy(ctx);
		return result;
	}

	return result;
}

int32_t
pfsd_chnl_accept_begin(void *ctx, void *op, int32_t conn_id_hint)
{
	pfsd_connect_entry_t *ptr = NULL;
	int32_t conn_id = -1;
	assert(pfsd_is_valid_connid(conn_id_hint));
	pthread_mutex_lock(&pfsd_connect_mutex);
	while (true) {
		//We use 2 to specialize odd id is for tool.
		for (int i = conn_id_hint; i < CHNL_MAX_CONN; i += 2) {
			ptr = pfsd_connect_data + i;
			auto locked = pfsd_connect_rwlocks[i].try_lock();
			if (locked && ptr->connect_id == 0) {
				ptr->connect_id = i;
				ptr->connect_data = ctx;
				ptr->connect_op = (pfsd_chnl_op_t *) op;
				pfsd_connect_rwlocks[i].unlock();
				conn_id = i;
				break;
			}
			if (locked) pfsd_connect_rwlocks[i].unlock();
		}
		if (conn_id == -1 && conn_id_hint > 1) {
			conn_id_hint = conn_id_hint % 2 + 4;
			continue;
		}
		break;
	}
	if (conn_id < 0) {
		fprintf(stderr, "failed to alloc conn id, hint %d\n",
		    conn_id_hint);
	}
	return conn_id;
}

void
pfsd_chnl_accept_begin_rollback(int32_t conn_id)
{
	if (pfsd_is_valid_connid(conn_id)) {
		pfsd_connect_entry_t *ptr = pfsd_connect_data + conn_id;
		pfsd_connect_rwlocks[conn_id].lock();
		ptr->connect_id = 0;
		pfsd_connect_rwlocks[conn_id].unlock();
	}
}

void
pfsd_chnl_accept_end(int32_t conn_id, int mnt_id)
{
	if (pfsd_is_valid_connid(conn_id)) {
		pfsd_connect_entry_t *ptr = pfsd_connect_data + conn_id;
		ptr->connect_mntid = mnt_id;
	}

	pthread_mutex_unlock(&pfsd_connect_mutex);
}

int
pfsd_chnl_close_begin(int32_t connect_id)
{
	assert (pfsd_is_valid_connid(connect_id));
	int result = -1;
	pfsd_connect_entry_t *ptr = NULL;
	pthread_mutex_lock(&pfsd_connect_mutex);

	ptr = pfsd_connect_data + connect_id;
	if (ptr->connect_id != 0) {
		pfsd_connect_rwlocks[connect_id].lock();
		assert (ptr->connect_op);
		result = ptr->connect_op->chnl_close(ptr->connect_data,
		    true);
		ptr->connect_id = 0;
		pfsd_connect_rwlocks[connect_id].unlock();
	} else {
		errno = EINVAL;
	}

	return result;
}

void
pfsd_chnl_close_end()
{
	pthread_mutex_unlock(&pfsd_connect_mutex);
}

bool pfsd_is_valid_connid(int32_t cid)
{
	bool ok = cid > 0 && cid < CHNL_MAX_CONN;
	if (!ok && cid != -1)
		fprintf(stderr, "Wrong conn id %d\n", cid);
	return ok;
}

bool
pfsd_is_conn_closed(int32_t connect_id)
{
	if (!pfsd_is_valid_connid(connect_id))
		return true;
	return pfsd_connect_data[connect_id].connect_id != connect_id;
}
