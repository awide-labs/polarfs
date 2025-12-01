#include <folly/SocketAddress.h>
#include <folly/io/Cursor.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/fdsock/AsyncFdSocket.h>
#include <semaphore.h>

#include "SharedIndexedMemPool.h"
#include "lib/dclcrwlock.h"
#include "memfd.h"
#include "proto.h"
#include "shm.h"

#include "pfsd_common.h"

namespace ipc {

struct ServerQueue {
  std::unique_ptr<Queue> queue;
  std::unique_ptr<MemFd> memfd;
};

template <typename Queue> class BestOfTwoPolicy {
public:
  static constexpr size_t QUEUE_DEPTH_LOOKUP_THRESHOLD = 4;
  static constexpr size_t QUEUE_DEPTH_SWITCH_THRESHOLD = 2;

  BestOfTwoPolicy(std::vector<Queue> queues) : queues_(queues) {}

  Queue pick() {
    auto *q1 = queues_[folly::AccessSpreader<>::current(queues_.size())];
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

class Session : public folly::AsyncSocket::ConnectCallback,
                public folly::AsyncTransport::WriteCallback,
                public folly::AsyncTransport::ReadCallback {
  using LoadBalancingPolicy = BestOfTwoPolicy<Queue *>;

public:
  explicit Session()
      : socket_(&evb_), bufferQueue_(folly::IOBufQueue::cacheChainLength()),
        error_(false), connected_(false), queuesAreReady_(false), fdSeqNum_(0),
        serverHelloReceived_(false, 0), ackReceived_(false), forkChild_(false) {
    rwLock_.init();
  }

  ~Session() { rwLock_.destroy(); }

  void setPbdname(std::string pbdname) { pbdname_ = pbdname; }

  bool start(std::string cluster, int host_id, int flags, int timeoutMs) {
    eventLoopThread_ = std::thread([this] {
      std::string sockPath = makeSockPath(pbdname_);
      folly::SocketAddress addr;
      addr.setFromPath(sockPath);
      socket_.connect(this, addr, 1000); // timeout: 1000ms
      socket_.setReadCB(this);

      evb_.loopForever();
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
    evb_.terminateLoopSoon();
    eventLoopThread_.join();
    return true;
  }

  void atforkChild() { forkChild_ = true; }

  bool restart(std::string cluster, int host_id, int flags, int timeoutMs) {
    return shutdown() && start(cluster, host_id, flags, timeoutMs);
  }

  std::optional<uint64_t>
  registerMemPool(std::unique_ptr<SharedIndexedMemPool<>> pool,
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

  bool allocRequest(SharedMemoryPools::AllocResult &r) {
    return pools_.allocRequest(r);
  }

  void free(SharedMemoryPools::AllocResult &r) { pools_.free(r); }

  void freeRequest(SharedMemoryPools::AllocResult &r) { pools_.freeRequest(r); }

  uint64_t connectionId() { return connectionId_; }

private:

  bool sendBuffers() {
    std::shared_lock lk(rwLock_);

    folly::SocketFds::ToSend toSend;
    RegisterBuffersMessage message;

    for (auto desc : pools_.allBuffers()) {
      message.buffers.push_back(BufferDesc{desc.id, desc.size});
      toSend.push_back(std::make_shared<folly::File>(desc.fd));
      PFSD_CLIENT_LOG("Sending buf %zu size %zu", desc.id, desc.size);
    }

    ackReceived_ = false;

    evb_.runInEventBaseThreadAndWait([this, &message, &toSend] {
      folly::SocketFds sockFds(toSend);
      if (sockFds.empty()) {
        socket_.writeChain(this, message.serialize());
      } else {
        sockFds.setFdSocketSeqNumOnce(fdSeqNum_);
        socket_.writeChainWithFds(this, message.serialize(),
                                  std::move(sockFds));
        fdSeqNum_ += message.buffers.size();
      }
      PFSD_CLIENT_LOG("Sent %zu memfds to server", message.buffers.size());
    });

    waitEvent([this] { return ackReceived_ || error_; });

    return !error_;
  }

  int sendHello(std::string cluster, int host_id, int flags, int timeoutMs) {
    evb_.runInEventBaseThreadAndWait([&cluster, this, host_id, flags] {
      ClientHelloMessage message;
      message.cluster = cluster;
      message.pbdname = pbdname_;
      message.host_id = host_id;
      message.flags = flags;

      socket_.writeChain(this, message.serialize());
    });

    waitEventMs([this] { return serverHelloReceived_.first; }, timeoutMs);

    if (!serverHelloReceived_.first) {
      return -1;
    }

    return serverHelloReceived_.second;
  }

  void connectSuccess() noexcept override {
    PFSD_CLIENT_LOG("Connected server");
    postEvent([this] { connected_ = true; });
  }

  void connectErr(const folly::AsyncSocketException &ex) noexcept override {
    PFSD_CLIENT_ELOG("Connection error: '%s'", ex.what());
    postEvent([this] { error_ = true; });
  }

  void writeSuccess() noexcept override { PFSD_CLIENT_LOG("Write success"); }

  void writeErr(size_t bytesWritten,
                const folly::AsyncSocketException &ex) noexcept override {
    PFSD_CLIENT_ELOG("Write error: '%s', bytes written: %zu", ex.what(),
                     bytesWritten);
  }

  void getReadBuffer(void **buf, size_t *len) noexcept override {
    *buf = transferBuf_;
    *len = sizeof(transferBuf_);
  }

  void readDataAvailable(size_t len) noexcept override {
    PFSD_CLIENT_LOG("Client received: %zu bytes", len);

    bufferQueue_.append(folly::IOBuf::copyBuffer(transferBuf_, len));

    while (bufferQueue_.chainLength() > 0) {
      int type;
      size_t len;

      if (!readMessageTypeAndLen(bufferQueue_.front(), type, len)) {
        break;
      }

      PFSD_CLIENT_LOG("Got message type: %d len: %zu", type, len);

      if (bufferQueue_.chainLength() < len) {
        break;
      }

      auto iobuf = bufferQueue_.split(len);

      if (type == ACK) {
        AckMessage message;

        if (!message.deserialize(iobuf)) {
          break;
        }

        postEvent([this] { ackReceived_ = true; });
      }

      if (type == REGISTER_QUEUES) {
        RegisterQueuesMessage message;

        if (!message.deserialize(iobuf)) {
          break;
        }

        int i = 0;
        for (auto &buf : message.buffers) {
          PFSD_CLIENT_LOG("Client received queue buffer id: %lu size: %zu",
                          message.buffers[i].id, message.buffers[i].size);
          i++;
        }

        if (!message.buffers.empty()) {
          auto receivedFds = socket_.popNextReceivedFds().releaseReceived();

          int i = 0;
          for (auto &fd : receivedFds) {
            auto memfd =
                std::make_unique<MemFd>(std::move(fd), message.buffers[i].size);
            auto queue = attachQueue(memfd->buf(), memfd->size());
            queues_.push_back(ServerQueue{std::move(queue), std::move(memfd)});
            i++;
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

        continue;
      }

      if (type == SERVER_HELLO) {
        ServerHelloMessage message;

        if (!message.deserialize(iobuf)) {
          break;
        }

        postEvent([this, &message] {
          connectionId_ = message.connectionId;
          serverHelloReceived_.first = true;
          serverHelloReceived_.second = message.error;
        });

        continue;
      }

      break;
    }
  }

  void readEOF() noexcept override { PFSD_CLIENT_LOG("Client disconnected."); }

  void readErr(const folly::AsyncSocketException &ex) noexcept override {
    PFSD_CLIENT_ELOG("Read error: %s", ex.what());
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

  template <typename Predicate> void waitEventMs(Predicate p, int timeoutMs) {
    std::unique_lock lk(cvMutex_);
    cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), p);
  }

  char transferBuf_[1024];
  folly::EventBase evb_;
  folly::AsyncFdSocket socket_;
  std::string pbdname_;
  folly::IOBufQueue bufferQueue_;
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
  std::atomic<int> fdSeqNum_;

  std::pair<bool, int> serverHelloReceived_;
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
