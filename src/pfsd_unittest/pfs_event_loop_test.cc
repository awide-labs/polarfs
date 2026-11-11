/*
 * pfs_event_loop_test -- cross-thread scheduling in pfsutil::EventLoop.
 *
 * runInLoopAndWait used to keep its mutex, condition variable and done flag
 * in the caller's stack frame and notify after releasing the mutex, so the
 * caller could return before the loop thread's notify_one() and have that
 * write land in a dead or reused frame. RunInLoopAndWaitStress makes many
 * short round trips from several threads, the pattern that exposed it. A
 * plain build rarely crashes on it; under ThreadSanitizer the old code is
 * reported (notify_one racing ~condition_variable) on every run, so CMake
 * builds this test with -fsanitize=thread when the toolchain supports it.
 * ASan does not see the race: the write happens inside libc.
 *
 * No daemon or mount required.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

#include "ipc/pfs_event_loop.h"

using pfsutil::EventLoop;

namespace {

/* An EventLoop running on its own thread for the lifetime of the fixture. */
class EventLoopTest : public ::testing::Test {
protected:
  void SetUp() override {
    thread_ = std::thread([this] { loop_.loopForever(); });
    /* Wait until loopForever has recorded its thread id. */
    std::atomic<bool> started{false};
    loop_.runInLoop([&] { started = true; });
    while (!started)
      std::this_thread::yield();
  }

  void TearDown() override {
    loop_.terminate();
    thread_.join();
  }

  EventLoop loop_;
  std::thread thread_;
};

} // namespace

TEST_F(EventLoopTest, RunInLoopAndWaitRunsOnLoopThread) {
  std::thread::id ran_on;
  loop_.runInLoopAndWait([&] { ran_on = std::this_thread::get_id(); });
  EXPECT_EQ(ran_on, thread_.get_id());
}

TEST_F(EventLoopTest, RunInLoopAndWaitStress) {
  constexpr int kThreads = 4;
  constexpr int kIters = 50000;
  std::atomic<long> count{0};
  std::vector<std::thread> callers;
  for (int t = 0; t < kThreads; ++t) {
    callers.emplace_back([&] {
      for (int i = 0; i < kIters; ++i) {
        int local = 0;
        loop_.runInLoopAndWait([&] { local = 1; });
        ASSERT_EQ(local, 1);
        count.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto &th : callers)
    th.join();
  EXPECT_EQ(count.load(), long(kThreads) * kIters);
}

TEST_F(EventLoopTest, RunInLoopAndWaitPropagatesException) {
  EXPECT_THROW(
      loop_.runInLoopAndWait([] { throw std::runtime_error("boom"); }),
      std::runtime_error);
  /* The loop thread survives and keeps serving requests. */
  bool ran = false;
  loop_.runInLoopAndWait([&] { ran = true; });
  EXPECT_TRUE(ran);
}

TEST_F(EventLoopTest, RunInLoopAndWaitInlineOnLoopThread) {
  bool inner_ran = false;
  bool inner_was_inline = false;
  loop_.runInLoopAndWait([&] {
    loop_.runInLoopAndWait([&] { inner_ran = true; });
    /* Inline: already done by the time the nested call returns. */
    inner_was_inline = inner_ran;
  });
  EXPECT_TRUE(inner_ran);
  EXPECT_TRUE(inner_was_inline);
}

int
main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
