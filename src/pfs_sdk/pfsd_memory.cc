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

#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "pfsd_memory.h"
#include "../ipc/access_spreader.h"

typedef struct pfsd_memory {
	const char	*mt_name;
	pfsutil::MemCounters<>	counters;
} pfsd_memtype_t;

/*
 * Positional (not designated) initializers, in MD_* enum order, with an
 * unsized array: if a future MD_* type is added to the enum but not to this
 * table, the static_assert below fails at compile time. This is the check
 * whose absence let MD_MOUNTARG ship with a NULL mt_name (which would feed a
 * NULL %s to the OOM fprintf).
 */
#define	MEMTYPE_ENTRY(tag)	{ #tag, }
static pfsd_memtype_t	pfsd_mem_type[] = {
	MEMTYPE_ENTRY(MD_NONE),
	MEMTYPE_ENTRY(MD_DIR),
	MEMTYPE_ENTRY(MD_FILE),
	MEMTYPE_ENTRY(MD_MOUNTARG),
	MEMTYPE_ENTRY(MD_PATH),
};
static_assert(sizeof(pfsd_mem_type) / sizeof(pfsd_mem_type[0]) == MD_NTYPE,
    "pfsd_mem_type must cover every MD_* type");

static inline const char *
memtype_name(int type)
{
	return pfsd_mem_type[type].mt_name;
}

static void
memtype_inc(int type, int count, size_t size)
{
	assert(0 < type && type < MD_NTYPE);

	pfsd_mem_type[type].counters.inc(size, count);
}

static void
memtype_dec(int type, size_t size)
{
	assert(0 < type && type < MD_NTYPE);

	pfsd_mem_type[type].counters.dec(size);
}

void *
pfsd_mem_malloc(size_t size, int type)
{
	void *ptr = malloc(size);
	if (ptr == NULL) {
		fprintf(stderr, "malloc failed: type %s, size %zu\n",
		    memtype_name(type), size);
		return NULL;
	}
	memtype_inc(type, 1, malloc_usable_size(ptr));

	return ptr;
}

void *
pfsd_mem_calloc(size_t nelem, size_t elemsize, int type)
{
	void *ptr = calloc(nelem, elemsize);
	if (ptr == NULL) {
		fprintf(stderr, "calloc failed: type %s, nelem %zu, elemsize %zu\n",
		    memtype_name(type), nelem, elemsize);
		return NULL;
	}
	memtype_inc(type, 1, malloc_usable_size(ptr));

	return ptr;
}

char *
pfsd_mem_strdup(const char *s, int type)
{
	if (s == NULL)
		return NULL;

	size_t len = strlen(s) + 1;
	char *p = (char *)malloc(len);
	if (p == NULL) {
		fprintf(stderr, "strdup failed: type %s, size %zu\n",
		    memtype_name(type), len);
		return NULL;
	}
	memcpy(p, s, len);
	memtype_inc(type, 1, malloc_usable_size(p));

	return p;
}

void
pfsd_mem_free(void *ptr, int type)
{
	if (ptr) {
		memtype_dec(type, malloc_usable_size(ptr));
		free(ptr);
	}
}

void *
pfsd_mem_realloc(void *ptr, size_t newsize, int type)
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
pfsd_mem_memalign(void **pp, size_t alignment, size_t size, int type)
{
	int err;

	err = posix_memalign(pp, alignment, size);
	if (err == 0)
		memtype_inc(type, 1, malloc_usable_size(*pp));
	return err;
}

int
pfsd_mem_stat(char *buf, size_t len)
{
	if (buf == NULL || len == 0)
		return -1;

	size_t pos = 0;
	int n;
	long long sballoc = 0, sbfree = 0, balloc, bfree;
	long long scalloc = 0, scfree = 0, calloc, cfree;

	n = snprintf(buf + pos, len - pos, "%-20s %16s %16s %16s %16s\n",
	    "name", "alloc-count", "free-count", "alloc-bytes", "free-bytes");
	if (n < 0)
		return -1;
	pos += (size_t)n;
	if (pos >= len)
		pos = len - 1;

	for (int t = 1; t < MD_NTYPE; t++) {
		pfsutil::MemCounters<>::Totals totals =
		    pfsd_mem_type[t].counters.sum();
		balloc = totals.bytes_alloc;
		bfree = totals.bytes_free;
		calloc = totals.count_alloc;
		cfree = totals.count_free;

		sballoc += balloc;
		sbfree += bfree;
		scalloc += calloc;
		scfree += cfree;

		n = snprintf(buf + pos, len - pos,
		    "%-20s %16lld %16lld %16lld %16lld\n",
		    pfsd_mem_type[t].mt_name, calloc, cfree, balloc, bfree);
		if (n < 0)
			return -1;
		pos += (size_t)n;
		if (pos >= len)
			pos = len - 1;
	}

	n = snprintf(buf + pos, len - pos,
	    "%-20s %16lld %16lld %16lld %16lld\n",
	    "total", scalloc, scfree, sballoc, sbfree);
	if (n < 0)
		return -1;

	return 0;
}
