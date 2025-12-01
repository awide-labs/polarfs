#pragma once

#include "SharedIndexedMemPool.h"
#include "memfd.h"
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

  bool alloc(size_t size, AllocResult &r) {
    const size_t sizeKiB = size / 1024;
    const size_t roundSize = sizeKiB > 0 ? folly::nextPowTwo(sizeKiB) : 2;
    const unsigned bucket = folly::findLastSet(roundSize) - 2;

    if (bucket >= poolSizeIndices_.size()) {
      return false;
    }

    unsigned index = poolSizeIndices_[bucket];

    while (index < pools_.size()) {
      auto &pool = pools_[index];
      uint32_t allocIndex = pool->allocIndex();
      if (allocIndex == 0) {
        continue;
      }

      r.bufferId = poolIds_[index];
      r.offset = pool->offsetOf(allocIndex);
      r.size = size;
      r.ptr = (*pool)[allocIndex];

      return true;
    }

    return false;
  }

  bool allocRequest(AllocResult &r) {
    auto &pool = requestsPool_;
    uint32_t allocIndex = pool->allocIndex();
    if (allocIndex == 0) {
      return false;
    }

    r.bufferId = requestsPoolId_;
    r.offset = pool->offsetOf(allocIndex);
    r.size = sizeof(Request);
    r.ptr = (*pool)[allocIndex];

    return true;
  }

  void free(AllocResult &r) {
    auto &pool = pools_[poolIndexById_[r.bufferId]];
    uint32_t elemIndex = pool->locateElem(r.ptr);
    pool->recycleIndex(elemIndex);
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

private:
  void populateSizeIndices() {
    std::vector<std::pair<unsigned, unsigned>> powers;
    powers.reserve(pools_.size());
    unsigned maxPower = 0;
    unsigned i = 0;
    for (auto &p : pools_) {
      auto size = p->elemSize() / 1024;
      auto power = folly::findLastSet(size) - 1;
      powers.emplace_back(i, power);
      if (power > maxPower) {
        maxPower = power;
      }
      maxPower = std::max(power, maxPower);
      ++i;
    }

    std::sort(powers.begin(), powers.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });

    poolSizeIndices_.resize(maxPower);
    for (auto &p : powers) {
      for (i = 0; i < p.second; i++) {
        poolSizeIndices_[i] = p.first;
      }
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
      : state_(std::make_shared<StackAllocatorState>(buf, buf, size, align)) {
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
