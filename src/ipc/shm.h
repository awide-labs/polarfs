#pragma once

#include "SharedIndexedMemPool.h"
#include "memfd.h"
#include "pfsd_common.h"
#include "proto.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace ipc {

struct SharedBufferInfo {
  int id;
  uintptr_t start;
  size_t len;
};

struct SharedMemoryPools {
  struct AllocResult {
    off_t offset;
    size_t size;
    void *ptr;
    int bufferId;
  };

  SharedMemoryPools() {}

  uint64_t addPool(std::unique_ptr<SharedIndexedMemPool<>> pool) {
    auto id = nextId_.fetch_add(1, std::memory_order_relaxed);
    if (pool->elemSize() % 1024 != 0) {
      throw std::runtime_error("Mempool element size must be multiple of 1024");
    }
    if (!folly::isPowTwo(pool->elemSize() / 1024)) {
      throw std::runtime_error("Mempool element size must be pow of two KiB");
    }
    pools_.push_back(std::move(pool));
    poolIds_.push_back(id);
    poolIndexById_[id] = pools_.size() - 1;

    populateSizeIndices();

    return id;
  }

  uint64_t addRequestsPool(std::unique_ptr<SharedIndexedMemPool<>> pool) {
    auto id = nextId_.fetch_add(1, std::memory_order_relaxed);
    requestsPoolId_ = id;
    requestsPool_ = std::move(pool);
    return id;
  }

  uint64_t addRawBuffer(std::unique_ptr<MemFd> buf) {
    auto id = nextId_.fetch_add(1, std::memory_order_relaxed);
    rawBuffers_.push_back(std::move(buf));
    rawBufferIds_.push_back(id);
    return id;
  }

  /*
   * Allocate a memory buffer of the given size from one of the memory pools
   */
  bool alloc(size_t size, AllocResult &r) {
    // Calculate the power of two exponent (log2) of the requested size in KiB
    const size_t sizeKiB = (size + 1023) / 1024;
    const size_t roundSize = folly::nextPowTwo(sizeKiB);
    const unsigned bucket = folly::findLastSet(roundSize) - 1;

    // We don't have a pool capable to serve allocations this large
    if (bucket >= poolSizeIndices_.size()) {
      PFSD_CLIENT_LOG("Failed to allocate IO buffer of size %zu", size);
      return false;
    }

    for (;;) {
      int tryBucket = bucket;
      // Start with smallest pool able to serve requests of our size
      while (tryBucket < poolSizeIndices_.size()) {
        unsigned index = poolSizeIndices_[tryBucket];
        auto &pool = pools_[index];
        uint32_t allocIndex = pool->allocIndex();
        if (allocIndex == 0) {
          // Try larger pools
          ++tryBucket;
          continue;
        }

        // Success
        r.bufferId = poolIds_[index];
        r.offset = pool->offsetOf(allocIndex);
        r.size = size;
        r.ptr = (*pool)[allocIndex];

        return true;
      }

      // All pools are exhausted, wait for someone to release an object
      waitForFreeSpace();
    }
  }

  void allocRequest(AllocResult &r) {
    auto &pool = requestsPool_;
    for (;;) {
      uint32_t allocIndex = pool->allocIndex();
      if (allocIndex == 0) {
        // Wait for someone to release an object
        waitForFreeSpace();
        continue;
      }

      r.bufferId = requestsPoolId_;
      r.offset = pool->offsetOf(allocIndex);
      r.size = sizeof(Request);
      r.ptr = (*pool)[allocIndex];

      return;
    }
  }

  void free(AllocResult &r) {
    auto &pool = pools_[poolIndexById_[r.bufferId]];
    uint32_t elemIndex = pool->locateElem(r.ptr);
    pool->recycleIndex(elemIndex);
    maybeWakeWaiters();
  }

  void freeRequest(AllocResult &r) {
    auto &pool = requestsPool_;
    uint32_t elemIndex = pool->locateElem(r.ptr);
    pool->recycleIndex(elemIndex);
  }

  void *resolvePtr(uint64_t bufId, off_t offset) {
    if (bufId == requestsPoolId_) {
      return static_cast<char *>(requestsPool_->memfd().buf()) + offset;
    }
    auto &pool = pools_[poolIndexById_[bufId]];
    return static_cast<char *>(pool->memfd().buf()) + offset;
  }

