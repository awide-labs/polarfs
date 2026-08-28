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

#include <string.h>
#include <sys/time.h>

#include "pfs_admin.h"
#include "pfs_devio.h"
#include "pfs_devstat.h"
#include "pfs_memory.h"
#include "pfs_util.h"

extern uint64_t		pfs_devs_epoch;
extern pfs_dev_t	*pfs_devs[PFS_MAX_NCHD];

/*
 * This is the format expected by the pfsadm utility.
 * If you change this format, change it in pfsadm as well.
 */
struct pfs_devstat_format {
	uint64_t	w_start_count;
	uint64_t	w_end_count;
	struct timeval	w_busy_time;
	uint64_t	w_bytes[PFSDEV_REQ_MAX];
	uint64_t	w_ops[PFSDEV_REQ_MAX];
	struct timeval	w_duration[PFSDEV_REQ_MAX];
};

struct devstat_snap {
	struct timeval	s_snaptime;
	uint64_t	s_ndev;
	uint64_t	s_epoch;

	char		s_cluster[PFS_MAX_CLUSTERLEN];
	char		s_devname[PFS_MAX_PBDLEN];
	int		s_type;
	int		s_flags;
	struct pfs_devstat_format s_iostat;
};

static inline pfs_devstat_shard_t *
devstat_shard(pfs_devstat_t *ds, const pfs_devio_t *io)
{
	return &ds->ds_shards[io->io_stat_cpu];
}

static bool
devstat_has_inflight(pfs_devstat_t *ds)
{
	int	i;

	for (i = 0; i < PFS_DEVSTAT_SHARDS; i++) {
		uint64_t	started;
		uint64_t	ended;

		started = __atomic_load_n(&ds->ds_shards[i].ds_start_count,
		    __ATOMIC_RELAXED);
		ended = __atomic_load_n(&ds->ds_shards[i].ds_end_count,
		    __ATOMIC_RELAXED);
		if (started > ended)
			return true;
	}
	return false;
}

