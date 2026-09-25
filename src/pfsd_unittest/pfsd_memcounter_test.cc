/*
 * pfsd_memcounter_test: runtime coverage for the sharded memory counters
 * (pfsutil::MemCounters<>) plus the client-side allocation overhead.
 *
 * Spread: pin N threads to distinct CPUs and assert the counters touch more
 *         than one shard (GTEST_SKIP when fewer than 2 usable CPUs).
 * Correctness: concurrent inc/dec of known amounts must fold to exact totals.
 * Overhead: malloc/free vs pfsd_mem_malloc/pfsd_mem_free (DISABLED_ so it
 *           stays out of the CI failure gate).
 */

#include <gtest/gtest.h>

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <thread>
#include <vector>

#include "pfsd_memory.h"
#include "ipc/access_spreader.h"

namespace {

int usable_cpu_count()
{
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0)
    return 0;
  int n = 0;
  for (int c = 0; c < CPU_SETSIZE; ++c)
    if (CPU_ISSET(c, &set))
      ++n;
  return n;
}

bool pin_to_cpu(int cpu)
{
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

TEST(MemCountersTest, SpreadAcrossShards)
{
  const int ncpu = usable_cpu_count();
  if (ncpu < 2)
    GTEST_SKIP() << "fewer than 2 usable CPUs";

  cpu_set_t set;
  CPU_ZERO(&set);
  sched_getaffinity(0, sizeof(set), &set);
  std::vector<int> cpus;
  for (int c = 0; c < CPU_SETSIZE; ++c)
    if (CPU_ISSET(c, &set))
      cpus.push_back(c);

  const int nthreads = std::min<int>(static_cast<int>(cpus.size()), 8);
  cpus.resize(nthreads);

  pfsutil::MemCounters<> counters;
  std::vector<size_t> stripes(nthreads, 0);
  std::vector<std::thread> threads;
  threads.reserve(nthreads);
  for (int i = 0; i < nthreads; ++i) {
    threads.emplace_back([&, i]() {
      pin_to_cpu(cpus[i]);
      stripes[i] = pfsutil::AccessSpreader::cachedCurrent(64);
      for (int k = 0; k < 8; ++k)
        counters.inc(1, 1);
    });
  }
  for (auto &t : threads)
    t.join();

  const std::set<size_t> distinct(stripes.begin(), stripes.end());
  EXPECT_GT(distinct.size(), 1u) << "pinned threads should spread across shards";
  EXPECT_EQ(counters.shards_in_use(), distinct.size())
      << "touched shards must match the predicted stripes";
}

TEST(MemCountersTest, SumMatchesIncDec)
{
  pfsutil::MemCounters<> counters;
  constexpr int kThreads = 4;
  constexpr int64_t kIter = 100000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&]() {
      for (int64_t i = 0; i < kIter; ++i) {
        counters.inc(16, 1);
        counters.dec(16);
      }
    });
  }
  for (auto &t : threads)
    t.join();

  const auto totals = counters.sum();
  EXPECT_EQ(totals.count_alloc, kThreads * kIter);
  EXPECT_EQ(totals.count_free, kThreads * kIter);
  EXPECT_EQ(totals.bytes_alloc, kThreads * kIter * 16);
  EXPECT_EQ(totals.bytes_free, kThreads * kIter * 16);
}

// Escape barrier so the compiler cannot elide the malloc/free pair as dead
// code (which would otherwise report a meaningless 0 ns/op at -O2).
static inline void
bench_escape(void *p)
{
  asm volatile("" : : "r"(p) : "memory");
}

// Overhead benchmark. DISABLED_ keeps it out of the CI failure gate; run with
// --gtest_also_run_disabled_tests.
TEST(MemCountersTest, DISABLED_MallocFreeOverhead)
{
  const size_t kSizes[] = {64, 4096};
  constexpr int kIters = 100000;

  for (size_t sz : kSizes) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
      void *p = malloc(sz);
      bench_escape(p);
      free(p);
    }
    auto t1 = std::chrono::steady_clock::now();
    const double plain_ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
      void *p = pfsd_mem_malloc(sz, MD_FILE);
      bench_escape(p);
      pfsd_mem_free(p, MD_FILE);
    }
    t1 = std::chrono::steady_clock::now();
    const double pfsd_ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;

    std::printf("size=%5zu  plain=%.1f ns/op  pfsd=%.1f ns/op  ratio=%.2fx\n",
                sz, plain_ns, pfsd_ns, pfsd_ns / plain_ns);
  }

  constexpr int kThreads = 8;
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&]() {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int i = 0; i < kIters / kThreads; ++i) {
        void *p = pfsd_mem_malloc(64, MD_FILE);
        bench_escape(p);
        pfsd_mem_free(p, MD_FILE);
      }
    });
  }
  auto t0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  for (auto &t : threads)
    t.join();
  auto t1 = std::chrono::steady_clock::now();
  const double total_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();
  std::printf("contended %d threads: %.1f ms total (%d ops)\n", kThreads,
              total_ms, kIters);
}

} // namespace
