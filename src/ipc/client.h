#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <semaphore.h>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "SharedIndexedMemPool.h"
#include "access_spreader.h"
#include "lib/dclcrwlock.h"
#include "memfd.h"
#include "pfs_event_loop.h"
#include "pfs_unix_socket.h"
#include "proto.h"
#include "shm.h"

#include "pfsd_common.h"

namespace ipc {

struct ServerQueue {
  std::unique_ptr<MemFd> memfd;
  std::unique_ptr<Queue> queue;
};

template <typename Queue> class BestOfTwoPolicy {
public:
  static constexpr size_t QUEUE_DEPTH_LOOKUP_THRESHOLD = 4;
  static constexpr size_t QUEUE_DEPTH_SWITCH_THRESHOLD = 2;

  BestOfTwoPolicy(std::vector<Queue> queues) : queues_(queues) {}

  Queue pick() {
    auto *q1 = queues_[pfsutil::AccessSpreader::current(queues_.size())];
    if (q1->size() < QUEUE_DEPTH_LOOKUP_THRESHOLD) {
      return q1;
    }

    auto *q2 = queues_[random_int_uniform(queues_.size())];
    if (q1->size() < q2->size() + QUEUE_DEPTH_SWITCH_THRESHOLD) {
      return q1;
    }

    return q2;
  }

private:
  std::vector<Queue> queues_;
};

class Session {
  using LoadBalancingPolicy = BestOfTwoPolicy<Queue *>;

public:
  explicit Session()
      : error_(false), connected_(false), queuesAreReady_(false),
        serverHelloReceived_(false, 0), ackReceived_(false), forkChild_(false) {
    rwLock_.init();
  }

  ~Session() { rwLock_.destroy(); }

  void setPbdname(std::string pbdname) { pbdname_ = pbdname; }

  /*
   * Capabilities advertised by the server in its SERVER_HELLO. 0 until the
   * handshake completes, and 0 for a pre-capability daemon (which omits the
   * trailing bitmap). Callers gate optional features (e.g. zero-copy) on the
   * relevant CAP_* bit before using them.
   */
  uint64_t serverCaps() const { return serverCaps_; }

  bool start(std::string cluster, int host_id, int flags, int timeoutMs) {
    // Set early so the mount-time control round-trips (sendBuffers) can bound
    // their ack waits on it, not just the post-mount ones.
    timeoutMs_ = timeoutMs;
    eventLoopThread_ = std::thread([this, timeoutMs] {
      try {
        std::string sockPath = makeSockPath(pbdname_);
        int fd = pfsutil::connectUnix(sockPath, timeoutMs);
        socket_ = std::make_unique<pfsutil::UnixSocket>(&evb_, fd);
        socket_->setReadCallback(
            [this](const uint8_t *data, size_t len) { onBytes(data, len); },
            [this] { onEof(); },
            [this](int err) { onError(err); });
        postEvent([this] { connected_ = true; });
      } catch (const std::system_error &ex) {
        PFSD_CLIENT_ELOG("Connection error: '%s'", ex.what());
        postEvent([this] { error_ = true; });
        return;
      }

      fdSeqNum_ = 0;
      evb_.loopForever();
      queuesAreReady_ = false;
      queues_.clear();
    });

    waitEvent([this] { return connected_ || error_; });
    if (error_) {
      return false;
    }

    waitEvent([this] { return queuesAreReady_ || error_; });
    if (error_) {
      return false;
    }

    if (sendHello(cluster, host_id, flags, timeoutMs) != 0) {
      return false;
    }

    if (!sendBuffers()) {
      return false;
    }

    cluster_ = cluster;
    hostId_ = host_id;
    flags_ = flags;

    return true;
  }

  bool shutdown() {
    if (forkChild_) {
      return true;
    }
    evb_.runInLoop([this] { socket_.reset(); });
    evb_.terminate();
    eventLoopThread_.join();
    pools_.clear();
    return true;
  }

  void atforkChild() { forkChild_ = true; }

  bool restart(std::string cluster, int host_id, int flags, int timeoutMs) {
    return shutdown() && start(cluster, host_id, flags, timeoutMs);
  }

  int remount(const std::string cluster, int host_id, int flags,
              int timeoutMs) {
    return sendRemount(cluster, host_id, flags, timeoutMs);
  }

  std::optional<uint64_t>
  registerMemPool(std::unique_ptr<SharedIndexedMemPool> pool,
                  bool requestPool) {
    std::lock_guard lk(rwLock_);
    uint64_t id;
    if (requestPool) {
      id = pools_.addRequestsPool(std::move(pool));
    } else {
      id = pools_.addPool(std::move(pool));
    }
    return id;
  }

  std::optional<uint64_t> registerMemBuffer(std::unique_ptr<MemFd> memfd) {
    std::lock_guard lk(rwLock_);
    return pools_.addRawBuffer(std::move(memfd));
  }

  /* Drop a locally-registered raw buffer. */
  bool removeRawBuffer(uint64_t id) {
    std::lock_guard lk(rwLock_);
    return pools_.removeRawBuffer(id);
  }

