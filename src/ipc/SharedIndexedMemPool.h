/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * SharedIndexedMemPool is modified version of folly::IndexedMemPool
 */

#pragma once

#include <assert.h>
#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <type_traits>
#include <unistd.h>

#include "access_spreader.h"
#include "memfd.h"
#include "pfs_align.h"

namespace ipc {

/// Instances of SharedIndexedMemPool dynamically allocate and then pool their
/// element type (T), returning 4-byte integer indices that can be passed
/// to the pool's operator[] method to access or obtain pointers to the
/// actual elements.  The memory backing items returned from the pool
/// will always be readable, even if items have been returned to the pool.
/// These two features are useful for lock-free algorithms.  The indexing
/// behavior makes it easy to build tagged pointer-like-things, since
/// a large number of elements can be managed using fewer bits than a
/// full pointer.  The access-after-free behavior makes it safe to read
/// from T-s even after they have been recycled, since it is guaranteed
/// that the memory won't have been returned to the OS and unmapped
/// (the algorithm must still use a mechanism to validate that the read
/// was correct, but it doesn't have to worry about page faults), and if
/// the elements use internal sequence numbers it can be guaranteed that
/// there won't be an ABA match due to the element being overwritten with
/// a different type that has the same bit pattern.
///
/// IMPORTANT: Space for extra elements is allocated to account for those
/// that are inaccessible because they are in other local lists, so the
/// actual number of items that can be allocated ranges from capacity to
/// capacity + (NumLocalLists_-1)*LocalListLimit_.  This is important if
/// you are trying to maximize the capacity of the pool while constraining
/// the bit size of the resulting pointers, because the pointers will
/// actually range up to the boosted capacity.  See maxIndexForCapacity
/// and capacityForMaxIndex.
///
/// To avoid contention, NumLocalLists_ free lists of limited (less than
/// or equal to LocalListLimit_) size are maintained, and each thread
/// retrieves and returns entries from its associated local list.  If the
/// local list becomes too large then elements are placed in bulk in a
/// global free list.  This allows items to be efficiently recirculated
/// from consumers to producers.  AccessSpreader is used to access the
/// local lists, so there is no performance advantage to having more
/// local lists than L1 caches.
///
/// The pool mmap-s the entire necessary address space when the pool is
/// constructed, but delays element construction.  This means that only
/// elements that are actually returned to the caller get paged into the
/// process's resident set (RSS).
struct SharedIndexedMemPool {
  SharedIndexedMemPool(const SharedIndexedMemPool &) = delete;
  SharedIndexedMemPool &operator=(const SharedIndexedMemPool &) = delete;

  // these are public because clients may need to reason about the number
  // of bits required to hold indices from a pool, given its capacity

  uint32_t maxIndexForCapacity(uint32_t capacity) {
    // index of std::numeric_limits<uint32_t>::max() is reserved for isAllocated
    // tracking
    return uint32_t(
        std::min(uint64_t(capacity) + (numLocalLists_ - 1) * localListLimit_,
                 uint64_t(std::numeric_limits<uint32_t>::max() - 1)));
  }

  uint32_t capacityForMaxIndex(uint32_t maxIndex) {
    return maxIndex - (numLocalLists_ - 1) * localListLimit_;
  }

