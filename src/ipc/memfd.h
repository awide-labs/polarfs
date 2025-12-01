#pragma once

#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/system/MemoryMapping.h>
#include <sys/mman.h>

namespace ipc {

class MemFd {
public:
  MemFd(const MemFd &) = delete;
  MemFd &operator=(const MemFd &) = delete;

  MemFd(const std::string &name, size_t size) {
    size_ = size;
    name_ = name;

    folly::File file(memfd_create(name_.c_str(), MFD_ALLOW_SEALING), true);

    if (folly::ftruncateNoInt(file.fd(), size_) != 0) {
      throw std::bad_alloc();
    }

    mapping_ = std::make_unique<folly::MemoryMapping>(
        std::move(file), 0, size_, folly::MemoryMapping::writable());
  }

  MemFd(folly::File file, size_t size) {
    size_ = size;

    mapping_ = std::make_unique<folly::MemoryMapping>(
        std::move(file), 0, size_, folly::MemoryMapping::writable());
  }

  void *buf() const { return mapping_->asWritableRange<char>().data(); }

  int fd() const { return mapping_->fd(); }

  size_t size() const { return size_; }

private:
  size_t size_{};
  std::unique_ptr<folly::MemoryMapping> mapping_{};
  std::string name_{};
};

} // namespace ipc
