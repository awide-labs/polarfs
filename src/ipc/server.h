#pragma once

#include <folly/SocketAddress.h>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/io/Cursor.h>
#include <folly/io/async/AsyncServerSocket.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/fdsock/AsyncFdSocket.h>
#include <sys/un.h>
#include <unistd.h>

#include "memfd.h"
#include "proto.h"

#include "pfsd_zlog.h"
#include <pfs_mount.h>

namespace ipc {

class BufferId {
public:
  BufferId(uint64_t connId, uint64_t bufId)
      : connectionId_(connId), bufferId_(bufId) {}

  BufferId() : connectionId_(0), bufferId_(0) {}

  uint64_t connectionId() const { return connectionId_; }
  uint64_t bufferId() const { return bufferId_; }

  bool operator==(const BufferId &other) const {
    return connectionId_ == other.connectionId_ && bufferId_ == other.bufferId_;
  }

private:
  uint64_t connectionId_;
  uint64_t bufferId_;

  friend struct std::hash<BufferId>;
};

} // namespace ipc

namespace std {
template <> struct hash<ipc::BufferId> {
  auto operator()(const ipc::BufferId &id) const noexcept {
    const uint64_t a = id.connectionId();
    const uint64_t b = id.bufferId();
    return std::hash<uint64_t>()(
        a ^ (b + 0x9e3779b97f4a7c15 + (a << 6) + (a >> 2)));
  }
};
} // namespace std

namespace ipc {

class BufferTable {
public:
  uint64_t addBuffer(uint64_t connectionId, uint64_t id,
                     std::unique_ptr<MemFd> memfd, size_t size) {
    BufferId bid(connectionId, id);
    buffers_[bid] = std::move(memfd);
    buffersPerConnection_[connectionId].push_back(bid);
    return id;
  }

  void eraseBuffer(uint64_t connectionId, uint64_t id) {
    BufferId bid(connectionId, id);
    buffers_.erase(bid);
    auto &v = buffersPerConnection_[connectionId];
    v.erase(std::remove(v.begin(), v.end(), bid), v.end());
    if (v.empty()) {
      buffersPerConnection_.erase(connectionId);
    }
  }

  void eraseBuffers(uint64_t connectionId) {
    for (auto id : buffersPerConnection_[connectionId]) {
      buffers_.erase(id);
    }
    buffersPerConnection_.erase(connectionId);
  }

  template <typename T>
  T *getPtr(uint64_t connectionId, uint64_t id, off_t offset) {
    auto iter = buffers_.find(BufferId(connectionId, id));
    if (iter != buffers_.cend()) {
      return reinterpret_cast<T *>((char *)iter->second->buf() + offset);
    }
    return nullptr;
  }

private:
  std::atomic<uint64_t> count_{0};
  std::unordered_map<BufferId, std::unique_ptr<MemFd>> buffers_;
  std::unordered_map<uint64_t, std::vector<BufferId>> buffersPerConnection_;
};

class ClientContext;

class Server : public folly::AsyncServerSocket::AcceptCallback {
public:
  explicit Server(folly::EventBase &evb, std::string pbdname)
      : evb_(evb), pbdname_(pbdname),
        serverSocket_(folly::AsyncServerSocket::newSocket(&evb)), done_(false) {
    rwLock_.init();
    sockPath_ = makeSockPath(pbdname_);
    folly::SocketAddress addr;
    addr.setFromPath(sockPath_);
    unlink(sockPath_.c_str());
    serverSocket_->bind(addr);
    serverSocket_->listen(128);
    serverSocket_->addAcceptCallback(this, &evb_);
    serverSocket_->startAccepting();
    chmod(sockPath_.c_str(), 0666);
  }

  ~Server() { rwLock_.destroy(); }

  void start() {
    pfsd_info("Server listening on %s", sockPath_.c_str());
    while (!done_) {
      evb_.loopOnce();
    }
  }

