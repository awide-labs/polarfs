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

#ifndef	_PFS_UTIL_H_
#define	_PFS_UTIL_H_

#include <sys/time.h>
#include <sys/types.h>
#include <stdint.h>

uint32_t 	crc32c(uint32_t crc, const void *buf, size_t size);
uint64_t	roundup_power2(uint64_t val);
int		strncpy_safe(char *dst, const char *src, size_t n);
uint32_t	crc32c_compute(const void *buf, size_t size, size_t offset);
uint64_t	gettimeofday_us();

static inline int64_t
pfs_timeval_to_us(const struct timeval *tv)
{
	return ((int64_t)tv->tv_sec * 1000000 + (int64_t)tv->tv_usec);
}

static inline void
pfs_us_to_timeval(int64_t us, struct timeval *tv)
{
	tv->tv_sec = (time_t)(us / 1000000);
	tv->tv_usec = (suseconds_t)(us % 1000000);
}

#define	DATA_SET_ATTR(set)	 	\
       	__attribute__((used)) 		\
	__attribute__((section(#set)))

#define	DATA_SET_DECL(type, set)	\
	extern struct type *__start_##set[], *__stop_##set[];

#define	DATA_SET_FOREACH(var, set)	\
	for (var = __start_##set; var < __stop_##set; var++)

typedef struct oidvect {
	uint64_t	*ov_buf;
	size_t		ov_size;
	int		ov_next;
	int32_t		*ov_holeoff_buf;
} oidvect_t;
void 	oidvect_init(oidvect_t *ov);
int 	oidvect_push(oidvect_t *ov, uint64_t val, int32_t holeoff);
uint64_t oidvect_pop(oidvect_t *ov);
void 	oidvect_fini(oidvect_t *ov);
static inline int
oidvect_begin(const oidvect_t *ov)
{
	return 0;
}

static inline int
oidvect_end(const oidvect_t *ov)
{
	return ov->ov_next;
}

static inline uint64_t
oidvect_get(const oidvect_t *ov, int indx)
{
	return ov->ov_buf[indx];
}

static inline int32_t
oidvect_get_holeoff(const oidvect_t *ov, int indx)
{
	return ov->ov_holeoff_buf[indx];
}

typedef struct pfs_printer {
	void	*pr_dest;
	int	(*pr_func)(void *dest, const char *fmt, va_list ap);
} pfs_printer_t;
int	pfs_printf(pfs_printer_t *pr, const char *fmt, ...);

#endif