  struct Desc {
    uint64_t id;
    size_t size;
    int fd;

    Desc() {}
    Desc(const Desc &) = default;
    Desc(Desc &&) = default;
    Desc &operator=(const Desc &) = default;
    Desc &operator=(Desc &&) = default;
    Desc(uint64_t id, size_t size, int fd) : id(id), size(size), fd(fd) {}
  };

  std::vector<Desc> allBuffers() {
    std::vector<Desc> rv;

    for (size_t i = 0; i < pools_.size(); i++) {
      Desc desc(poolIds_[i], pools_[i]->memfd().size(), pools_[i]->fd());
      rv.push_back(desc);
    }

    for (size_t i = 0; i < rawBuffers_.size(); i++) {
      Desc desc(rawBufferIds_[i], rawBuffers_[i]->size(), rawBuffers_[i]->fd());
      rv.push_back(desc);
    }

    Desc desc(requestsPoolId_, requestsPool_->memfd().size(),
              requestsPool_->fd());
    rv.push_back(desc);

    return rv;
  }

  void clear() {
    pools_.clear();
    poolIds_.clear();
    rawBuffers_.clear();
    rawBufferIds_.clear();
    poolIndexById_.clear();
    poolSizeIndices_.clear();
    nextId_ = 0;
  }

private:
  /**
   * Builds a mapping from pool element size expressed as pow2 in KiB
   * to the index of the pool that can handle allocations of that size.
   *
   * Result: poolSizeIndices_[n] gives the index of the smallest pool
   * that can serve allocations up to 2^(n) KiB in size.
   *
   * This mapping is used for constant time lookups in alloc() method.
   */
  void populateSizeIndices() {
    // Build a mapping from pool index to its "power" value.
    // Power value is the largest power of two <= pool element size in KiB 
    std::vector<std::pair<unsigned, unsigned>> powers(pools_.size());
    unsigned maxPower = 0;
    for (unsigned i = 0; i < pools_.size(); i++) {
      auto &p = pools_[i];
      auto size = p->elemSize() / 1024;
      auto power = size > 1 ? folly::findLastSet(size) - 1 : 0;
      powers.emplace_back(i, power);
      maxPower = std::max(power, maxPower);
    }

    // Sort powers descending
    std::sort(powers.begin(), powers.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });

    // Build a mapping.
    // Start with largest pool, it can serve requests of size ranging
    // from 0 to it's power value. Assign poolSizeIndices_[0..pool_pow]
    // Repeat for smaller pools.
    // In the end we should get something like;
    // poolSizeMapping_[0] = &pool-of-size-1024
    // poolSizeMapping_[1] = &pool-of-size-4096
    // poolSizeMapping_[2] = &pool-of-size-4096
    // poolSizeMapping_[3] = &pool-of-size-8192
    // poolSizeMapping_[4] = &pool-of-size-16384
    // poolSizeMapping_[5] = &pool-of-size-65536
    // poolSizeMapping_[6] = &pool-of-size-65536
    // poolSizeMapping_[7] = &pool-of-size-1048576
    // poolSizeMapping_[8] = &pool-of-size-1048576
    // poolSizeMapping_[9] = &pool-of-size-1048576
    // poolSizeMapping_[10] = &pool-of-size-1048576
    poolSizeIndices_.resize(maxPower + 1);
    for (auto &p : powers) {
      for (unsigned i = 0; i <= p.second; i++) {
        poolSizeIndices_[i] = p.first;
      }
    }
  }

  void waitForFreeSpace() {
    std::unique_lock lk(waitMutex_);
    empty_.store(true, std::memory_order_release);
    waitCV_.wait_for(lk, std::chrono::milliseconds(20));
  }

  void maybeWakeWaiters() {
    // there is a race condition here:
    // waiter acquires mutex
    // waker checks empty_ flag and exits
    // maiter sets empty_ = true and waits on cv
    //
    // it is fine, because
    // 1. wait events should be extremely rare,
    //    if they are not rarre, it is better to
    //    increase pool size
    // 2. wait on CV is timed, so waiter will soon
    //    wake up and retry anyways
    if (empty_.load(std::memory_order_acquire)) {
      std::unique_lock lk(waitMutex_);
      empty_.store(false, std::memory_order_release);
      waitCV_.notify_all();
    }
  }

  std::unique_ptr<SharedIndexedMemPool<>> requestsPool_;
  uint64_t requestsPoolId_;
  std::vector<std::unique_ptr<SharedIndexedMemPool<>>> pools_;
  std::vector<uint64_t> poolIds_;
  std::vector<std::unique_ptr<MemFd>> rawBuffers_;
  std::vector<uint64_t> rawBufferIds_;
  std::unordered_map<uint64_t, int> poolIndexById_;
  std::vector<int> poolSizeIndices_;
  std::atomic<uint64_t> nextId_{};
  std::atomic<bool> empty_{false};
  std::mutex waitMutex_;
  std::condition_variable waitCV_;
};

