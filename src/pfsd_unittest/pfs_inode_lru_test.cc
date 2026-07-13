/*
 * pfs_inode_lru_test — regression test for inode LRU eviction.
 *
 * Verifies that pfs_put_inode() LRU eviction correctly checks the eviction
 * candidate's refcnt (in2) rather than the caller's (in).
 *
 * Usage:
 *   pfs_inode_lru_test [-l lru_size] [-n inodes]
 *
 * No block device or pfsd required — uses a synthetic pfs_mount_t with
 * only the inode tree fields initialized.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>

#include "pfs_impl.h"
#include "pfs_mount.h"
#include "pfs_inode.h"
#include "pfs_avl.h"

/* inode numbers well above any real inode range */
#define INO_BASE	1000000

static pfs_mount_t *mnt;
static std::atomic<bool> stop_flag{false};

#define DIE(fmt, ...) do {						\
	fprintf(stderr, "FATAL: " fmt "\n", ##__VA_ARGS__);		\
	exit(EXIT_FAILURE);						\
} while (0)

/*
 * Continuously get/put a single inode to keep in->in_refcnt > 0 during
 * concurrent pfs_put_inode calls from other threads.  This is the condition
 * that triggers the bug: when another thread calls pfs_put_inode for a
 * different inode while this thread holds `anchor`, the eviction check
 * incorrectly tests anchor->in_refcnt (>0) instead of the LRU candidate's.
 */
static void *
anchor_thread(void *arg)
{
	pfs_ino_t anchor_ino = (pfs_ino_t)(uintptr_t)arg;

	while (!stop_flag.load(std::memory_order_relaxed)) {
		pfs_inode_t *in = pfs_get_inode(mnt, anchor_ino);
		if (in == NULL)
			continue;
		/* Hold briefly so concurrent put_inode sees in_refcnt > 0 */
		usleep(100);
		pfs_put_inode(mnt, in);
	}
	return NULL;
}

/*
 * Create a minimal pfs_mount_t with only the inode tree infrastructure.
 * No disk, log, paxos, or any I/O state — just enough for
 * pfs_inode_get / pfs_get_inode / pfs_put_inode to work.
 */
static pfs_mount_t *
create_fake_mount(void)
{
	pfs_mount_t *m;
	int cpu_count;
	uint64_t cpu_shift;

	m = (pfs_mount_t *)calloc(1, sizeof(*m));
	if (m == NULL)
		DIE("calloc(pfs_mount_t) failed");

	pfs_avl_create(&m->mnt_inodetree, pfs_inode_compare,
	    offsetof(pfs_inode_t, in_node));

	m->mnt_inodetree_rwlock = new DCLCRWLock;
	m->mnt_inodetree_rwlock->init();

	cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
	if (cpu_count <= 1)
		cpu_count = 2;
	cpu_shift = 64 - __builtin_clzl(cpu_count - 1);
	m->mnt_num_shards = 1 << cpu_shift;

	m->mnt_inodelist = (pfs_mount_t::mnt_inodelist_t *)
	    calloc(m->mnt_num_shards, sizeof(pfs_mount_t::mnt_inodelist_t));
	if (m->mnt_inodelist == NULL)
		DIE("calloc(inodelist) failed");

	for (int i = 0; i < m->mnt_num_shards; i++) {
		mutex_init(&m->mnt_inodelist[i].mtx);
		TAILQ_INIT(&m->mnt_inodelist[i].list);
	}

	return m;
}

static void
destroy_fake_mount(pfs_mount_t *m)
{
	// See: pfs_destroy_mount
	pfs_inode_t *in = (pfs_inode_t *)pfs_avl_first(&mnt->mnt_inodetree);
	while (in != NULL) {
		pfs_inode_t *tmp = in;
		in = (pfs_inode_t *)pfs_avl_next(&mnt->mnt_inodetree, in);
		pfs_avl_remove(&mnt->mnt_inodetree, tmp);
		// FIXME
		// Dir-index maintain the reference across inodes in the form
		// of pfs_dxent_t->e_in (see 77ddaef8 for details).
		// In PFSD, however, destroy-mount should clean up all inodes
		// left in inodelist LRU set. Throughout this process, we may
		// destroy child inode before its parent due to the random
		// order of avl traversal, leaving reference ('e_in') of parent
		// inode a dangling pointer and crash.
		// We hotfix this by calling a simlilar inode-destroy function.
		// The only difference is that it won't follow the reference
		// of inode.
		pfs_inode_destroy_self(tmp);
	}
	pfs_avl_destroy(&mnt->mnt_inodetree);

	m->mnt_inodetree_rwlock->destroy();
	delete m->mnt_inodetree_rwlock;
	free(m->mnt_inodelist);
	free(m);
}

static void
usage(const char *prog)
{
	printf("usage: %s [-l lru_size] [-n inodes]\n", prog);
	printf("  -l lru_size   inodetree_lru_size to set (default: 100)\n");
	printf("  -n inodes     total inodes to create (default: 3 * lru_size)\n");
}

