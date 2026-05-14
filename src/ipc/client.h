#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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

  bool start(std::string cluster, int host_id, int flags, int timeoutMs) {
    eventLoopThread_ = std::thread([this] {
      try {
        std::string sockPath = makeSockPath(pbdname_);
        int fd = pfsutil::connectUnix(sockPath, 1000);
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
    timeoutMs_ = timeoutMs;

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

    waitEvent([this] { return ackReceived_ || error_; });

    return !error_;
  }

  int sendHello(std::string cluster, int host_id, int flags, int timeoutMs) {
    evb_.runInLoopAndWait([&cluster, this, host_id, flags] {
      ClientHelloMessage message;
      message.cluster = cluster;
      message.pbdname = pbdname_;
      message.host_id = host_id;
      message.flags = flags;

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

  void onError(int err) { PFSD_CLIENT_ELOG("Read error: %d", err); }

  template <typename Function> void postEvent(Function func) {
    std::unique_lock lk(cvMutex_);
    func();
    cv_.notify_all();
  }

  template <typename Predicate> void waitEvent(Predicate p) {
    std::unique_lock lk(cvMutex_);
    cv_.wait(lk, p);
  }

  template <typename Predicate> void waitEventMs(Predicate p, int timeoutMs) {
    std::unique_lock lk(cvMutex_);
    cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), p);
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

  std::vector<ServerQueue> queues_;
  DCLCRWLock rwLock_;
  std::unique_ptr<LoadBalancingPolicy> policy_;

  std::queue<SharedBufferInfo> pendingSharedBuffers_;
  int fdSeqNum_;

  std::pair<bool, int> serverHelloReceived_;
  std::pair<bool, int> remountResult_;
  uint64_t connectionId_;

  bool ackReceived_;

  SharedMemoryPools pools_;

  std::string cluster_;
  int hostId_;
  int flags_;
  int timeoutMs_;
  bool forkChild_;
};

} // namespace ipc
