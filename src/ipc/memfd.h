#pragma once

#include <cerrno>
#include <string>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>

namespace ipc {

class MemFd {
public:
  MemFd(const MemFd &) = delete;
  MemFd &operator=(const MemFd &) = delete;

  // Creates a new sealable memfd-backed shared mapping of `size` bytes.
  MemFd(const std::string &name, size_t size) : size_(size), name_(name) {
    int fd = ::memfd_create(name_.c_str(), MFD_ALLOW_SEALING);
    if (fd < 0) {
      throw std::system_error(errno, std::generic_category(), "memfd_create");
    }
    if (::ftruncate(fd, static_cast<off_t>(size_)) != 0) {
      int e = errno;
      ::close(fd);
      throw std::system_error(e, std::generic_category(), "ftruncate");
    }
    fd_ = fd;
    map();
  }

  // Adopts ownership of `fd` (must already be sized to `size`).
  MemFd(int fd, size_t size) : size_(size), fd_(fd) { map(); }

  ~MemFd() {
    if (buf_ != nullptr && buf_ != MAP_FAILED) {
      ::munmap(buf_, size_);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  void *buf() const { return buf_; }
  int fd() const { return fd_; }
  size_t size() const { return size_; }

private:
  void map() {
    buf_ = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (buf_ == MAP_FAILED) {
      int e = errno;
      ::close(fd_);
      fd_ = -1;
      throw std::system_error(e, std::generic_category(), "mmap");
    }
  }

  size_t size_{};
  int fd_{-1};
  void *buf_{nullptr};
  std::string name_{};
};

} // namespace ipc