  void addBuffer(uint64_t clientId, uint64_t id, std::unique_ptr<MemFd> memfd,
                 size_t size) {
    std::lock_guard lk(rwLock_);
    bufferTable_.addBuffer(clientId, id, std::move(memfd), size);
  }

  void addQueue(std::unique_ptr<MemFd> memfd) {
    std::lock_guard lk(rwLock_);
    queueMemfds_.push_back(std::move(memfd));
  }

  template <typename Function> void forEachQueue(Function func) {
    std::shared_lock lk(rwLock_);
    size_t i = 0;
    size_t count = queueMemfds_.size();
    for (const auto &memfd : queueMemfds_) {
      func(memfd, i == count - 1);
      i++;
    }
  }

  template <typename T>
  std::pair<T *, std::shared_lock<DCLCRWLock>>
  getPtrWithLock(uint64_t connId, uint64_t bufferId, off_t offset) {
    std::shared_lock lk(rwLock_);
    return std::make_pair(bufferTable_.getPtr<T>(connId, bufferId, offset),
                          std::move(lk));
  }

  template <typename T>
  T *getPtr(uint64_t connId, uint64_t bufferId, off_t offset) {
    return bufferTable_.getPtr<T>(connId, bufferId, offset);
  }

  void cleanupClient(uint64_t clientId) {
    evb_.runInEventBaseThread([this, clientId] {
      bufferTable_.eraseBuffers(clientId);
      clients_.erase(clientId);
      pfsd_info("Cleaned up client %lu", clientId);
    });
  }

private:
  void connectionAccepted(folly::NetworkSocket fd,
                          const folly::SocketAddress &clientAddr,
                          AcceptInfo info) noexcept override;

