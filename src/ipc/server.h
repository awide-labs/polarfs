#pragma once

#include <atomic>
#include <cstring>
#include <memory>
#include <shared_mutex>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "memfd.h"
#include "pfs_event_loop.h"
#include "pfs_unix_socket.h"
#include "proto.h"

#include "lib/dclcrwlock.h"
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

class Server {
public:
  explicit Server(pfsutil::EventLoop &evb, std::string pbdname)
      : evb_(evb), pbdname_(pbdname), listener_(&evb), done_(false) {
    rwLock_.init();
    sockPath_ = makeSockPath(pbdname_);
    listener_.bind(sockPath_);
    listener_.start(128, [this](int fd) { onAccept(fd); });
    chmod(sockPath_.c_str(), 0666);
  }

  ~Server() { rwLock_.destroy(); }

  void start() {
    pfsd_info("Server listening on %s", sockPath_.c_str());
    evb_.loopForever();
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
    evb_.queueInLoop([this, clientId] {
      {
        std::lock_guard lk(rwLock_);
        bufferTable_.eraseBuffers(clientId);
      }
      clients_.erase(clientId);
      pfsd_info("Cleaned up client %lu", clientId);
    });
  }

private:
  void onAccept(int fd);

  pfsutil::EventLoop &evb_;
  std::string pbdname_;
  std::string sockPath_;
  pfsutil::UnixListener listener_;
  std::unordered_map<uint64_t, std::unique_ptr<ClientContext>> clients_;
  bool done_;
  BufferTable bufferTable_;
  std::vector<std::unique_ptr<MemFd>> queueMemfds_;
  std::atomic<uint64_t> nextClientId_{};
  DCLCRWLock rwLock_;
};

class ClientContext {
public:
  ClientContext(pfsutil::EventLoop &evb, int fd, Server *server,
                uint64_t clientId)
      : socket_(std::make_unique<pfsutil::UnixSocket>(&evb, fd)),
        server_(server), clientId_(clientId), host_id_(-1) {
    socket_->setReadCallback(
        [this](const uint8_t *data, size_t len) { onBytes(data, len); },
        [this] { onEof(); },
        [this](int err) { onError(err); });
  }

  void sendQueuesToClient() {
    constexpr size_t kFdsPerSend = 253; // SCM_RIGHTS limit
    RegisterQueuesMessage message;
    std::vector<int> toSend;
    uint64_t i = 0, bufid = 0;
    server_->forEachQueue([&message, &toSend, &i, this,
                           &bufid](const std::unique_ptr<MemFd> &memfd,
                                   bool last) {
      message.buffers.push_back(BufferDesc{bufid++, memfd->size()});
      toSend.push_back(memfd->fd());
      ++i;
      if (i >= kFdsPerSend) {
        message.last = last;
        socket_->writeWithFds(message.serialize(), std::move(toSend));
        message.buffers.clear();
        toSend.clear();
        i = 0;
      }
    });

    message.last = true;
    if (toSend.empty()) {
      socket_->write(message.serialize());
    } else {
      socket_->writeWithFds(message.serialize(), std::move(toSend));
    }
  }

private:
  void onBytes(const uint8_t *data, size_t len) {
    pfsd_info("Server received: %zu bytes", len);
    readBuf_.insert(readBuf_.end(), data, data + len);

    while (!readBuf_.empty()) {
      int type;
      size_t msgLen;
      if (!readMessageTypeAndLen(readBuf_.data(), readBuf_.size(), type,
                                 msgLen)) {
        break;
      }
      pfsd_info("Got message type: %d len: %zu", type, msgLen);
      if (readBuf_.size() < msgLen) {
        break;
      }

      const uint8_t *msg = readBuf_.data();

      if (type == REGISTER_BUFFERS) {
        RegisterBuffersMessage message;
        if (!message.deserialize(msg, msgLen)) {
          break;
        }

        auto receivedFds = socket_->popReceivedFds();
        for (size_t k = 0; k < receivedFds.size(); ++k) {
          pfsd_info("Received fd: %d id: %lu size: %zu", receivedFds[k],
                    message.buffers[k].id, message.buffers[k].size);
        }
        for (size_t k = 0; k < receivedFds.size(); ++k) {
          size_t size = message.buffers[k].size;
          server_->addBuffer(
              clientId_, message.buffers[k].id,
              std::make_unique<MemFd>(receivedFds[k], size), size);
        }
        receivedFds.clear(); // ownership transferred to MemFd

        AckMessage ack;
        socket_->write(ack.serialize());
      } else if (type == CLIENT_HELLO) {
        ClientHelloMessage message;
        if (!message.deserialize(msg, msgLen)) {
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
        socket_->write(reply.serialize());
      } else if (type == REMOUNT) {
        RemountMessage message;
        if (!message.deserialize(msg, msgLen)) {
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
        socket_->write(reply.serialize());
      } else {
        // Unknown message: stop framing further (would desync the stream).
        break;
      }

      readBuf_.erase(readBuf_.begin(), readBuf_.begin() + msgLen);
    }
  }

  void teardown() {
    if (host_id_ >= 0) {
      pfs_mount_release(pbdname_.c_str(), host_id_);
    }
    server_->cleanupClient(clientId_);
  }

  void onEof() {
    pfsd_info("Client disconnected");
    teardown();
  }

  void onError(int err) {
    pfsd_error("Socket error: %d (%s)", err, strerror(err));
    teardown();
  }

  std::unique_ptr<pfsutil::UnixSocket> socket_;
  std::vector<uint8_t> readBuf_;
  Server *server_;
  uint64_t clientId_;
  std::string pbdname_;
  int host_id_;
};

} // namespace ipc
