/*
 * pfs_hole_bench: Benchmark for concurrent hole-filling writes.
 *
 * Mounts a PFS filesystem in RW mode, preallocates N files (each
 * FILE_SIZE bytes), then starts N threads. Each thread writes 8KB
 * pages sequentially into its own file, filling the block holes.
 *
 * This exercises the writemodify_commit path (hole shrink without
 * size change) to measure the impact of the TXT_HOLE_WRITE rdlock
 * optimization.
 *
 * Usage:
 *   pfs_hole_bench -C <cluster> -D <device> [-n threads] [-s file_size_mb] [-p page_size]
 */

#include "pfs_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <atomic>

static char cluster[128];
static char device[128];
static int  nthreads = 4;
static int  file_size_mb = 16;		/* per-file size in MB */
static int  page_size = 8192;		/* write granularity */

static std::atomic<int64_t> g_total_pages{0};	/* global page counter */
static struct timespec g_t0;			/* global start time */

#define DIE(fmt, ...) do {						\
	fprintf(stderr, "FATAL: " fmt "\n", ##__VA_ARGS__);		\
	exit(EXIT_FAILURE);						\
} while (0)

static double
timespec_diff_sec(struct timespec *start, struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) +
	    (end->tv_nsec - start->tv_nsec) / 1e9;
}

/* ------------------------------------------------------------------ */

typedef struct {
	int		thread_id;
	int		fd;
	off_t		file_size;
	int		page_size;
	/* results */
	int64_t		pages_written;
	double		elapsed_sec;
} worker_arg_t;

/*
 * Worker thread: write page_size pages sequentially from offset 0
 * to file_size. The file is already fallocated (blocks exist, holes
 * are full-block). Each pfs_pwrite shrinks the hole — this is the
 * hot path we're benchmarking.
 */
static void *
worker(void *arg)
{
	worker_arg_t *wa = (worker_arg_t *)arg;
	char *buf;
	struct timespec t0, t1;
	off_t off;
	ssize_t rv;

	buf = (char *)malloc(wa->page_size);
	if (!buf)
		DIE("malloc(%d) failed", wa->page_size);

	/* Fill buffer with recognizable pattern */
	memset(buf, 'A' + (wa->thread_id % 26), wa->page_size);

	clock_gettime(CLOCK_MONOTONIC, &t0);

	int64_t total_pages = wa->file_size / wa->page_size;
	int pct_step = 1;	/* report every 1% */
	int64_t next_report = total_pages * pct_step / 100;

	wa->pages_written = 0;
	for (off = 0; off < wa->file_size; off += wa->page_size) {
		rv = pfs_pwrite(wa->fd, buf, wa->page_size, off);
		if (rv != wa->page_size) {
			fprintf(stderr, "T%d: pfs_pwrite at off=%ld returned %ld (err=%s)\n",
			    wa->thread_id, (long)off, (long)rv, strerror(errno));
			break;
		}
		wa->pages_written++;
		g_total_pages.fetch_add(1, std::memory_order_relaxed);
		if (wa->pages_written >= next_report) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = timespec_diff_sec(&g_t0, &now);
			int64_t gp = g_total_pages.load(std::memory_order_relaxed);
			int pct = (int)(wa->pages_written * 100 / total_pages);
			fprintf(stderr, "  T%d: %3d%%  %ld/%ld pages  "
			    "%.1f MB/s  (total: %.1f MB/s, %ld pages)\n",
			    wa->thread_id, pct,
			    (long)wa->pages_written, (long)total_pages,
			    (wa->pages_written * (double)wa->page_size / (1 << 20))
			    / elapsed,
			    (gp * (double)wa->page_size / (1 << 20)) / elapsed,
			    (long)gp);
			next_report += total_pages * pct_step / 100;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);
	wa->elapsed_sec = timespec_diff_sec(&t0, &t1);

	free(buf);
	return NULL;
}

/* ------------------------------------------------------------------ */

static void
usage(const char *prog)
{
	printf("usage: %s -C <cluster> -D <device> [options]\n", prog);
	printf("  -C cluster       cluster name\n");
	printf("  -D device        PBD device name\n");
	printf("  -n threads       number of concurrent writers (default: 4)\n");
	printf("  -s size_mb       per-file size in MB (default: 16)\n");
	printf("  -p page_size     write page size in bytes (default: 8192)\n");
}

