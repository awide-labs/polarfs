#pragma once

#include "folly/io/Cursor.h"
#include "folly/json/json.h"
#include "ipc/mpmc.h"
#include <cstdint>
#include <folly/dynamic.h>
#include <folly/io/async/fdsock/AsyncFdSocket.h>
#include <semaphore.h>

#include <pfsd_proto.h>

namespace ipc {

enum MessageType {
  CLIENT_HELLO,
  SERVER_HELLO,
  REGISTER_QUEUES,
  REGISTER_BUFFERS,
  ACK,
  REMOUNT,
  REMOUNT_RESULT,
};

struct BufferDesc {
  uint64_t id;
  uint64_t size;

  BufferDesc(const BufferDesc &) = default;
  BufferDesc(BufferDesc &&) = default;
  BufferDesc &operator=(const BufferDesc &) = default;
  BufferDesc &operator=(BufferDesc &&) = default;
  BufferDesc(uint64_t id, uint64_t size) : id(id), size(size) {}
};

struct Request : folly::cacheline_align_t {
  int memBufId;
  off_t offset;
  size_t size;
  int type;
  int32_t file_type;
  bool initialized;
  sem_t sem;

  union {
    struct {
      COMMON_REQUEST_HEADER;
    } common;
    growfs_request_t g_req;
    open_request_t o_req;
    read_request_t r_req;
    write_request_t w_req;
    ftruncate_request_t ft_req;
    truncate_request_t t_req;
    unlink_request_t un_req;
    fstat_request_t f_req;
    stat_request_t s_req;
    fallocate_request_t fa_req;
    chdir_request_t cd_req;
    mkdir_request_t mk_req;
    rmdir_request_t rm_req;
    opendir_request_t od_req;
    readdir_request_t rd_req;
    rename_request_t re_req;
    lseek_request_t l_req;
    access_request_t a_req;
  } req;
  union {
    struct {
      COMMON_RESPONSE_HEADER;
    } common;
    growfs_response_t g_rsp;
    open_response_t o_rsp;
    read_response_t r_rsp;
    write_response_t w_rsp;
    ftruncate_response_t ft_rsp;
    truncate_response_t t_rsp;
    unlink_response_t un_rsp;
    fstat_response_t f_rsp;
    stat_response_t s_rsp;
    fallocate_response_t fa_rsp;
    chdir_response_t cd_rsp;
    mkdir_response_t mk_rsp;
    rmdir_response_t rm_rsp;
    opendir_response_t od_rsp;
    readdir_response_t rd_rsp;
    rename_response_t re_rsp;
    lseek_response_t l_rsp;
    access_response_t a_rsp;
  } rsp;
};

struct RequestPtr {
  uint64_t connectionId;
  off_t offset;
  size_t size;
  int memBufId;
};

static inline std::string makeSockPath(std::string pbdname) {
  std::string sockPath = "/var/run/pfsd-" + pbdname + ".socket";
  return sockPath;
}

static inline bool readMessageTypeAndLen(const folly::IOBuf *buf, int &type,
                                         size_t &len) {
  if (buf->computeChainDataLength() < sizeof(uint32_t) * 2) {
    return false;
  }

  folly::io::Cursor cursor(buf);

  type = cursor.readLE<uint32_t>();
  len = cursor.readLE<uint32_t>();

  return true;
}

struct ClientHelloMessage {
  std::string cluster;
  std::string pbdname;
  int host_id;
  int flags;