  /// Constructs a pool that can allocate at least _capacity_ elements,
  /// even if all the local lists are full
  explicit SharedIndexedMemPool(std::string name, size_t elemSize,
                                uint32_t capacity, uint32_t numLocalLists,
                                uint32_t localLstLimit)
      : elemSize_(elemSize), numLocalLists_(numLocalLists),
        localListLimit_(localLstLimit),
        actualCapacity_(maxIndexForCapacity(capacity)) {
    size_t pagesize = size_t(sysconf(_SC_PAGESIZE));

    size_t slotsSize = sizeof(Slot) * (actualCapacity_ + 1);
    slotsSize = pfsutil::align_ceil(slotsSize, pagesize);

    size_t dataSize = elemSize * (actualCapacity_ + 1);
    dataSize = pfsutil::align_ceil(dataSize, pagesize);

    size_t dynamicDataSize =
        sizeof(DynamicData) + sizeof(LocalList) * numLocalLists_;
    dynamicDataSize = pfsutil::align_ceil(dynamicDataSize, pagesize);

    mmapLength_ = slotsSize + dataSize + dynamicDataSize;
    assert((mmapLength_ % pagesize) == 0);

    memfd_ = std::make_unique<MemFd>(name, mmapLength_);
    slots_ = (Slot *)memfd_->buf();
    data_ = (char *)memfd_->buf() + slotsSize;

    for (int i = 0; i < actualCapacity_; i++) {
      new (&slots_[i].localNext) std::atomic<uint32_t>;
      new (&slots_[i].globalNext) std::atomic<uint32_t>;
    }

    if (localLstLimit >= 255) {
      throw std::runtime_error("localListLimit must fit in 8 bits");
    }

    dynamicData_ =
        (DynamicData *)((char *)memfd_->buf() + slotsSize + dataSize);
    new (dynamicData_) DynamicData;
    dynamicData_->size_ = 0;
    dynamicData_->globalHead_ = TaggedPtr{};
    for (int i = 0; i < numLocalLists_; i++) {
      new (&dynamicData_->local_[i]) LocalList;
    }
  }

  explicit SharedIndexedMemPool(std::unique_ptr<MemFd> memfd, size_t elemSize,
                                uint32_t capacity, uint32_t numLocalLists,
                                uint32_t localLstLimit)
      : elemSize_(elemSize), numLocalLists_(numLocalLists),
        localListLimit_(localLstLimit),
        actualCapacity_(maxIndexForCapacity(capacity)) {
    size_t pagesize = size_t(sysconf(_SC_PAGESIZE));

    size_t headerSize = elemSize_ * (actualCapacity_ + 1);
    headerSize = ((headerSize - 1) & ~(pagesize - 1)) + pagesize;

    size_t dataSize = elemSize * (actualCapacity_ + 1);
    dataSize = ((dataSize - 1) & ~(pagesize - 1)) + pagesize;

    mmapLength_ = headerSize + dataSize + pagesize;
    assert((mmapLength_ % pagesize) == 0);

    slots_ = (Slot *)memfd_->buf();
    data_ = (char *)memfd_->buf() + headerSize;

    if (localLstLimit >= 255) {
      throw std::runtime_error("localListLimit must fit in 8 bits");
    }
  }

  /// Destroys all of the contained elements
  ~SharedIndexedMemPool() {}

  void zeroInit() {
    size_t pagesize = size_t(sysconf(_SC_PAGESIZE));
    size_t dataSize = elemSize_ * (actualCapacity_ + 1);
    dataSize = ((dataSize - 1) & ~(pagesize - 1)) + pagesize;
    memset(data_, 0, dataSize);
  }

  size_t elemSize() const { return elemSize_; }

  /// Returns a lower bound on the number of elements that may be
  /// simultaneously allocated and not yet recycled.  Because of the
  /// local lists it is possible that more elements than this are returned
  /// successfully
  uint32_t capacity() { return capacityForMaxIndex(actualCapacity_); }

  /// Returns the maximum index of elements ever allocated in this pool
  /// including elements that have been recycled.
  uint32_t maxAllocatedIndex() const {
    // Take the minimum since it is possible that size_ > actualCapacity_.
    // This can happen if there are multiple concurrent requests
    // when size_ == actualCapacity_ - 1.
    return std::min(uint32_t(dynamicData_->size_), uint32_t(actualCapacity_));
  }

  /// Finds a slot with a non-zero index, emplaces a T there if we're
  /// using the eager recycle lifecycle mode, and returns the index,
  /// or returns 0 if no elements are available.
  uint32_t allocIndex() {
    auto idx = localPop(localHead());
    if (idx != 0) {
      Slot &s = slot(idx);
      markAllocated(s);
    }
    return idx;
  }

  /// Gives up ownership previously granted by alloc()
  void recycleIndex(uint32_t idx) {
    assert(isAllocated(idx));
    localPush(localHead(), idx);
  }

