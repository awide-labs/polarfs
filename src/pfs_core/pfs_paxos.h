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

#ifndef	_PFS_PAXOS_H_
#define	_PFS_PAXOS_H_

#include <stddef.h>
#include <stdint.h>

typedef struct pfs_mount 		pfs_mount_t;

#define NAME_ID_SIZE 			48
#define	DEFAULT_MAX_HOSTS		254

#define PFS_LEADER_MAGIC 		0xbabababa
#define PFS_LEADER_CLEAR 		0x11282016
#define PFS_LEADER_VERSION_PRIMARY 	0x00010000
#define PFS_LEADER_VERSION_SECONDARY 	0x00000002
#define PFS_LEADER_UNUSED		0xb0

typedef struct pfs_leader_record {
        uint32_t magic;
        uint32_t version;
        uint32_t flags;
        uint32_t sector_size;
        uint64_t num_hosts;
        uint64_t max_hosts;

        uint8_t  unused[PFS_LEADER_UNUSED];

	/*
	 * Fields for log. The txid range on log is (tail, head].
	 * Tx upto tail have been committed to PBD.
	 */
	uint64_t tail_txid;		/* tx in range (tail, head] is */
	uint64_t head_txid;		/* in log; (-max, tail] is on pbd */
	uint64_t tail_offset;
	uint64_t head_offset;
	uint64_t log_size;
	uint64_t head_lsn;		/* sequence number; (0, head_lsn] */
	uint32_t checksum;
} pfs_leader_record_t;

/* LR size only relating to paxos */
#define	LR_PAXOS_SIZE		offsetof(pfs_leader_record, tail_txid)
#define	LEADER_CHECKSUM_LEN	offsetof(pfs_leader_record, checksum)

#define PFS_OK                   1
#define PFS_NONE                 0    /* unused */
#define PFS_ERROR             -201
#define PFS_AIO_TIMEOUT       -202
#define PFS_WD_ERROR          -203

#define PFS_LEADER_EMAGIC      -223
#define PFS_LEADER_EVERSION    -224
#define PFS_LEADER_ESECTORSIZE -225
#define PFS_LEADER_ENUMHOSTS   -228
#define PFS_LEADER_ECHECKSUM   -229

static inline uint32_t
leader_checksum(struct pfs_leader_record *lr)
{
	return crc32c((uint32_t)~1, (uint8_t *)lr, LEADER_CHECKSUM_LEN);
}

int 	pfs_leader_init(pfs_mount_t *mnt, int num_hosts, int max_hosts,
	    int write_clear, size_t logsize);
int 	pfs_leader_load(pfs_mount_t *mnt);
void 	pfs_leader_unload(pfs_mount_t *mnt);
int 	pfs_leader_write(pfs_mount_t *mnt, pfs_leader_record_t *nl);
int 	pfs_leader_read(pfs_mount_t *mnt, pfs_leader_record_t *leader_ret);

int	paxos_hostid_local_lock(const char *pbdname, int hostid, const char *caller);
void	paxos_hostid_local_unlock(int fd);

int read_leader(pfs_mount_t *mnt, struct pfs_leader_record *lr,
		uint32_t *checksum);

/* ---- per-host RW lease record (one per sector, sector = host_id) ---- */

#define PFS_HOST_MAGIC		0xcafebabe
#define PFS_HOST_FL_RW		0x00000001
#define PFS_HOST_FL_PREPARE	0x00000002

/* Error codes for host record I/O */
#define PFS_HOST_EMAGIC		-241
#define PFS_HOST_ECHECKSUM	-242

#define HOST_CHECKSUM_LEN	offsetof(pfs_host_record_t, hr_checksum)

#define PFS_HOST_RECORD_UNUSED	\
    (512 - sizeof(uint32_t)*4 - sizeof(uint64_t)*3 - sizeof(uint32_t))

typedef struct pfs_host_record {
	uint32_t hr_magic;	  /* PFS_HOST_MAGIC or 0 if empty */
	uint32_t hr_flags;	  /* PFS_HOST_FL_RW, PFS_HOST_FL_PREPARE */
	uint32_t hr_host_id;	  /* 1..max_hosts */
	uint32_t hr_generation;	  /* mount epoch: prev+1 after crash, 1 after clean start */
	uint64_t hr_timestamp;	  /* CLOCK_REALTIME seconds of last renewal */
	uint64_t hr_mbal;	  /* highest ballot promised (Phase 1 prepare) */
	uint64_t hr_bal;	  /* ballot of accepted/acquired lease (Phase 2) */
	uint8_t  hr_unused[PFS_HOST_RECORD_UNUSED];
	uint32_t hr_checksum;	  /* crc32c of all bytes before this field */
} pfs_host_record_t;		  /* exactly 512 bytes */

static_assert(sizeof(pfs_host_record_t) == 512,
    "pfs_host_record_t must be exactly 512 bytes");

int64_t	pfs_paxos_lease_duration(void);
int	pfs_rw_lease_prepare(pfs_mount_t *mnt);
int	pfs_rw_lease_verify_prepare(pfs_mount_t *mnt);
int	pfs_rw_lease_acquire(pfs_mount_t *mnt);
int	pfs_rw_lease_write_foreign(pfs_mount_t *mnt, uint32_t foreign_hostid);
int	pfs_rw_lease_write_foreign_prepare(pfs_mount_t *mnt,
	    uint32_t foreign_hostid, uint32_t generation);
int	pfs_write_raw_host_sector(pfs_mount_t *mnt, uint32_t host_id,
	    const void *src, size_t srclen);
int	pfs_host_record_read(pfs_mount_t *mnt, uint32_t host_id,
	    pfs_host_record_t *hr_ret);
int	pfs_check_host_sector(pfs_mount_t *mnt, uint32_t host_id);
int	pfs_rw_lease_renew(pfs_mount_t *mnt);
void	pfs_rw_lease_release(pfs_mount_t *mnt);
void	paxos_watchdog_open(pfs_mount_t *mnt);
void	paxos_watchdog_pet(pfs_mount_t *mnt);
void	paxos_watchdog_close(pfs_mount_t *mnt);

#endif