  /*
   * Ship a single, already locally-registered raw buffer to the server after
   * mount. Used for post-mount pfsd_register_shared_buffer; pre-mount buffers
   * are batched by sendBuffers() during start(). Returns false on error.
   */
  bool sendBuffer(uint64_t id) {
    RegisterBuffersMessage message;
    std::vector<int> toSend;
    {
      std::shared_lock lk(rwLock_);
      auto desc = pools_.rawBufferDesc(id);
      if (!desc) {
        PFSD_CLIENT_ELOG("unknown buffer id %lu", id);
        return false;
      }
      message.buffers.push_back(BufferDesc{desc->id, desc->size});
      toSend.push_back(desc->fd);
    }

    std::lock_guard<std::mutex> ctl(ctrlMutex_);
    ackReceived_ = false;
    evb_.runInLoopAndWait([this, &message, &toSend] {
      socket_->writeWithFds(message.serialize(), std::move(toSend));
      fdSeqNum_ += message.buffers.size();
    });
    if (!waitEventMs([this] { return ackReceived_ || error_; }, timeoutMs_)) {
      PFSD_CLIENT_ELOG("timed out waiting for server ack");
      return false;
    }
    return !error_;
  }

  /*
   * Unregister a raw buffer on the server, then drop the local mapping. The
   * local fd is released only after the server acknowledges it stopped using
   * its own copy, so no in-flight server I/O can touch a munmap'd region.
   */
  bool sendUnregisterBuffer(uint64_t id) {
    UnregisterBuffersMessage message;
    message.ids.push_back(id);

    {
      std::lock_guard<std::mutex> ctl(ctrlMutex_);
      ackReceived_ = false;
      evb_.runInLoopAndWait(
          [this, &message] { socket_->write(message.serialize()); });
      if (!waitEventMs([this] { return ackReceived_ || error_; }, timeoutMs_)) {
        PFSD_CLIENT_ELOG("timed out waiting for server ack");
        return false;
      }
      if (error_) {
        return false;
      }
    }

    std::lock_guard lk(rwLock_);
    pools_.removeRawBuffer(id);
    return true;
  }

  void *getPtrFromSharedBuf(uint64_t bufId, off_t offset) {
    std::shared_lock lk(rwLock_);
    return pools_.resolvePtr(bufId, offset);
  }

  void executeRequest(RequestPtr rp, Request *r) {
    if (!r->initialized) {
      sem_init(&r->sem, 1, 0);
      r->initialized = true;
    }

    r->req.common.owner = pfs_getpid();
    r->rsp.common.error = 0;

    rp.connectionId = connectionId_;

    std::shared_lock lk(rwLock_);
    auto queue = policy_->pick();
    queue->emplace(rp);

    while (sem_wait(&r->sem) != 0 && errno == EINTR)
      ;
  }

  bool alloc(size_t size, SharedMemoryPools::AllocResult &r) {
    return pools_.alloc(size, r);
  }

  void allocRequest(SharedMemoryPools::AllocResult &r) {
    return pools_.allocRequest(r);
  }

  void free(SharedMemoryPools::AllocResult &r) { pools_.free(r); }

  void freeRequest(SharedMemoryPools::AllocResult &r) { pools_.freeRequest(r); }

  uint64_t connectionId() { return connectionId_; }

private:
  bool sendBuffers() {
    std::shared_lock lk(rwLock_);

    std::vector<int> toSend;
    RegisterBuffersMessage message;

    for (auto desc : pools_.allBuffers()) {
      message.buffers.push_back(BufferDesc{desc.id, desc.size});
      toSend.push_back(desc.fd);
      PFSD_CLIENT_LOG("Sending buf %zu size %zu", desc.id, desc.size);
    }

    ackReceived_ = false;

    evb_.runInLoopAndWait([this, &message, &toSend] {
      if (toSend.empty()) {
        socket_->write(message.serialize());
      } else {
        socket_->writeWithFds(message.serialize(), std::move(toSend));
        fdSeqNum_ += message.buffers.size();
      }
      PFSD_CLIENT_LOG("Sent %zu memfds to server", message.buffers.size());
    });

    if (!waitEventMs([this] { return ackReceived_ || error_; }, timeoutMs_)) {
      PFSD_CLIENT_ELOG("timed out waiting for server ack");
      return false;
    }

    return !error_;
  }

  int sendHello(std::string cluster, int host_id, int flags, int timeoutMs) {
    evb_.runInLoopAndWait([&cluster, this, host_id, flags] {
      ClientHelloMessage message;
      message.cluster = cluster;
      message.pbdname = pbdname_;
      message.host_id = host_id;
      message.flags = flags;
      message.caps = kLocalCaps;

      socket_->write(message.serialize());
    });

    waitEventMs([this] { return serverHelloReceived_.first; }, timeoutMs);

    if (!serverHelloReceived_.first) {
      return -1;
    }

    return serverHelloReceived_.second;
  }