  /// Provides access to the pooled element referenced by idx
  void *operator[](uint32_t idx) { return (void *)(data_ + idx * elemSize_); }

  off_t offsetOf(uint32_t idx) {
    return (data_ + idx * elemSize_) - (char *)memfd_->buf();
  }

  /// If elem == &pool[idx], then pool.locateElem(elem) == idx.  Also,
  /// pool.locateElem(nullptr) == 0
  uint32_t locateElem(const void *elem) const {
    if (!elem) {
      return 0;
    }

    static_assert(std::is_standard_layout<Slot>::value, "offsetof needs POD");

    uintptr_t diff = (uintptr_t)(elem) - (uintptr_t)data_;

    auto rv = uint32_t(diff / elemSize_);

    return rv;
  }

  /// Returns true iff idx has been alloc()ed and not recycleIndex()ed
  bool isAllocated(uint32_t idx) const {
    return slot(idx).localNext.load(std::memory_order_acquire) == uint32_t(-1);
  }

  int fd() const { return memfd_->fd(); }

  MemFd &memfd() const { return *memfd_; }

private:
  ///////////// types

  struct Slot : public pfsutil::cacheline_align_t {
    std::atomic<uint32_t> localNext;
    std::atomic<uint32_t> globalNext;

    Slot() : localNext{}, globalNext{} {}
  };

  struct TaggedPtr {
    uint32_t idx;

    // size is bottom 8 bits, tag in top 24.  g++'s code generation for
    // bitfields seems to depend on the phase of the moon, plus we can
    // do better because we can rely on other checks to avoid masking
    uint32_t tagAndSize;

    enum : uint32_t {
      SizeBits = 8,
      SizeMask = (1U << SizeBits) - 1,
      TagIncr = 1U << SizeBits,
    };

    uint32_t size() const { return tagAndSize & SizeMask; }

    TaggedPtr withSize(uint32_t repl) const {
      return TaggedPtr{idx, (tagAndSize & ~SizeMask) | repl};
    }

    TaggedPtr withSizeIncr() const { return TaggedPtr{idx, tagAndSize + 1}; }

    TaggedPtr withSizeDecr() const {
      assert(size() > 0);
      return TaggedPtr{idx, tagAndSize - 1};
    }

    TaggedPtr withIdx(uint32_t repl) const {
      return TaggedPtr{repl, tagAndSize + TagIncr};
    }

    TaggedPtr withEmpty() const { return withIdx(0).withSize(0); }
  };

  struct alignas(pfsutil::kCachelineSize) LocalList {
    std::atomic<TaggedPtr> head;

    LocalList() : head(TaggedPtr{}) {}
  };

  ////////// fields

  size_t elemSize_;

  /// the number of bytes allocated from mmap, which is a multiple of
  /// the page size of the machine
  size_t mmapLength_;

  uint32_t numLocalLists_;

  uint32_t localListLimit_;

  /// the actual number of slots that we will allocate, to guarantee
  /// that we will satisfy the capacity requested at construction time.
  /// They will be numbered 1..actualCapacity_ (note the 1-based counting),
  /// and occupy slots_[1..actualCapacity_].
  uint32_t actualCapacity_;

  /// raw storage, only 1..min(size_,actualCapacity_) (inclusive) are
  /// actually constructed.  Note that slots_[0] is not constructed or used
  Slot *slots_;

  char *data_;

  std::unique_ptr<MemFd> memfd_;

  struct DynamicData {
    /// this records the number of slots that have actually been constructed.
    /// To allow use of atomic ++ instead of CAS, we let this overflow.
    /// The actual number of constructed elements is min(actualCapacity_,
    /// size_)
    std::atomic<uint32_t> size_;

    /// this is the head of a list of node chained by globalNext, that are
    /// themselves each the head of a list chained by localNext
    alignas(pfsutil::kCachelineSize)
        std::atomic<TaggedPtr> globalHead_;

    /// use AccessSpreader to find your list.  We use stripes instead of
    /// thread-local to avoid the need to grow or shrink on thread start
    /// or join.   These are heads of lists chained with localNext
    LocalList local_[];
  };

