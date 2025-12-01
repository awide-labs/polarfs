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

#include <atomic>
#include <sys/queue.h>

#include <malloc.h>
#include <stdlib.h>

#include "pfs_impl.h"
#include "pfs_admin.h"

constexpr size_t MEMCOUNTER_SHARDS = 64;
thread_local int tls_memcounter_idx = pthread_self() % MEMCOUNTER_SHARDS;

typedef struct pfs_memtype {
	const char 	*mt_name;
	struct {
		std::atomic<ssize_t>		mt_bytes_alloc;
		std::atomic<ssize_t>		mt_bytes_free;
		std::atomic<int64_t>		mt_count_alloc;
		std::atomic<int64_t>		mt_count_free;
		char		mt_padding[64 - sizeof(ssize_t) * 2 - sizeof(int64_t) * 2];
	} conters[MEMCOUNTER_SHARDS];
} pfs_memtype_t;

#define	MEMTYPE_ENTRY(tag)	[tag] = { #tag, }
static pfs_memtype_t	pfs_mem_type[M_NTYPE] = {
	MEMTYPE_ENTRY(M_NONE),
	MEMTYPE_ENTRY(M_SECTOR),
	MEMTYPE_ENTRY(M_FRAG),
	MEMTYPE_ENTRY(M_TX),
	MEMTYPE_ENTRY(M_TXOP),
	MEMTYPE_ENTRY(M_TXCB),
	MEMTYPE_ENTRY(M_REQWAIT),
	MEMTYPE_ENTRY(M_DBLKV),
	MEMTYPE_ENTRY(M_CHUNKV),
	MEMTYPE_ENTRY(M_CHDESC),
	MEMTYPE_ENTRY(M_TLS),
	MEMTYPE_ENTRY(M_INODE),
	MEMTYPE_ENTRY(M_DIR),
	MEMTYPE_ENTRY(M_FILE),
	MEMTYPE_ENTRY(M_FILEHOLE),
	MEMTYPE_ENTRY(M_CHUNK),
	MEMTYPE_ENTRY(M_MOUNT),
	MEMTYPE_ENTRY(M_OBJBUFV),
	MEMTYPE_ENTRY(M_METASET),
	MEMTYPE_ENTRY(M_ANODEV),
	MEMTYPE_ENTRY(M_PAXOS_SECTOR),
	MEMTYPE_ENTRY(M_ADMINFO),
	MEMTYPE_ENTRY(M_ADMBUF),
	MEMTYPE_ENTRY(M_CMDINFO),
	MEMTYPE_ENTRY(M_SECTHDR),
	MEMTYPE_ENTRY(M_SECTBUF),
	MEMTYPE_ENTRY(M_OIDV),
	MEMTYPE_ENTRY(M_IO_TMPBUF),
	MEMTYPE_ENTRY(M_ZERO_BUF),
	MEMTYPE_ENTRY(M_DEV_IO),
	MEMTYPE_ENTRY(M_POLAR_DEV),
	MEMTYPE_ENTRY(M_POLAR_IOQ),
	MEMTYPE_ENTRY(M_PANGU_DEV),
	MEMTYPE_ENTRY(M_PANGU_IOQ),
	MEMTYPE_ENTRY(M_PANGU_TASK),
	MEMTYPE_ENTRY(M_DISK_DEV),
	MEMTYPE_ENTRY(M_DISK_IOQ),
	MEMTYPE_ENTRY(M_DISK_DIOBUF),
	MEMTYPE_ENTRY(M_DENO_VECT),
	MEMTYPE_ENTRY(M_CHUNK_META),
	MEMTYPE_ENTRY(M_CHUNK_READSTREAM),
	MEMTYPE_ENTRY(M_CHUNK_WRITESTREAM),
	MEMTYPE_ENTRY(M_CHUNK_METABUF),
	MEMTYPE_ENTRY(M_CHUNK_CRCBUF),
	MEMTYPE_ENTRY(M_CHUNK_BLOCKUSED),
	MEMTYPE_ENTRY(M_NAMECACHE),
	MEMTYPE_ENTRY(M_OIDV_HOLEOFF),
	MEMTYPE_ENTRY(M_DXENT),
	MEMTYPE_ENTRY(M_INSTK_VEC),
	MEMTYPE_ENTRY(M_CONFIG),
	MEMTYPE_ENTRY(M_CONFIG_SECT),
	MEMTYPE_ENTRY(M_CONFIG_KV),
	MEMTYPE_ENTRY(M_FDTBL_PTR),
	MEMTYPE_ENTRY(M_INODE_BLK_TABLE),
};