  std::unique_ptr<folly::IOBuf> serialize() const {
    const size_t totalSize =
        sizeof(uint32_t) +                  // message type
        sizeof(uint32_t) +                  // total length
        sizeof(uint32_t) +                  // version
        sizeof(uint32_t) + cluster.size() + // cluster length + data
        sizeof(uint32_t) + pbdname.size() + // pbdname length + data
        sizeof(int32_t) +                   // host_id
        sizeof(int32_t);                    // flags

    auto buf = folly::IOBuf::create(totalSize);
    folly::io::Appender appender(buf.get(), 0);

    appender.writeLE<uint32_t>(CLIENT_HELLO);
    appender.writeLE<uint32_t>(totalSize);
    appender.writeLE<uint32_t>(1); // version

    appender.writeLE<uint32_t>(cluster.size());
    appender.push(reinterpret_cast<const uint8_t *>(cluster.data()),
                  cluster.size());

    appender.writeLE<uint32_t>(pbdname.size());
    appender.push(reinterpret_cast<const uint8_t *>(pbdname.data()),
                  pbdname.size());

    appender.writeLE<int32_t>(host_id);
    appender.writeLE<int32_t>(flags);

    return buf;
  }

  bool deserialize(std::unique_ptr<folly::IOBuf> &buf) {
    if (buf->computeChainDataLength() < sizeof(uint32_t) * 2) {
      return false;
    }

    folly::io::Cursor cursor(buf.get());

    int messageType = cursor.readLE<uint32_t>();
    size_t totalLength = cursor.readLE<uint32_t>();

    if (messageType != CLIENT_HELLO) {
      return false;
    }

    if (buf->computeChainCapacity() < totalLength) {
      return false;
    }

    auto version = cursor.readLE<uint32_t>();
    if (version > 1) {
      return false;
    }

    const uint32_t clusterLen = cursor.readLE<uint32_t>();
    cluster = cursor.readFixedString(clusterLen);

    const uint32_t pbdnameLen = cursor.readLE<uint32_t>();
    pbdname = cursor.readFixedString(pbdnameLen);

    host_id = cursor.readLE<int32_t>();
    flags = cursor.readLE<int32_t>();

    return true;
  }
};

struct RemountMessage {
  std::string cluster;
  std::string pbdname;
  int host_id;
  int flags;

  std::unique_ptr<folly::IOBuf> serialize() const {
    const size_t totalSize =
        sizeof(uint32_t) +                  // message type
        sizeof(uint32_t) +                  // total length
        sizeof(uint32_t) +                  // version
        sizeof(uint32_t) + cluster.size() + // cluster length + data
        sizeof(uint32_t) + pbdname.size() + // pbdname length + data
        sizeof(int32_t) +                   // host_id
        sizeof(int32_t);                    // flags

    auto buf = folly::IOBuf::create(totalSize);
    folly::io::Appender appender(buf.get(), 0);

    appender.writeLE<uint32_t>(REMOUNT);
    appender.writeLE<uint32_t>(totalSize);
    appender.writeLE<uint32_t>(1); // version

    appender.writeLE<uint32_t>(cluster.size());
    appender.push(reinterpret_cast<const uint8_t *>(cluster.data()),
                  cluster.size());

    appender.writeLE<uint32_t>(pbdname.size());
    appender.push(reinterpret_cast<const uint8_t *>(pbdname.data()),
                  pbdname.size());

    appender.writeLE<int32_t>(host_id);
    appender.writeLE<int32_t>(flags);

    return buf;
  }

  bool deserialize(std::unique_ptr<folly::IOBuf> &buf) {
    if (buf->computeChainDataLength() < sizeof(uint32_t) * 2) {
      return false;
    }

    folly::io::Cursor cursor(buf.get());

    int messageType = cursor.readLE<uint32_t>();
    size_t totalLength = cursor.readLE<uint32_t>();

    if (messageType != REMOUNT) {
      return false;
    }

    if (buf->computeChainCapacity() < totalLength) {
      return false;
    }

    auto version = cursor.readLE<uint32_t>();
    if (version > 1) {
      return false;
    }

    const uint32_t clusterLen = cursor.readLE<uint32_t>();
    cluster = cursor.readFixedString(clusterLen);

    const uint32_t pbdnameLen = cursor.readLE<uint32_t>();
    pbdname = cursor.readFixedString(pbdnameLen);

    host_id = cursor.readLE<int32_t>();
    flags = cursor.readLE<int32_t>();

    return true;
  }
};

struct RegisterQueuesMessage {
  std::vector<BufferDesc> buffers;
  bool last{false};

