#ifndef __DCLC_RWLOCK_H__
#define __DCLC_RWLOCK_H__

#include <atomic>

/*
 * TODO: Add the blabla here
 *
 * Disadvantages:
 * - Can't use this lock on code that can fork()
 * - Shouldn't use this on signal handler code (or any kind of lock for that
 * matter)
 *
 *
 */

// Use 0 for writer's "unlocked" and 1 for "locked" state
#define DCLC_RWL_UNLOCKED 0
#define DCLC_RWL_LOCKED 1

// Cache line optimization constants
#define DCLC_CACHE_LINE 64 // Size in bytes of a cache line
#define DCLC_CACHE_PADD (DCLC_CACHE_LINE - sizeof(std::atomic<int>))
#define DCLC_NUMBER_OF_CORES 32
#define DCLC_HASH_RATIO 3
#define DCLC_COUNTERS_RATIO (DCLC_HASH_RATIO * DCLC_CACHE_LINE / sizeof(int))

/* This is not recursive/reentrant */
class DCLCRWLock {
public:
  void init();
  void init(int num_cores);
  void destroy();
  void lock_shared();
  bool try_lock_shared();
  bool unlock_shared();
  void lock();
  bool try_lock();
  bool unlock();
  bool downgrade_lock();
  bool try_upgrade_lock();

private:
  int thread2idx();

private:
  /* Number of cores on the system */
  int numCores;
  /* Length of readers_counters[] */
  int countersLength;
  /* Distributed Counters for Readers */
  std::atomic<int> *readersCounters;
  /* Padding */
  char pad1[DCLC_CACHE_PADD];
  /* lock/unlocked in write-mode */
  std::atomic<int> writersMutex;
};

#endif