int
main(int argc, char **argv)
{
	int opt, i, rv;

	while ((opt = getopt(argc, argv, "C:D:n:s:p:h")) != -1) {
		switch (opt) {
		case 'C': strncpy(cluster, optarg, sizeof(cluster) - 1); break;
		case 'D': strncpy(device, optarg, sizeof(device) - 1); break;
		case 'n': nthreads = atoi(optarg); break;
		case 's': file_size_mb = atoi(optarg); break;
		case 'p': page_size = atoi(optarg); break;
		default:
			usage(argv[0]);
			return (opt == 'h') ? 0 : 1;
		}
	}

	if (!cluster[0] || !device[0]) {
		usage(argv[0]);
		return 1;
	}
	if (nthreads < 1 || nthreads > 256)
		DIE("bad thread count: %d", nthreads);
	if (page_size < 512 || page_size > (4 << 20))
		DIE("bad page size: %d", page_size);

	off_t file_size = (off_t)file_size_mb << 20;

	printf("=== pfs_hole_bench ===\n");
	printf("cluster:   %s\n", cluster);
	printf("device:    %s\n", device);
	printf("threads:   %d\n", nthreads);
	printf("file_size: %d MB\n", file_size_mb);
	printf("page_size: %d bytes\n", page_size);
	printf("\n");

	/* ---- Mount ---- */
	rv = pfs_mount(cluster, device, 1, PFS_RDWR);
	if (rv != 0)
		DIE("pfs_mount(%s, %s) failed: %d", cluster, device, rv);
	printf("Mounted %s\n", device);

	/* ---- Create and fallocate files ---- */
	int *fds = (int *)calloc(nthreads, sizeof(int));
	char path[256];

	printf("Creating %d files, %d MB each...\n", nthreads, file_size_mb);
	for (i = 0; i < nthreads; i++) {
		snprintf(path, sizeof(path), "/%s/hole_bench_%d", device, i);

		/* Remove stale file if it exists */
		pfs_unlink(path);

		fds[i] = pfs_open(path, O_CREAT | O_RDWR, 0644);
		if (fds[i] < 0)
			DIE("pfs_open(%s) failed: %d", path, fds[i]);

		/* Preallocate — this allocates blocks but keeps holes.
		 * FALLOC_FL_KEEP_SIZE is NOT set, so file size grows. */
		rv = pfs_posix_fallocate(fds[i], 0, file_size);
		if (rv != 0)
			DIE("pfs_posix_fallocate(%s, %ld) failed: %d",
			    path, (long)file_size, rv);
	}
	printf("Files created and preallocated.\n\n");

	/* ---- Run benchmark ---- */
	pthread_t *tids = (pthread_t *)calloc(nthreads, sizeof(pthread_t));
	worker_arg_t *args = (worker_arg_t *)calloc(nthreads, sizeof(worker_arg_t));

	struct timespec wall_t0, wall_t1;
	g_total_pages.store(0, std::memory_order_relaxed);
	clock_gettime(CLOCK_MONOTONIC, &wall_t0);
	g_t0 = wall_t0;

	for (i = 0; i < nthreads; i++) {
		args[i].thread_id = i;
		args[i].fd = fds[i];
		args[i].file_size = file_size;
		args[i].page_size = page_size;
		rv = pthread_create(&tids[i], NULL, worker, &args[i]);
		if (rv != 0)
			DIE("pthread_create failed: %s", strerror(rv));
	}

	for (i = 0; i < nthreads; i++)
		pthread_join(tids[i], NULL);

	clock_gettime(CLOCK_MONOTONIC, &wall_t1);
	double wall_sec = timespec_diff_sec(&wall_t0, &wall_t1);

	/* ---- Report ---- */
	printf("=== Results ===\n");
	int64_t total_pages = 0;
	int64_t total_bytes = 0;
	for (i = 0; i < nthreads; i++) {
		printf("  T%d: %ld pages in %.3f s (%.1f MB/s)\n",
		    i, (long)args[i].pages_written, args[i].elapsed_sec,
		    (args[i].pages_written * (double)page_size / (1 << 20))
		    / args[i].elapsed_sec);
		total_pages += args[i].pages_written;
		total_bytes += args[i].pages_written * page_size;
	}
	printf("\n");
	printf("Wall time:       %.3f s\n", wall_sec);
	printf("Total pages:     %ld\n", (long)total_pages);
	printf("Total data:      %.1f MB\n", total_bytes / (double)(1 << 20));
	printf("Aggregate:       %.1f MB/s\n",
	    total_bytes / (double)(1 << 20) / wall_sec);
	printf("Pages/sec:       %.0f\n", total_pages / wall_sec);
	printf("\n");

	/* ---- Cleanup ---- */
	for (i = 0; i < nthreads; i++) {
		pfs_close(fds[i]);
		snprintf(path, sizeof(path), "/%s/hole_bench_%d", device, i);
		pfs_unlink(path);
	}

	pfs_umount(device);
	printf("Unmounted. Done.\n");

	free(fds);
	free(tids);
	free(args);
	return 0;
}