  int sendRemount(std::string cluster, int host_id, int flags, int timeoutMs) {
    remountResult_.first = false;

    evb_.runInLoopAndWait([&cluster, this, host_id, flags] {
      RemountMessage message;
      message.cluster = cluster;
      message.pbdname = pbdname_;
      message.host_id = host_id;
      message.flags = flags;

      socket_->write(message.serialize());
    });

    waitEventMs([this] { return remountResult_.first; }, timeoutMs);
    PFSD_CLIENT_LOG("remount result: %d %d", remountResult_.first,
                    remountResult_.second);

    if (!remountResult_.first) {
      return -1;
    }

    return remountResult_.second;
  }

  void onBytes(const uint8_t *data, size_t len) {
    PFSD_CLIENT_LOG("Client received: %zu bytes", len);
    readBuf_.insert(readBuf_.end(), data, data + len);

    while (!readBuf_.empty()) {
      int type;
      size_t msgLen;
      if (!readMessageTypeAndLen(readBuf_.data(), readBuf_.size(), type,
                                 msgLen)) {
        break;
      }
      PFSD_CLIENT_LOG("Got message type: %d len: %zu", type, msgLen);
      if (readBuf_.size() < msgLen) {
        break;
      }
      const uint8_t *msg = readBuf_.data();

      if (type == ACK) {
        AckMessage message;
        if (!message.deserialize(msg, msgLen)) {
          break;
        }
        postEvent([this] { ackReceived_ = true; });
      } else if (type == REGISTER_QUEUES) {
        RegisterQueuesMessage message;
        if (!message.deserialize(msg, msgLen)) {
          break;
        }
        for (size_t i = 0; i < message.buffers.size(); ++i) {
          PFSD_CLIENT_LOG("Client received queue buffer id: %lu size: %zu",
                          message.buffers[i].id, message.buffers[i].size);
        }
        if (!message.buffers.empty()) {
          auto receivedFds = socket_->popReceivedFds();
          for (size_t i = 0; i < receivedFds.size(); ++i) {
            auto memfd = std::make_unique<MemFd>(receivedFds[i],
                                                 message.buffers[i].size);
            auto queue = attachQueue(memfd->buf(), memfd->size());
            queues_.push_back(ServerQueue{std::move(memfd), std::move(queue)});
          }
        }
        if (message.last) {
          std::vector<Queue *> qlist;
          for (auto &q : queues_) {
            qlist.emplace_back(q.queue.get());
          }
          {
            std::lock_guard lk(rwLock_);
            policy_ = std::make_unique<LoadBalancingPolicy>(qlist);
          }
          postEvent([this] { queuesAreReady_ = true; });
        }
      } else if (type == SERVER_HELLO) {
        ServerHelloMessage message;
        if (!message.deserialize(msg, msgLen)) {
          break;
        }
        postEvent([this, &message] {
          connectionId_ = message.connectionId;
          serverCaps_ = message.caps;
          serverHelloReceived_.first = true;
          serverHelloReceived_.second = message.error;
        });
      } else if (type == REMOUNT_RESULT) {
        RemountResultMessage message;
        if (!message.deserialize(msg, msgLen)) {
          break;
        }
        postEvent([this, &message] {
          remountResult_.first = true;
          remountResult_.second = message.error;
        });
      } else {
        break;
      }

      readBuf_.erase(readBuf_.begin(), readBuf_.begin() + msgLen);
    }
  }

  void onEof() {
    connected_ = false;
    PFSD_CLIENT_LOG("Client disconnected.");
  }

  void onError(int err) {
    PFSD_CLIENT_ELOG("Socket error: %d (%s)", err, strerror(err));
  }

  template <typename Function> void postEvent(Function func) {
    std::unique_lock lk(cvMutex_);
    func();
    cv_.notify_all();
  }

  template <typename Predicate> void waitEvent(Predicate p) {
    std::unique_lock lk(cvMutex_);
    cv_.wait(lk, p);
  }

  // Returns true if the predicate became satisfied, false on timeout.
  template <typename Predicate> bool waitEventMs(Predicate p, int timeoutMs) {
    std::unique_lock lk(cvMutex_);
    return cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), p);
  }

  pfsutil::EventLoop evb_;
  std::unique_ptr<pfsutil::UnixSocket> socket_;
  std::string pbdname_;
  std::vector<uint8_t> readBuf_;
  std::thread eventLoopThread_;

  bool error_;
  bool connected_;
  bool queuesAreReady_;
  std::mutex cvMutex_;
  std::condition_variable cv_;
  // Serializes post-mount control round-trips (register/unregister buffer),
  // which share ackReceived_ and the control socket.
  std::mutex ctrlMutex_;

  std::vector<ServerQueue> queues_;
  DCLCRWLock rwLock_;
  std::unique_ptr<LoadBalancingPolicy> policy_;

  std::queue<SharedBufferInfo> pendingSharedBuffers_;
  int fdSeqNum_;

  std::pair<bool, int> serverHelloReceived_;
  std::pair<bool, int> remountResult_;
  uint64_t connectionId_;
  uint64_t serverCaps_ = 0;

  bool ackReceived_;

  SharedMemoryPools pools_;

  std::string cluster_;
  int hostId_;
  int flags_;
  int timeoutMs_;
  bool forkChild_;
};

} // namespace ipc
