/*
 * AF_UNIX async socket + listener built on pfsutil::EventLoop. Drop-in
 * replacement for the slice of folly::AsyncFdSocket / AsyncServerSocket /
 * SocketFds PFS uses: edge-triggered read/write, deferred write queue,
 * SCM_RIGHTS fd passing on both sides.
 *
 * UnixSocket owns its fd. It does NOT take ownership of fds passed to
 * writeWithFds() — caller keeps them open at least until the write completes.
 * Inbound fds (popReceivedFds) ARE owned by the caller, who must close them.
 */
#pragma once

#include "pfs_event_loop.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace pfsutil {

struct WriteResult {
  bool ok;
  int err; // errno when !ok; 0 on success
};

class UnixSocket {
public:
  // Bytes read from the socket. `data` is valid for the call's duration.
  using ReadCallback = std::function<void(const uint8_t* data, size_t len)>;
  // Connection closed gracefully (peer FIN).
  using EofCallback = std::function<void()>;
  // Read or write error. err is errno (0 if unknown).
  using ErrorCallback = std::function<void(int err)>;
  // Per-write completion callback.
  using WriteDoneCallback = std::function<void(WriteResult)>;

  // Adopt a connected fd. Sets it non-blocking and registers with the loop.
  UnixSocket(EventLoop* loop, int fd);
  ~UnixSocket();
  UnixSocket(const UnixSocket&) = delete;
  UnixSocket& operator=(const UnixSocket&) = delete;

  // Loop-thread only.
  void setReadCallback(ReadCallback rcb, EofCallback ecb, ErrorCallback errcb);

  // Submit a write. Thread-safe — internally schedules on the loop thread.
  // fdsToSend, if non-empty, is attached via SCM_RIGHTS to the first sendmsg.
  // The caller retains ownership of the fds.
  void write(std::vector<uint8_t> data, WriteDoneCallback done = nullptr);
  void writeWithFds(std::vector<uint8_t> data,
                    std::vector<int> fdsToSend,
                    WriteDoneCallback done = nullptr);

  // Pop the next batch of received fds. Caller takes ownership and must
  // close. Returns empty vector if nothing pending.
  std::vector<int> popReceivedFds();
  bool hasReceivedFds() const noexcept { return !receivedFds_.empty(); }

  int fd() const noexcept { return fd_; }

private:
  void onEvent(uint32_t events);
  void doRead();
  void doWrite();
  void close();
  void closeWithError(int err);

  EventLoop* loop_;
  int fd_{-1};

  ReadCallback readCb_;
  EofCallback eofCb_;
  ErrorCallback errorCb_;

  struct PendingWrite {
    std::vector<uint8_t> data;
    size_t offset{0};       // bytes already sent
    std::vector<int> fds;   // attached only on first sendmsg of this entry
    WriteDoneCallback done;
  };
  std::deque<PendingWrite> writeQueue_;
  bool watchingWrite_{false};

  std::deque<std::vector<int>> receivedFds_;

  bool closed_{false};
};

// Connect to an AF_UNIX path with a timeout (milliseconds, -1 = no timeout).
// Returns a connected non-blocking fd, or throws std::system_error on failure.
int connectUnix(const std::string& path, int timeoutMs);

class UnixListener {
public:
  using AcceptCallback = std::function<void(int fd)>;

  explicit UnixListener(EventLoop* loop) : loop_(loop) {}
  ~UnixListener();
  UnixListener(const UnixListener&) = delete;
  UnixListener& operator=(const UnixListener&) = delete;

  // Bind to AF_UNIX `path`. Removes existing path if present.
  void bind(const std::string& path);
  // Begin listening; cb fires for each accepted client fd. Loop-thread only.
  void start(int backlog, AcceptCallback cb);
  // Stop accepting and close the listening socket. Removes the path file.
  void stop();

  int fd() const noexcept { return fd_; }

private:
  void onEvent(uint32_t events);

  EventLoop* loop_;
  int fd_{-1};
  std::string path_;
  AcceptCallback acceptCb_;
  bool listening_{false};
};

} // namespace pfsutil