/**
 * StackAllocator is a custom allocator that allocates memory from a
 * user-supplied buffer using a stack-like (LIFO) strategy. It supports aligned
 * allocations.
 *
 * This allocator conforms to the C++ allocator requirements and can be used
 * with standard containers via std::allocator_traits.
 *
 * Features:
 * - Allocates from fixed-size buffer with customizable alignment
 * - Fast allocation and deallocation (pointer bumping)
 * - Only supports LIFO deallocation order
 *
 * Limitations:
 * - No memory reclamation other than LIFO deallocation
 * - Not thread-safe
 * - Does not support non-LIFO deallocation or resizing
 */

struct StackAllocatorState {
  uint8_t *buffer;
  uint8_t *top;
  size_t capacity;
  size_t alignment;

  StackAllocatorState(uint8_t *buffer, uint8_t *top, size_t capacity,
                      size_t alignment)
      : buffer(buffer), top(top), capacity(capacity), alignment(alignment) {}
};

template <typename T> class StackAllocator {
protected:
  std::shared_ptr<StackAllocatorState> state_;

public:
  using value_type = T;

  explicit StackAllocator(std::shared_ptr<StackAllocatorState> state)
      : state_(std::move(state)) {}

  StackAllocator(void *buf, std::size_t size, std::size_t align = alignof(T))
      : state_(std::make_shared<StackAllocatorState>(
            static_cast<uint8_t *>(buf),
            static_cast<uint8_t *>(buf),
            size,
            align)) {
    state_->top = folly::align_ceil(state_->buffer, state_->alignment);
  }

  template <typename U>
  StackAllocator(const StackAllocator<U> &other) : state_(other.state_) {}

  T *allocate(std::size_t n) {
    const std::size_t size = n * sizeof(T);
    void *ptr = state_->top;
    state_->top = folly::align_ceil(state_->top + size, state_->alignment);
    if (state_->top - state_->buffer > state_->capacity) {
      throw std::bad_alloc();
    }
    return static_cast<T *>(ptr);
  }

  void deallocate(T *p, std::size_t n) {
    const std::size_t size = n * sizeof(T);
    state_->top = folly::align_floor(state_->top - size, state_->alignment);
    assert(reinterpret_cast<T *>(state_->top) == p);
  }

  template <typename U> struct rebind {
    using other = StackAllocator<U>;
  };

  // Needed to allow rebinds access to state_
  template <typename U> friend class StackAllocator;
};

template <typename T, typename U>
bool operator==(const StackAllocator<T> &a, const StackAllocator<U> &b) {
  return a.state_ == b.state_;
}

template <typename T, typename U>
bool operator!=(const StackAllocator<T> &a, const StackAllocator<U> &b) {
  return !(a == b);
}

using QueueAllocator = StackAllocator<rigtorp::mpmc::Slot<RequestPtr>>;
using Queue = rigtorp::mpmc::Queue<RequestPtr, QueueAllocator>;

static inline std::unique_ptr<Queue> attachQueue(void *buf, size_t size) {
  return std::make_unique<Queue>(QueueAllocator(buf, size));
}

static inline std::unique_ptr<Queue> makeQueue(void *buf, size_t size,
                                               size_t capacity) {
  return std::make_unique<Queue>(capacity, QueueAllocator(buf, size));
}

} // namespace ipc
