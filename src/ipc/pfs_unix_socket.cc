#include "pfs_unix_socket.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>

namespace pfsutil {

namespace {

constexpr size_t kReadBufSize = 8192;
// SCM_RIGHTS hard kernel limit is 253 fds per cmsg; we cap our incoming and
// outgoing batches at this. Anything larger must be split into multiple
// writes by the caller.
constexpr size_t kMaxFdsPerCmsg = 253;

void setNonBlock(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    throw std::system_error(errno, std::generic_category(), "fcntl GETFL");
  }
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    throw std::system_error(errno, std::generic_category(), "fcntl SETFL");
  }
}

} // namespace

// ============================== UnixSocket ==============================

UnixSocket::UnixSocket(EventLoop* loop, int fd) : loop_(loop), fd_(fd) {
  setNonBlock(fd_);
  loop_->watch(fd_, EventLoop::EV_READ | EventLoop::EV_RDHUP,
               [this](uint32_t e) { onEvent(e); });
}

UnixSocket::~UnixSocket() {
  if (!closed_) closeWithError(ECANCELED);
  for (auto& batch : receivedFds_) {
    for (int f : batch) ::close(f);
  }
}

void UnixSocket::setReadCallback(ReadCallback rcb,
                                 EofCallback ecb,
                                 ErrorCallback errcb) {
  readCb_ = std::move(rcb);
  eofCb_ = std::move(ecb);
  errorCb_ = std::move(errcb);
}

void UnixSocket::write(std::vector<uint8_t> data, WriteDoneCallback done) {
  writeWithFds(std::move(data), {}, std::move(done));
}

void UnixSocket::writeWithFds(std::vector<uint8_t> data,
                              std::vector<int> fdsToSend,
                              WriteDoneCallback done) {
  loop_->runInLoop([this,
                    data = std::move(data),
                    fds = std::move(fdsToSend),
                    done = std::move(done)]() mutable {
    if (closed_) {
      if (done) done(WriteResult{false, EBADF});
      return;
    }
    PendingWrite pw;
    pw.data = std::move(data);
    pw.fds = std::move(fds);
    pw.done = std::move(done);
    writeQueue_.push_back(std::move(pw));
    if (!watchingWrite_) {
      // Try sending immediately; if the socket buffer fills, doWrite will
      // arm EPOLLOUT for us.
      doWrite();
    }
  });
}

std::vector<int> UnixSocket::popReceivedFds() {
  if (receivedFds_.empty()) return {};
  auto r = std::move(receivedFds_.front());
  receivedFds_.pop_front();
  return r;
}

void UnixSocket::close() {
  if (closed_) return;
  closed_ = true;
  if (fd_ >= 0) {
    loop_->unwatch(fd_);
    ::close(fd_);
    fd_ = -1;
  }
  watchingWrite_ = false;
}

void UnixSocket::onEvent(uint32_t events) {
  if (events & EventLoop::EV_READ) {
    doRead();
    if (closed_) return;
  }
  if (events & EventLoop::EV_WRITE) {
    doWrite();
    if (closed_) return;
  }
  if (events & EventLoop::EV_ERR) {
    int err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
    closeWithError(err ? err : EIO);
  }
}