int
main(int argc, char **argv)
{
	int lru_limit = 100;
	int total_inodes = 0;	/* 0 = auto (3 * lru_limit) */
	int opt;

	while ((opt = getopt(argc, argv, "l:n:h")) != -1) {
		switch (opt) {
		case 'l': lru_limit = atoi(optarg); break;
		case 'n': total_inodes = atoi(optarg); break;
		default:
			usage(argv[0]);
			return (opt == 'h') ? 0 : 1;
		}
	}

	if (lru_limit < 10)
		DIE("lru_size too small: %d (min 10)", lru_limit);
	if (total_inodes == 0)
		total_inodes = lru_limit * 3;
	if (total_inodes <= lru_limit)
		DIE("total inodes (%d) must exceed lru_size (%d)",
		    total_inodes, lru_limit);

	printf("=== pfs_inode_lru_test ===\n");
	printf("lru_size:     %d\n", lru_limit);
	printf("total_inodes: %d\n\n", total_inodes);

	pfs_inodetree_lru_size_set(lru_limit);
	mnt = create_fake_mount();

	/*
	 * Phase 1: populate the inode cache with synthetic inodes.
	 * pfs_inode_get (needload=false) creates + adds + bumps refcnt to 1.
	 */
	pfs_inode_t **inodes = (pfs_inode_t **)calloc(total_inodes, sizeof(*inodes));
	if (inodes == NULL)
		DIE("calloc(%d) failed", total_inodes);

	printf("populating %d inodes...\n", total_inodes);
	for (int i = 0; i < total_inodes; i++) {
		pfs_ino_t ino = INO_BASE + i;
		inodes[i] = pfs_inode_get(mnt, ino);
		if (inodes[i] == NULL)
			DIE("pfs_inode_get failed for ino=%ld", (long)ino);
	}

	uint64_t before = pfs_avl_numnodes(&mnt->mnt_inodetree);
	printf("tree size after populate: %llu\n", (unsigned long long)before);

	/*
	 * Phase 2: release all inodes, but with concurrent get/put on a
	 * subset to keep some in_refcnt > 0 during eviction.
	 *
	 * Start anchor threads that continuously get/put a few inodes.
	 * Then release the rest — each pfs_put_inode should trigger eviction
	 * once numnodes > lru_limit.
	 *
	 * With the bug: eviction checks the anchor's in_refcnt (>0), no
	 * eviction, tree stays at total_inodes.
	 * With the fix: eviction checks the LRU candidate's in_refcnt (0),
	 * tree shrinks toward lru_limit.
	 */
	#define N_ANCHORS 4
	pthread_t tids[N_ANCHORS];
	/* Use the first N_ANCHORS inodes as anchors */
	for (int i = 0; i < N_ANCHORS; i++) {
		pfs_ino_t ino = INO_BASE + i;
		pthread_create(&tids[i], NULL, anchor_thread,
		    (void *)(uintptr_t)ino);
	}

	/* Release all non-anchor inodes (these become eviction candidates) */
	for (int i = N_ANCHORS; i < total_inodes; i++)
		pfs_put_inode(mnt, inodes[i]);

	/* Give anchor threads time to cycle, triggering eviction attempts */
	usleep(500000);

	/* Now release anchor inodes too */
	for (int i = 0; i < N_ANCHORS; i++)
		pfs_put_inode(mnt, inodes[i]);

	/*
	 * Do a few more put/get cycles to give eviction more chances to fire.
	 * Get and immediately put a non-anchor inode — this triggers the
	 * eviction check in pfs_put_inode.
	 */
	for (int i = N_ANCHORS; i < total_inodes && i < N_ANCHORS + 200; i++) {
		pfs_inode_t *in = pfs_get_inode(mnt, INO_BASE + i);
		if (in)
			pfs_put_inode(mnt, in);
	}

	stop_flag.store(true, std::memory_order_relaxed);
	for (int i = 0; i < N_ANCHORS; i++)
		pthread_join(tids[i], NULL);

	uint64_t after = pfs_avl_numnodes(&mnt->mnt_inodetree);
	printf("tree size after eviction: %llu\n", (unsigned long long)after);

	/*
	 * With the fix, the tree should have been trimmed to approximately
	 * lru_limit.  Allow some slack for sharding (eviction is per-shard,
	 * one per cycle) and timing.  The key assertion is that it's
	 * significantly smaller than total_inodes.
	 */
	int threshold = lru_limit + (lru_limit / 2);  /* 150% of limit */
	bool passed = (after <= (uint64_t)threshold);

	printf("threshold:    %d\n", threshold);
	printf("result:       %s\n", passed ? "PASS" : "FAIL (tree not evicted)");

	free(inodes);
	destroy_fake_mount(mnt);

	return passed ? 0 : 1;
}
