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
#undef	 NDEBUG
#include <assert.h>
#include <errno.h>
#include <execinfo.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>

#include "pfs_impl.h"
#include "pfs_util.h"
#include "pfs_memory.h"
#include "pfs_trace.h"

#include <folly/hash/Checksum.h>

uint32_t
crc32c(uint32_t crc, const void *buf, size_t size)
{
  return folly::crc32c(static_cast<const unsigned char *>(buf), size, crc);
}

uint64_t
roundup_power2(uint64_t val)
{
	val--;
	val |= val >> 1;
	val |= val >> 2;
	val |= val >> 4;
	val |= val >> 8;
	val |= val >> 16;
	val |= val >> 32;
	val++;
	return val;
}

int
strncpy_safe(char *dst, const char *src, size_t n)
{
	size_t ncopy;

	assert(n > 0);
	(void)strncpy(dst, src, n);
	dst[n - 1] = '\0';
	if ((ncopy = strlen(dst)) != strlen(src))
		return -1;
	return (int)ncopy;
}

uint32_t
crc32c_compute(const void *buf, size_t size, size_t offset)
{
	char dup[size];
	uint32_t rv, *crcp;

	assert((offset + sizeof(uint32_t)) <= size);

	memcpy(dup, buf, size);
	crcp = (uint32_t *)(dup + offset);
	*crcp = 0;

	rv = crc32c((uint32_t)~1, (uint8_t *)(dup), size);
	return rv;
}

void
oidvect_init(oidvect_t *ov)
{
	ov->ov_buf = NULL;
	ov->ov_next = 0;
	ov->ov_size = 0;
	ov->ov_holeoff_buf = NULL;
}

int
oidvect_push(oidvect_t *ov, uint64_t val, int32_t holeoff)
{
#define	OIDV_INC	512
	if (ov->ov_next >= (int)ov->ov_size) {
		void *tmp;
		ov->ov_size += OIDV_INC;
		tmp = pfs_mem_realloc(ov->ov_buf,
		    ov->ov_size * sizeof(val), M_OIDV);
		if (tmp == NULL) {
			ov->ov_size -= OIDV_INC;
			return -ENOMEM;
		}
		ov->ov_buf = (uint64_t *)tmp;

		tmp = pfs_mem_realloc(ov->ov_holeoff_buf, 
		    ov->ov_size * sizeof(holeoff), M_OIDV_HOLEOFF);
		if (tmp == NULL) {
			ov->ov_size -= OIDV_INC;
			return -ENOMEM;
		}
		ov->ov_holeoff_buf = (int32_t *)tmp;
	}

	ov->ov_buf[ov->ov_next] = val;
	ov->ov_holeoff_buf[ov->ov_next] = holeoff;
	ov->ov_next++;
	return 0;
}

uint64_t
oidvect_pop(oidvect_t *ov)
{
	if (ov->ov_next > 0)
		return ov->ov_buf[--ov->ov_next];
	return (uint64_t)-1;
}

void
oidvect_fini(oidvect_t *ov)
{
	if (ov->ov_buf) {
		pfs_mem_free(ov->ov_buf, M_OIDV);
		ov->ov_buf = NULL;
	}

	if (ov->ov_holeoff_buf) {
		pfs_mem_free(ov->ov_holeoff_buf, M_OIDV_HOLEOFF);
		ov->ov_holeoff_buf = NULL;
	}
	ov->ov_next = 0;
	ov->ov_size = 0;
}

int
pfs_printf(pfs_printer_t *pr, const char *fmt, ...)
{
	int rv;
	va_list ap;

	va_start(ap, fmt);
	if (pr == NULL) {
		rv = vprintf(fmt, ap);
	} else {
		rv = (*pr->pr_func)(pr->pr_dest, fmt, ap);
	}
	va_end(ap);

	return rv;
}

void
pfs_abort(const char *action, const char *cond, const char *func, int line)
{
#define	SYM_SIZE	128
	void *buf[SYM_SIZE];
	int nsym;
	char **syms;

	pfs_etrace("failed to %s %s at %s: %d\n", action, cond, func, line);
	nsym = backtrace(buf, SYM_SIZE);
	syms = backtrace_symbols(buf, nsym);
	for (int i = 0; i < nsym; i++)
		pfs_etrace("%s\n", syms[i]);
	free(syms);

	abort();
}

uint64_t
gettimeofday_us()
{
	struct timeval now;
	int err = gettimeofday(&now, NULL);
	PFS_VERIFY(err == 0);
	(void)err;
	return now.tv_sec * 1000000 + now.tv_usec;
}