static inline void
devstat_update_busy_time(pfs_devstat_t *ds, pfs_devstat_shard_t *sh,
    int64_t now, bool is_end)
{
	int64_t		stamp;

	stamp = __atomic_load_n(&ds->ds_busy_stamp, __ATOMIC_RELAXED);

	if (stamp == 0) { /* just initialize with now */
		__atomic_compare_exchange_n(&ds->ds_busy_stamp, &stamp, now, 0,
		    __ATOMIC_RELAXED, __ATOMIC_RELAXED);

		return;
	}

	if (now <= stamp || now - stamp < PFS_DEVSTAT_BUSY_QUANTUM)
		return;

	if (!__atomic_compare_exchange_n(&ds->ds_busy_stamp, &stamp, now, 0,
	    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		return;

	if (is_end || devstat_has_inflight(ds))
		__atomic_fetch_add(&sh->ds_local_busy_time, now - stamp,
		    __ATOMIC_RELAXED);
}

void
pfs_devstat_init(pfs_devstat_t *ds)
{
	int	err;

	memset(ds, 0, sizeof(*ds));
	err = pfs_mem_memalign((void **)&ds->ds_shards, alignof(pfs_devstat_shard_t),
	    sizeof(pfs_devstat_shard_t) * PFS_DEVSTAT_SHARDS, M_DEVSTAT);
	PFS_VERIFY(err == 0 && ds->ds_shards != NULL);
	memset(ds->ds_shards, 0,
	    sizeof(pfs_devstat_shard_t) * PFS_DEVSTAT_SHARDS);
}

void
pfs_devstat_uninit(pfs_devstat_t *ds)
{
	uint64_t	started = 0, ended = 0;
	int		i;

	for (i = 0; i < PFS_DEVSTAT_SHARDS; i++) {
		started += __atomic_load_n(&ds->ds_shards[i].ds_start_count,
		    __ATOMIC_RELAXED);
		ended += __atomic_load_n(&ds->ds_shards[i].ds_end_count,
		    __ATOMIC_RELAXED);
	}
	PFS_ASSERT(started == ended);

	if (ds->ds_shards != NULL) {
		pfs_mem_free(ds->ds_shards, M_DEVSTAT);
		ds->ds_shards = NULL;
	}
}

void
pfs_devstat_io_start(pfs_devstat_t *ds, const pfs_devio_t *io)
{
	pfs_devstat_shard_t	*sh;
	int64_t			now;

	PFS_ASSERT(ds == &io->io_dev->d_ds);
	if (!(io->io_flags & IO_STAT))
		return;

	sh = devstat_shard(ds, io);

	/* reuse the timestamp already taken in pfs_io_start() */
	now = pfs_timeval_to_us(&io->io_start_ts);

	devstat_update_busy_time(ds, sh, now, false);
	__atomic_fetch_add(&sh->ds_start_count, 1, __ATOMIC_RELAXED);
}

void
pfs_devstat_io_end(pfs_devstat_t *ds, const pfs_devio_t *io)
{
	pfs_devstat_shard_t	*sh;
	int64_t			now;

	PFS_ASSERT(ds == &io->io_dev->d_ds);
	if (!(io->io_flags & IO_STAT))
		return;

	sh = devstat_shard(ds, io);
	now = (int64_t)gettimeofday_us();

	devstat_update_busy_time(ds, sh, now, true);

	if (io->io_error == 0) {
		int		op = io->io_op;
		int64_t		start_us = pfs_timeval_to_us(&io->io_start_ts);

		__atomic_fetch_add(&sh->ds_bytes[op], io->io_len,
		    __ATOMIC_RELAXED);
		__atomic_fetch_add(&sh->ds_ops[op], 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&sh->ds_duration[op], now - start_us,
		    __ATOMIC_RELAXED);
	}

	__atomic_fetch_add(&sh->ds_end_count, 1, __ATOMIC_RELAXED);
}

int
pfs_devstat_snap(int devi, admin_buf_t *ab)
{
	pfs_dev_t		*dev = pfs_devs[devi];
	pfs_devstat_t		*ds;
	struct devstat_snap	*snap;
	int64_t			busy_us = 0;
	int64_t			dur_us[PFSDEV_REQ_MAX];
	int			err, n, i, j;

	PFS_ASSERT(0 <= devi && devi < PFS_MAX_NCHD && dev != NULL);
	ds = &dev->d_ds;

	/* 1. reserve buffer */
	snap = (struct devstat_snap *)pfs_adminbuf_reserve(ab, sizeof(*snap));
	if (snap == NULL)
		ERR_RETVAL(ENOBUFS);

	/* 2. snapshot header */
	err = gettimeofday(&snap->s_snaptime, NULL);
	PFS_VERIFY(err == 0);
	snap->s_ndev = 1;
	snap->s_epoch = pfs_devs_epoch;

	n = strncpy_safe(snap->s_cluster, dev->d_cluster, sizeof(snap->s_cluster));
	PFS_VERIFY(n > 0);
	n = strncpy_safe(snap->s_devname, dev->d_devname, sizeof(snap->s_devname));
	PFS_VERIFY(n > 0);
	snap->s_type = dev->d_type;
	snap->s_flags = dev->d_flags;

	/* 3. aggregate per-CPU shards (eventually consistent) */
	memset(&snap->s_iostat, 0, sizeof(snap->s_iostat));
	memset(dur_us, 0, sizeof(dur_us));

	for (i = 0; i < PFS_DEVSTAT_SHARDS; i++) {
		pfs_devstat_shard_t	*sh = &ds->ds_shards[i];

		snap->s_iostat.w_start_count += __atomic_load_n(&sh->ds_start_count,
		    __ATOMIC_RELAXED);
		snap->s_iostat.w_end_count += __atomic_load_n(&sh->ds_end_count,
		    __ATOMIC_RELAXED);
		busy_us += __atomic_load_n(&sh->ds_local_busy_time,
		    __ATOMIC_RELAXED);
		for (j = 0; j < PFSDEV_REQ_MAX; j++) {
			snap->s_iostat.w_bytes[j] += __atomic_load_n(&sh->ds_bytes[j],
			    __ATOMIC_RELAXED);
			snap->s_iostat.w_ops[j] += __atomic_load_n(&sh->ds_ops[j],
			    __ATOMIC_RELAXED);
			dur_us[j] += __atomic_load_n(&sh->ds_duration[j],
			    __ATOMIC_RELAXED);
		}
	}

	/* convert internal us back to the stats's struct timeval */
	pfs_us_to_timeval(busy_us, &snap->s_iostat.w_busy_time);
	for (j = 0; j < PFSDEV_REQ_MAX; j++)
		pfs_us_to_timeval(dur_us[j], &snap->s_iostat.w_duration[j]);

	pfs_adminbuf_consume(ab, sizeof(*snap));
	return 0;
}