  DynamicData *dynamicData_;

  ///////////// private methods

  uint32_t slotIndex(uint32_t idx) const {
    assert(0 < idx && idx <= actualCapacity_ &&
           idx <= dynamicData_->size_.load(std::memory_order_acquire));
    return idx;
  }

  Slot &slot(uint32_t idx) const { return slots_[slotIndex(idx)]; }

  // localHead references a full list chained by localNext.  s should
  // reference slot(localHead), it is passed as a micro-optimization
  void globalPush(Slot &s, uint32_t localHead) {
    while (true) {
      TaggedPtr gh = dynamicData_->globalHead_.load(std::memory_order_acquire);
      s.globalNext.store(gh.idx, std::memory_order_relaxed);
      if (dynamicData_->globalHead_.compare_exchange_strong(
              gh, gh.withIdx(localHead))) {
        // success
        return;
      }
    }
  }

  // idx references a single node
  void localPush(std::atomic<TaggedPtr> &head, uint32_t idx) {
    Slot &s = slot(idx);
    TaggedPtr h = head.load(std::memory_order_acquire);
    while (true) {
      s.localNext.store(h.idx, std::memory_order_release);

      if (h.size() == localListLimit_) {
        // push will overflow local list, steal it instead
        if (head.compare_exchange_strong(h, h.withEmpty())) {
          // steal was successful, put everything in the global list
          globalPush(s, idx);
          return;
        }
      } else {
        // local list has space
        if (head.compare_exchange_strong(h, h.withIdx(idx).withSizeIncr())) {
          // success
          return;
        }
      }
      // h was updated by failing CAS
    }
  }

  // returns 0 if empty
  uint32_t globalPop() {
    while (true) {
      TaggedPtr gh = dynamicData_->globalHead_.load(std::memory_order_acquire);
      if (gh.idx == 0 || dynamicData_->globalHead_.compare_exchange_strong(
                             gh, gh.withIdx(slot(gh.idx).globalNext.load(
                                     std::memory_order_relaxed)))) {
        // global list is empty, or pop was successful
        return gh.idx;
      }
    }
  }

  // returns 0 if allocation failed
  uint32_t localPop(std::atomic<TaggedPtr> &head) {
    while (true) {
      TaggedPtr h = head.load(std::memory_order_acquire);
      if (h.idx != 0) {
        // local list is non-empty, try to pop
        Slot &s = slot(h.idx);
        auto next = s.localNext.load(std::memory_order_relaxed);
        if (head.compare_exchange_strong(h, h.withIdx(next).withSizeDecr())) {
          // success
          return h.idx;
        }
        continue;
      }

      uint32_t idx = globalPop();
      if (idx == 0) {
        // global list is empty, allocate and construct new slot
        if (dynamicData_->size_.load(std::memory_order_relaxed) >=
                actualCapacity_ ||
            (idx = ++dynamicData_->size_) > actualCapacity_) {
          // allocation failed
          return 0;
        }
        Slot &s = slot(idx);
        // std::atomic<uint32_t> is nothrow-default-constructible.
        // As an optimization, use default-initialization (no parens) rather
        // than direct-initialization (with parens): these locations are
        // stored-to before they are loaded-from
        new (&s.localNext) std::atomic<uint32_t>;
        new (&s.globalNext) std::atomic<uint32_t>;
        return idx;
      }

      Slot &s = slot(idx);
      auto next = s.localNext.load(std::memory_order_relaxed);
      if (head.compare_exchange_strong(
              h, h.withIdx(next).withSize(localListLimit_))) {
        // global list moved to local list, keep head for us
        return idx;
      }
      // local bulk push failed, return idx to the global list and try again
      globalPush(s, idx);
    }
  }

  std::atomic<TaggedPtr> &localHead() {
    auto stripe = pfsutil::AccessSpreader::current(numLocalLists_);
    return dynamicData_->local_[stripe].head;
  }

  void markAllocated(Slot &slot) {
    slot.localNext.store(uint32_t(-1), std::memory_order_release);
  }
};

} // namespace ipc