void UnixSocket::doRead() {
  for (;;) {
    uint8_t buf[kReadBufSize];
    alignas(cmsghdr) char cmsgbuf[CMSG_SPACE(sizeof(int) * kMaxFdsPerCmsg)];
    std::memset(cmsgbuf, 0, sizeof(cmsgbuf));
    iovec iov{buf, sizeof(buf)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    ssize_t n;
    do {
      n = ::recvmsg(fd_, &msg, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      int e = errno;
      closeWithError(e);
      return;
    }
    if (n == 0) {
      auto cb = eofCb_;
      std::deque<PendingWrite> q;
      q.swap(writeQueue_);
      close();
      for (auto& pw : q) {
        if (pw.done) pw.done(WriteResult{false, ECANCELED});
      }
      if (cb) cb();
      return;
    }

    std::vector<int> incoming;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr;
         c = CMSG_NXTHDR(&msg, c)) {
      if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
      size_t nfds = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      const int* fds = reinterpret_cast<const int*>(CMSG_DATA(c));
      incoming.insert(incoming.end(), fds, fds + nfds);
    }
    if (msg.msg_flags & MSG_CTRUNC) {
      // Lost some control data — fatal: close any fds we did get.
      for (int f : incoming) ::close(f);
      closeWithError(EOVERFLOW);
      return;
    }
    if (!incoming.empty()) {
      receivedFds_.push_back(std::move(incoming));
    }

    if (readCb_) readCb_(buf, static_cast<size_t>(n));
    if (closed_) return;

    // Short read: socket buffer drained, wait for next EPOLLIN.
    if (n < static_cast<ssize_t>(sizeof(buf))) return;
  }
}

void UnixSocket::doWrite() {
  while (!writeQueue_.empty() && !closed_) {
    PendingWrite& pw = writeQueue_.front();
    iovec iov{pw.data.data() + pw.offset, pw.data.size() - pw.offset};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    alignas(cmsghdr) char cmsgbuf[CMSG_SPACE(sizeof(int) * kMaxFdsPerCmsg)];
    if (!pw.fds.empty()) {
      if (pw.fds.size() > kMaxFdsPerCmsg) {
        // Caller violated the contract; refuse the write.
        WriteDoneCallback done = std::move(pw.done);
        writeQueue_.pop_front();
        if (done) done(WriteResult{false, E2BIG});
        continue;
      }
      size_t fdcnt = pw.fds.size();
      msg.msg_control = cmsgbuf;
      msg.msg_controllen = CMSG_SPACE(sizeof(int) * fdcnt);
      cmsghdr* c = CMSG_FIRSTHDR(&msg);
      c->cmsg_level = SOL_SOCKET;
      c->cmsg_type = SCM_RIGHTS;
      c->cmsg_len = CMSG_LEN(sizeof(int) * fdcnt);
      std::memcpy(CMSG_DATA(c), pw.fds.data(), sizeof(int) * fdcnt);
    }

    ssize_t n;
    do {
      n = ::sendmsg(fd_, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (!watchingWrite_) {
          loop_->modify(fd_, EventLoop::EV_READ | EventLoop::EV_RDHUP |
                                 EventLoop::EV_WRITE);
          watchingWrite_ = true;
        }
        return;
      }
      int e = errno;
      WriteDoneCallback done = std::move(pw.done);
      writeQueue_.pop_front();
      if (done) done(WriteResult{false, e});
      closeWithError(e);
      return;
    }

    pw.offset += static_cast<size_t>(n);
    pw.fds.clear(); // SCM_RIGHTS attaches to the first sendmsg only
    if (pw.offset >= pw.data.size()) {
      WriteDoneCallback done = std::move(pw.done);
      writeQueue_.pop_front();
      if (done) done(WriteResult{true, 0});
    }
  }

  if (writeQueue_.empty() && watchingWrite_) {
    loop_->modify(fd_, EventLoop::EV_READ | EventLoop::EV_RDHUP);
    watchingWrite_ = false;
  }
}

void UnixSocket::closeWithError(int err) {
  if (closed_) return;
  auto cb = errorCb_;
  // Drop any pending writes.
  std::deque<PendingWrite> q;
  q.swap(writeQueue_);
  close();
  for (auto& pw : q) {
    if (pw.done) pw.done(WriteResult{false, err});
  }
  if (cb) cb(err);
}

// ============================== connectUnix ==============================

int connectUnix(const std::string& path, int timeoutMs) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(), "socket");
  }
  setNonBlock(fd);

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    throw std::runtime_error("connectUnix: path too long");
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

  int r = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (r == 0) {
    return fd;
  }
  if (errno != EINPROGRESS) {
    int e = errno;
    ::close(fd);
    throw std::system_error(e, std::generic_category(), "connect");
  }

  // Wait for writability (= connect complete) up to timeoutMs.
  pollfd pfd{fd, POLLOUT, 0};
  int p;
  do {
    p = ::poll(&pfd, 1, timeoutMs);
  } while (p < 0 && errno == EINTR);
  if (p == 0) {
    ::close(fd);
    throw std::system_error(ETIMEDOUT, std::generic_category(),
                            "connect timeout");
  }
  if (p < 0) {
    int e = errno;
    ::close(fd);
    throw std::system_error(e, std::generic_category(), "poll");
  }

  int err = 0;
  socklen_t len = sizeof(err);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
    int e = errno;
    ::close(fd);
    throw std::system_error(e, std::generic_category(), "getsockopt SO_ERROR");
  }
  if (err != 0) {
    ::close(fd);
    throw std::system_error(err, std::generic_category(), "connect");
  }
  return fd;
}

// ============================== UnixListener =============================

UnixListener::~UnixListener() { stop(); }

void UnixListener::bind(const std::string& path) {
  fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd_ < 0) {
    throw std::system_error(errno, std::generic_category(), "socket");
  }
  ::unlink(path.c_str()); // best-effort; ignore errors

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error("UnixListener: path too long");
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

  if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    int e = errno;
    ::close(fd_);
    fd_ = -1;
    throw std::system_error(e, std::generic_category(), "bind");
  }
  path_ = path;
}

void UnixListener::start(int backlog, AcceptCallback cb) {
  if (::listen(fd_, backlog) != 0) {
    throw std::system_error(errno, std::generic_category(), "listen");
  }
  acceptCb_ = std::move(cb);
  loop_->watch(fd_, EventLoop::EV_READ, [this](uint32_t e) { onEvent(e); });
  listening_ = true;
}

void UnixListener::stop() {
  if (listening_) {
    loop_->unwatch(fd_);
    listening_ = false;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (!path_.empty()) {
    ::unlink(path_.c_str());
    path_.clear();
  }
}

void UnixListener::onEvent(uint32_t /*events*/) {
  for (;;) {
    int cfd = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
      // Non-fatal: keep listening.
      return;
    }
    if (acceptCb_) acceptCb_(cfd);
    else ::close(cfd);
  }
}

} // namespace pfsutil
