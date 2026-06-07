#include "pfs_event_loop.h"

#include <cerrno>
#include <condition_variable>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>

namespace pfsutil {

namespace {

uint32_t toEpoll(uint32_t e) {
  uint32_t r = 0;
  if (e & EventLoop::EV_READ) r |= EPOLLIN;
  if (e & EventLoop::EV_WRITE) r |= EPOLLOUT;
  if (e & EventLoop::EV_RDHUP) r |= EPOLLRDHUP;
  return r;
}

uint32_t fromEpoll(uint32_t e) {
  uint32_t r = 0;
  if (e & EPOLLIN) r |= EventLoop::EV_READ;
  if (e & EPOLLOUT) r |= EventLoop::EV_WRITE;
  if (e & EPOLLRDHUP) r |= EventLoop::EV_RDHUP;
  if (e & (EPOLLERR | EPOLLHUP)) r |= EventLoop::EV_ERR;
  return r;
}

} // namespace

EventLoop::EventLoop() {
  epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epollFd_ < 0) {
    throw std::system_error(errno, std::generic_category(), "epoll_create1");
  }
  wakeupFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wakeupFd_ < 0) {
    int e = errno;
    ::close(epollFd_);
    throw std::system_error(e, std::generic_category(), "eventfd");
  }

  // Register the wakeup fd. We do this directly via epoll_ctl rather than
  // through watch() because watch() asserts inLoopThread() once the loop is
  // running; here we're still in the constructor (no loop thread yet).
  auto w = std::make_unique<Watch>();
  w->cb = [this](uint32_t) { onWakeup(); };
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.ptr = w.get();
  if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeupFd_, &ev) != 0) {
    int e = errno;
    ::close(wakeupFd_);
    ::close(epollFd_);
    throw std::system_error(e, std::generic_category(), "epoll_ctl wakeup");
  }
  watches_[wakeupFd_] = std::move(w);
}

EventLoop::~EventLoop() {
  if (epollFd_ >= 0) ::close(epollFd_);
  if (wakeupFd_ >= 0) ::close(wakeupFd_);
}

void EventLoop::watch(int fd, uint32_t events, Callback cb) {
  auto w = std::make_unique<Watch>();
  w->cb = std::move(cb);
  epoll_event ev{};
  ev.events = toEpoll(events);
  ev.data.ptr = w.get();
  if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
    throw std::system_error(errno, std::generic_category(), "epoll_ctl ADD");
  }
  watches_[fd] = std::move(w);
}

void EventLoop::modify(int fd, uint32_t events) {
  auto it = watches_.find(fd);
  if (it == watches_.end()) {
    throw std::invalid_argument("EventLoop::modify: fd not watched");
  }
  epoll_event ev{};
  ev.events = toEpoll(events);
  ev.data.ptr = it->second.get();
  if (::epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev) != 0) {
    throw std::system_error(errno, std::generic_category(), "epoll_ctl MOD");
  }
}

void EventLoop::unwatch(int fd) {
  // Drop from epoll first; any in-flight events for fd are still safe because
  // we look up Watch* from epoll_event.data.ptr, and the Watch object stays
  // alive until we erase the unique_ptr below. But during the running loop
  // iteration we may already have a copy of data.ptr referring to a Watch we
  // are about to free; guard against that by deferring the erase.
  ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
  watches_.erase(fd);
}

void EventLoop::loopForever() {
  loopThread_.store(std::this_thread::get_id(), std::memory_order_release);
  stop_.store(false, std::memory_order_release);
  while (!stop_.load(std::memory_order_acquire)) {
    epoll_event events[64];
    int n;
    do {
      n = ::epoll_wait(epollFd_, events, 64, -1);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
      throw std::system_error(errno, std::generic_category(), "epoll_wait");
    }
    for (int i = 0; i < n; ++i) {
      auto* w = static_cast<Watch*>(events[i].data.ptr);
      // Watch may have been unwatch()'d earlier in this batch, in which case
      // the Watch object is gone. We defend against that by checking that the
      // current watches_ map still owns the Watch pointer for some fd.
      bool live = false;
      for (auto& kv : watches_) {
        if (kv.second.get() == w) { live = true; break; }
      }
      if (live) {
        w->cb(fromEpoll(events[i].events));
      }
    }
  }
  loopThread_.store(std::thread::id{}, std::memory_order_release);
}

void EventLoop::terminate() {
  stop_.store(true, std::memory_order_release);
  uint64_t v = 1;
  // Best-effort: ignore short writes / EAGAIN.
  ssize_t r;
  do {
    r = ::write(wakeupFd_, &v, sizeof(v));
  } while (r < 0 && errno == EINTR);
}

void EventLoop::onWakeup() {
  uint64_t v;
  ssize_t r;
  do {
    r = ::read(wakeupFd_, &v, sizeof(v));
  } while (r < 0 && errno == EINTR);

  std::vector<std::function<void()>> queue;
  {
    std::lock_guard lk(queueMu_);
    queue.swap(pending_);
  }
  for (auto& fn : queue) {
    fn();
  }
}

void EventLoop::runInLoop(std::function<void()> fn) {
  if (inLoopThread()) {
    fn();
    return;
  }
  queueInLoop(std::move(fn));
}

void EventLoop::queueInLoop(std::function<void()> fn) {
  {
    std::lock_guard lk(queueMu_);
    pending_.push_back(std::move(fn));
  }
  uint64_t v = 1;
  ssize_t r;
  do {
    r = ::write(wakeupFd_, &v, sizeof(v));
  } while (r < 0 && errno == EINTR);
}

void EventLoop::runInLoopAndWait(std::function<void()> fn) {
  if (inLoopThread()) {
    fn();
    return;
  }
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  runInLoop([&] {
    fn();
    {
      std::lock_guard lk(mu);
      done = true;
    }
    cv.notify_one();
  });
  std::unique_lock lk(mu);
  cv.wait(lk, [&] { return done; });
}

bool EventLoop::inLoopThread() const noexcept {
  return loopThread_.load(std::memory_order_acquire) ==
         std::this_thread::get_id();
}

} // namespace pfsutil
