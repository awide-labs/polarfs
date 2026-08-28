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

#ifndef _PFS_DEVSTAT_H_
#define _PFS_DEVSTAT_H_

#include "pfs_impl.h"
#ifndef PFS_DISK_IO_ONLY
#include "pfs_iochnl.h"
#else
enum {
    PFSDEV_REQ_NOP      = 0,
    PFSDEV_REQ_INFO     = 1,
    PFSDEV_REQ_RD       = 2,
    PFSDEV_REQ_WR       = 3,
    PFSDEV_REQ_TRIM     = 4,
    PFSDEV_REQ_FLUSH    = 5,

    PFSDEV_REQ_MAX,
};
#endif

typedef struct pfs_devio pfs_devio_t;
typedef struct admin_buf admin_buf_t;

#include <stddef.h>

#include "../ipc/access_spreader.h"
#include "../ipc/pfs_align.h"

/*
 * Per-CPU sharded devstat counters with an eventually consistent
 * snapshot.  Additive counters are updated with relaxed atomic
 * fetch_add, and the current CPU is cached for the whole IO, so the
 * accounting stays correct even when threads migrate between CPUs.
 *
 * busy_time follows the approach of Linux iostat (see
 * update_io_ticks()).  The only shared atomic variable here is
 * ds_busy_stamp.  Time is quantized with a granularity of
 * PFS_DEVSTAT_BUSY_QUANTUM: at IO start and IO end each thread
 * checks whether a full quantum has passed since the last
 * ds_busy_stamp, and if so tries to advance it to now() with a CAS.
 * Many threads may attempt this at the same time, but only one
 * succeeds (and invalidates that cacheline for everyone else).  The
 * winner then walks the counters of the other shards to tell
 * whether the elapsed span (now - old ds_busy_stamp) was busy, and
 * if it was, charges it to its own local ds_local_busy_time.
 * Summing the local counters back yields an approximate total busy
 * time: very short IOs may be missed, or, conversely, a single
 * short IO may mark a large span as fully busy.
 */

#define PFS_DEVSTAT_BUSY_QUANTUM 1000 /* 1 ms, in microseconds */

static constexpr size_t PFS_DEVSTAT_SHARDS = pfsutil::AccessSpreader::kMaxCpus;
static_assert(PFS_DEVSTAT_SHARDS <= pfsutil::AccessSpreader::kMaxCpus,
    "PFS_DEVSTAT_SHARDS must not exceed the AccessSpreader::cachedCurrent retval");

typedef struct pfs_devstat_shard : pfsutil::cacheline_align_t {
	uint64_t	ds_start_count;
	uint64_t	ds_end_count;
	int64_t		ds_local_busy_time;
	uint64_t	ds_bytes[PFSDEV_REQ_MAX];
	uint64_t	ds_ops[PFSDEV_REQ_MAX];
	int64_t		ds_duration[PFSDEV_REQ_MAX];
} pfs_devstat_shard_t;

/* device statistics */
typedef struct pfs_devstat {
	alignas(pfsutil::kCachelineSize) int64_t ds_busy_stamp;
	char _ds_pad[pfsutil::kCachelineSize - sizeof(int64_t)];

	pfs_devstat_shard_t	*ds_shards; /* Per-CPU shard array */
} pfs_devstat_t;

void pfs_devstat_init(pfs_devstat_t *ds);
void pfs_devstat_uninit(pfs_devstat_t *ds);
void pfs_devstat_io_start(pfs_devstat_t *ds, const pfs_devio_t *io);
void pfs_devstat_io_end(pfs_devstat_t *ds, const pfs_devio_t *io);
int pfs_devstat_snap(int devi, admin_buf_t *ab);

#endif	/* _PFS_DEVSTAT_H_ */
