#pragma once

#include <atomic>
#include <folly/lang/Align.h>
#include <folly/synchronization/AtomicStruct.h>

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

  TaggedPtr withSizeDecr() const { return TaggedPtr{idx, tagAndSize - 1}; }

  TaggedPtr withIdx(uint32_t repl) const {
    return TaggedPtr{repl, tagAndSize + TagIncr};
  }

  TaggedPtr withEmpty() const { return withIdx(0).withSize(0); }
};

struct LocalList : public folly::cacheline_align_t {
  folly::AtomicStruct<TaggedPtr, std::atomic> head;
  LocalList() : head(TaggedPtr{}) {}
};