static inline const char *
memtype_name(int type)
{
	return pfs_mem_type[type].mt_name;
}

static void
memtype_inc(int type, int count, size_t size)
{
	PFS_ASSERT(0 < type && type < M_NTYPE);

	auto &cell = pfs_mem_type[type].conters[tls_memcounter_idx];
	cell.mt_bytes_alloc.fetch_add(size, std::memory_order_relaxed);
	cell.mt_count_alloc.fetch_add(count, std::memory_order_relaxed);
}

static void
memtype_dec(int type, size_t size)
{
	PFS_ASSERT(0 < type && type < M_NTYPE);

	auto &cell = pfs_mem_type[type].conters[tls_memcounter_idx];
	cell.mt_bytes_free.fetch_add(size, std::memory_order_relaxed);
	cell.mt_count_free.fetch_add(1, std::memory_order_relaxed);
}

void *
pfs_mem_malloc(size_t size, int type)
{
	void *ptr;

	ptr = malloc(size);
	if (ptr == NULL) {
		pfs_etrace("malloc failed: type %s, size %zu\n",
		    memtype_name(type), size);
		return NULL;
	}
	memset(ptr, 0, size);
	memtype_inc(type, 1, malloc_usable_size(ptr));

	return ptr;
}

void
pfs_mem_free(void *ptr, int type)
{
	if (ptr)
		memtype_dec(type, malloc_usable_size(ptr));
	free(ptr);
}

void *
pfs_mem_realloc(void *ptr, size_t newsize, int type)
{
	void *newptr;
	int inc;
	size_t oldsize;

	if (ptr) {
		oldsize = malloc_usable_size(ptr);
		inc = 0;
	} else {
		oldsize = 0;
		inc = 1;
	}
	newptr = realloc(ptr, newsize);
	if (newptr) {
		memtype_inc(type, inc, malloc_usable_size(newptr) - oldsize);
	}
	return newptr;
}

int
pfs_mem_memalign(void **pp, size_t alignment, size_t size, int type)
{
	int err;

	err = posix_memalign(pp, alignment, size);
	if (err == 0)
		memtype_inc(type, 1, malloc_usable_size(*pp));
	return err;
}

int
pfs_mem_stat(admin_buf_t *ab)
{
	int n, t;
	pfs_memtype_t *mt;
	ssize_t sballoc, sbfree, balloc, bfree;
	int64_t scalloc, scfree, calloc, cfree;

	n = pfs_adminbuf_printf(ab, "%-20s %16s %16s %16s %16s\n",
	    "name", "alloc-count", "free-count", "alloc-bytes", "free-bytes");
	if (n < 0)
		return n;

	sballoc = sbfree = 0;
	scalloc = scfree = 0;
	for (t = 1; t < M_NTYPE; t++) {
		mt = &pfs_mem_type[t];

		balloc = bfree = 0;
		calloc = cfree = 0;

		for (int shard = 0; shard < MEMCOUNTER_SHARDS; shard++) {
			auto &cell = pfs_mem_type[t].conters[shard];
			balloc += cell.mt_bytes_alloc.load(std::memory_order_relaxed);
			bfree += cell.mt_bytes_free.load(std::memory_order_relaxed);
			calloc += cell.mt_count_alloc.load(std::memory_order_relaxed);
			cfree += cell.mt_count_free.load(std::memory_order_relaxed);
		}

		sballoc += balloc;
		sbfree += bfree;
		scalloc += calloc;
		scfree += cfree;

		n = pfs_adminbuf_printf(ab,
		    "%-20s %16lld %16lld %16lld %16lld\n",
		    mt->mt_name, calloc, cfree, balloc, bfree);
		if (n < 0)
			return n;
	}

	n = pfs_adminbuf_printf(ab,
	    "%-20s %16lld %16lld %16lld %16lld\n",
	    "total", scalloc, scfree, sballoc, sbfree);
	if (n < 0)
		return n;

	return 0;
}