  std::unique_ptr<folly::IOBuf> serialize() {
    const size_t totalSize =
        sizeof(uint32_t) +                                      // message type
        sizeof(uint32_t) +                                      // total length
        sizeof(uint32_t) +                                      // version
        sizeof(uint32_t) +                                      // last
        (sizeof(uint64_t) + sizeof(uint64_t)) * buffers.size(); // buffers

    auto buf = folly::IOBuf::create(totalSize);
    folly::io::Appender appender(buf.get(), 0);

    appender.writeLE<uint32_t>(REGISTER_QUEUES);
    appender.writeLE<uint32_t>(totalSize);
    appender.writeLE<uint32_t>(1); // version
    appender.writeLE<uint32_t>(last);

    for (auto &buffer : buffers) {
      appender.writeLE<uint64_t>(buffer.id);
      appender.writeLE<uint64_t>(buffer.size);
    }

    return buf;
  }

  bool deserialize(std::unique_ptr<folly::IOBuf> &buf) {
    if (buf->computeChainDataLength() < sizeof(uint32_t) * 2) {
      return false;
    }

    folly::io::Cursor cursor(buf.get());

    int messageType = cursor.readLE<uint32_t>();
    size_t totalLength = cursor.readLE<uint32_t>();

    if (messageType != REGISTER_QUEUES) {
      return false;
    }

    if (buf->computeChainCapacity() < totalLength) {
      return false;
    }

    auto version = cursor.readLE<uint32_t>();
    if (version > 1) {
      return false;
    }
    last = cursor.readLE<uint32_t>();

    while (cursor.getCurrentPosition() < totalLength) {
      uint64_t id = cursor.readLE<uint64_t>();
      uint64_t size = cursor.readLE<uint64_t>();
      buffers.emplace_back(id, size);
    }

    return true;
  }
};

struct RegisterBuffersMessage {
  std::vector<BufferDesc> buffers;
  bool last{false};

  std::unique_ptr<folly::IOBuf> serialize() {
    const size_t totalSize =
        sizeof(uint32_t) +                                      // message type
        sizeof(uint32_t) +                                      // total length
        sizeof(uint32_t) +                                      // version
        sizeof(uint32_t) +                                      // last
        (sizeof(uint64_t) + sizeof(uint64_t)) * buffers.size(); // buffers

    auto buf = folly::IOBuf::create(totalSize);
    folly::io::Appender appender(buf.get(), 0);

    appender.writeLE<uint32_t>(REGISTER_BUFFERS);
    appender.writeLE<uint32_t>(totalSize);
    appender.writeLE<uint32_t>(1); // version
    appender.writeLE<uint32_t>(last);

    for (auto &buffer : buffers) {
      appender.writeLE<uint64_t>(buffer.id);
      appender.writeLE<uint64_t>(buffer.size);
    }

    return buf;
  }

  bool deserialize(std::unique_ptr<folly::IOBuf> &buf) {
    if (buf->computeChainDataLength() < sizeof(uint32_t) * 2) {
      return false;
    }

    folly::io::Cursor cursor(buf.get());

    int messageType = cursor.readLE<uint32_t>();
    size_t totalLength = cursor.readLE<uint32_t>();

    if (messageType != REGISTER_BUFFERS) {
      return false;
    }

    if (buf->computeChainCapacity() < totalLength) {
      return false;
    }

    auto version = cursor.readLE<uint32_t>();
    if (version > 1) {
      return false;
    }
    last = cursor.readLE<uint32_t>();

    while (cursor.getCurrentPosition() < totalLength) {
      uint64_t id = cursor.readLE<uint64_t>();
      uint64_t size = cursor.readLE<uint64_t>();
      buffers.emplace_back(id, size);
    }

    return true;
  }
};

struct AckMessage {
  std::unique_ptr<folly::IOBuf> serialize() {
    const size_t totalSize = sizeof(uint32_t) + // message type
                             sizeof(uint32_t);  // total length
    auto buf = folly::IOBuf::create(totalSize);
    buf->append(totalSize);
    folly::io::RWPrivateCursor cursor(buf.get());
    cursor.writeLE<uint32_t>(ACK);
    cursor.writeLE<uint32_t>(totalSize);
    return buf;
  }