  folly::EventBase &evb_;
  std::string pbdname_;
  std::string sockPath_;
  std::shared_ptr<folly::AsyncServerSocket> serverSocket_;
  std::unordered_map<uint64_t, std::unique_ptr<ClientContext>> clients_;
  bool done_;
  BufferTable bufferTable_;
  std::vector<std::unique_ptr<MemFd>> queueMemfds_;
  std::atomic<uint64_t> nextClientId_{};
  DCLCRWLock rwLock_;
};

class ClientContext : public folly::AsyncTransport::ReadCallback,
                      public folly::AsyncTransport::WriteCallback {
public:
  ClientContext(folly::EventBase &evb, folly::NetworkSocket &fd, Server *server,
                uint64_t clientId)
      : socket_(std::make_unique<folly::AsyncFdSocket>(&evb, fd)),
        bufferQueue_(folly::IOBufQueue::cacheChainLength()) {
    socket_->setReadCB(this);
    server_ = server;
    clientId_ = clientId;
  }

  void sendQueuesToClient() {
    RegisterQueuesMessage message;
    folly::SocketFds::ToSend toSend;
    uint64_t i = 0, bufid = 0;
    int seqNo = 0;
    server_->forEachQueue([&message, &toSend, &i, &seqNo, this, &bufid](
                              const std::unique_ptr<MemFd> &memfd, bool last) {
      message.buffers.push_back(BufferDesc{bufid++, memfd->size()});
      toSend.push_back(std::make_shared<folly::File>(memfd->fd()));

      i++;
      // see https://gist.github.com/kentonv/bc7592af98c68ba2738f4436920868dc
      if (i >= 253) {
        message.last = last;
        folly::SocketFds sockFds(toSend);
        sockFds.setFdSocketSeqNumOnce(seqNo);
        socket_->writeChainWithFds(this, message.serialize(),
                                   std::move(sockFds));
        message.buffers.clear();
        toSend.clear();
        seqNo += i;
        i = 0;
      }
    });

    message.last = true;

    folly::SocketFds sockFds(toSend);
    if (sockFds.empty()) {
      socket_->writeChain(this, message.serialize());
    } else {
      sockFds.setFdSocketSeqNumOnce(seqNo);
      socket_->writeChainWithFds(this, message.serialize(), std::move(sockFds));
    }
  }

private:
  void getReadBuffer(void **buf, size_t *len) override {
    *buf = transferBuf_;
    *len = sizeof(transferBuf_);
  }

  void readDataAvailable(size_t len) noexcept override {
    pfsd_info("Server received: %zu bytes", len);
    bufferQueue_.append(folly::IOBuf::copyBuffer(transferBuf_, len));

    while (bufferQueue_.chainLength() > 0) {
      int type;
      size_t len;

      if (!readMessageTypeAndLen(bufferQueue_.front(), type, len)) {
        break;
      }

      pfsd_info("Got message type: %d len: %zu", type, len);

      if (bufferQueue_.chainLength() < len) {
        break;
      }

      auto iobuf = bufferQueue_.split(len);

      if (type == REGISTER_BUFFERS) {
        RegisterBuffersMessage message;

        if (!message.deserialize(iobuf)) {
          break;
        }

        auto receivedFds = socket_->popNextReceivedFds().releaseReceived();

        int i = 0;
        for (auto &fd : receivedFds) {
          pfsd_info("Received fd: %d id: %lu size: %zu", fd.fd(),
                    message.buffers[i].id, message.buffers[i].size);
          ++i;
        }

        i = 0;
        for (auto &fd : receivedFds) {
          size_t size = message.buffers[i].size;
          server_->addBuffer(
              clientId_, message.buffers[i].id,
              std::make_unique<MemFd>(folly::File(fd.fd()), size), size);
          ++i;
        }

        AckMessage ack;
        socket_->writeChain(this, ack.serialize());

        continue;
      }

      if (type == CLIENT_HELLO) {
        ClientHelloMessage message;

        if (!message.deserialize(iobuf)) {
          break;
        }

        int err =
            pfs_mount_acquire(message.cluster.c_str(), message.pbdname.c_str(),
                              message.host_id, message.flags);

        if (err != 0) {
          pfsd_error("Mount error: %d", err);
          host_id_ = -1;
        } else {
          pbdname_ = message.pbdname;
          host_id_ = message.host_id;
        }

        ServerHelloMessage reply;
        reply.connectionId = clientId_;
        reply.error = err;
        socket_->writeChain(this, reply.serialize());

        continue;
      }

      if (type == REMOUNT) {
        RemountMessage message;

        if (!message.deserialize(iobuf)) {
          break;
        }

        int err = pfs_remount(message.cluster.c_str(), message.pbdname.c_str(),
                              message.host_id, message.flags);

        if (err != 0) {
          pfsd_error("Remount error: %d", err);
        } else {
          pbdname_ = message.pbdname;
          host_id_ = message.host_id;
        }

        RemountResultMessage reply;
        reply.error = err;
        socket_->writeChain(this, reply.serialize());

        continue;
      }

      break;
    }
  }

  void readEOF() noexcept override {
    pfsd_info("Client disconnected");
    socket_->close();
    server_->cleanupClient(clientId_);
    if (host_id_ >= 0) {
      pfs_mount_release(pbdname_.c_str(), host_id_);
    }
  }

  void readErr(const folly::AsyncSocketException &ex) noexcept override {
    pfsd_error("Read error: %s", ex.what());
  }

  void writeSuccess() noexcept override { pfsd_info("Write success"); }

  void writeErr(size_t bytesWritten,
                const folly::AsyncSocketException &ex) noexcept override {
    pfsd_error("Write error: %s bytes written: %zu", ex.what(), bytesWritten);
  }

  char transferBuf_[1024];
  std::unique_ptr<folly::AsyncFdSocket> socket_;
  folly::IOBufQueue bufferQueue_;
  Server *server_;
  uint64_t clientId_;
  std::string pbdname_;
  int host_id_;
};

} // namespace ipc
