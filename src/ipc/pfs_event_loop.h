/*
 * Single-threaded epoll event loop. Drop-in replacement for the slice of
 * folly::EventBase that PFS uses: watch / modify / unwatch fd-level events,
 * loopForever / terminate, and runInLoop / runInLoopAndWait for cross-thread
 * scheduling (eventfd-backed wakeup).
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pfsutil {

class EventLoop {
public:
  // Bitmask values for watch() / events callback parameter.
  enum : uint32_t {
    EV_READ  = 1u << 0,
    EV_WRITE = 1u << 1,
    EV_RDHUP = 1u << 2, // peer half-closed (mostly informational)
    EV_ERR   = 1u << 3, // EPOLLERR / EPOLLHUP
  };

  using Callback = std::function<void(uint32_t events)>;

  EventLoop();
  ~EventLoop();
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  // fd registration. Loop-thread only after loopForever() has started.
  void watch(int fd, uint32_t events, Callback cb);
  void modify(int fd, uint32_t events);
  void unwatch(int fd);

  // Run until terminate() is called.
  void loopForever();
  // Make loopForever() return after the current iteration. Thread-safe.
  void terminate();

  // Run fn on the loop thread. If called from the loop thread, runs inline.
  // Thread-safe.
  void runInLoop(std::function<void()> fn);
  // Same, but blocks until fn() returns. Must NOT be called from the loop
  // thread (would deadlock); inline-runs in that case as a courtesy.
  void runInLoopAndWait(std::function<void()> fn);

  bool inLoopThread() const noexcept;

private:
  void onWakeup();

  int epollFd_{-1};
  int wakeupFd_{-1}; // eventfd
  std::atomic<std::thread::id> loopThread_{};
  std::atomic<bool> stop_{false};

  struct Watch {
    Callback cb;
  };
  // unique_ptr keeps the address stable for use as epoll_event.data.ptr.
  std::unordered_map<int, std::unique_ptr<Watch>> watches_;

  std::mutex queueMu_;
  std::vector<std::function<void()>> pending_;
};

} // namespace pfsutil