  bool deserialize(std::unique_ptr<folly::IOBuf> &buf) {
    if (buf->computeChainDataLength() < sizeof(uint32_t) * 2) {
      return false;
    }

    folly::io::Cursor cursor(buf.get());

    int type = cursor.readLE<uint32_t>();
    size_t len = cursor.readLE<uint32_t>();

    if (buf->computeChainCapacity() < len) {
      return false;
    }

    return true;
  }
};

struct ServerHelloMessage {
  uint64_t connectionId;
  int error;

  std::unique_ptr<folly::IOBuf> serialize() {
    const size_t totalSize = sizeof(uint32_t) + // message type
                             sizeof(uint32_t) + // total length
                             sizeof(uint32_t) + // version
                             sizeof(uint64_t) + // connectionId
                             sizeof(int32_t);   // error

    auto buf = folly::IOBuf::create(totalSize);
    folly::io::Appender appender(buf.get(), 0);

    appender.writeLE<uint32_t>(SERVER_HELLO);
    appender.writeLE<uint32_t>(totalSize);
    appender.writeLE<uint32_t>(1); // version

    appender.writeLE<uint64_t>(connectionId);
    appender.writeLE<int32_t>(error);

    return buf;
  }

  bool deserialize(std::unique_ptr<folly::IOBuf> &buf) {
    using dynamic = folly::dynamic;

    if (buf->computeChainDataLength() < sizeof(uint32_t) * 2) {
      return false;
    }

    folly::io::Cursor cursor(buf.get());

    int type = cursor.readLE<uint32_t>();
    size_t len = cursor.readLE<uint32_t>();

    if (buf->computeChainCapacity() < len) {
      return false;
    }

    auto version = cursor.readLE<uint32_t>();
    if (version > 1) {
      return false;
    }

    if (type != SERVER_HELLO) {
      return false;
    }

    connectionId = cursor.readLE<uint64_t>();
    error = cursor.readLE<int32_t>();

    return true;
  }
};

struct RemountResultMessage {
  int error;

  std::unique_ptr<folly::IOBuf> serialize() {
    const size_t totalSize = sizeof(uint32_t) + // message type
                             sizeof(uint32_t) + // total length
                             sizeof(uint32_t) + // version
                             sizeof(int32_t);   // error

    auto buf = folly::IOBuf::create(totalSize);
    folly::io::Appender appender(buf.get(), 0);

    appender.writeLE<uint32_t>(REMOUNT_RESULT);
    appender.writeLE<uint32_t>(totalSize);
    appender.writeLE<uint32_t>(1); // version

    appender.writeLE<int32_t>(error);

    return buf;
  }

  bool deserialize(std::unique_ptr<folly::IOBuf> &buf) {
    using dynamic = folly::dynamic;

    if (buf->computeChainDataLength() < sizeof(uint32_t) * 2) {
      return false;
    }

    folly::io::Cursor cursor(buf.get());

    int type = cursor.readLE<uint32_t>();
    size_t len = cursor.readLE<uint32_t>();

    if (buf->computeChainCapacity() < len) {
      return false;
    }

    auto version = cursor.readLE<uint32_t>();
    if (version > 1) {
      return false;
    }

    if (type != REMOUNT_RESULT) {
      return false;
    }

    error = cursor.readLE<int32_t>();

    return true;
  }
};

} // namespace ipc
