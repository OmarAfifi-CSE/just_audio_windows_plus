#include "../platform_thread.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <thread>

namespace {
// Exercise the real HWND and WndProc without a Flutter engine or timed sleeps.
void PumpMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

TEST(PlatformThreadDispatcher, WorkerPostsExecuteOnlyWhenPlatformPumpsMessages) {
  PlatformThreadDispatcher dispatcher;
  ASSERT_TRUE(dispatcher.available());
  ASSERT_TRUE(dispatcher.on_platform_thread());
  const DWORD platform = GetCurrentThreadId();
  DWORD callback_thread = 0;
  bool posted = false;
  bool worker_is_platform = true;
  std::thread worker([&] {
    worker_is_platform = dispatcher.on_platform_thread();
    posted = dispatcher.Post([&] { callback_thread = GetCurrentThreadId(); });
  });
  worker.join();
  EXPECT_FALSE(worker_is_platform);
  ASSERT_TRUE(posted);
  EXPECT_EQ(callback_thread, 0u);
  PumpMessages();
  EXPECT_EQ(callback_thread, platform);
}

TEST(PlatformThreadDispatcher, ReentrantPostsDrainWithoutDeadlockInFifoOrder) {
  PlatformThreadDispatcher dispatcher;
  ASSERT_TRUE(dispatcher.available());
  std::vector<int> calls;
  ASSERT_TRUE(dispatcher.Post([&] {
    calls.push_back(1);
    EXPECT_TRUE(dispatcher.Post([&] { calls.push_back(3); }));
  }));
  ASSERT_TRUE(dispatcher.Post([&] { calls.push_back(2); }));
  PumpMessages();
  EXPECT_EQ(calls, (std::vector<int>{1, 2, 3}));
}

TEST(PlatformThreadDispatcher, ExpiredOwnerDropsQueuedCallback) {
  PlatformThreadDispatcher dispatcher;
  ASSERT_TRUE(dispatcher.available());
  auto owner = std::make_shared<int>(0);
  std::weak_ptr<int> life = owner;
  int calls = 0;
  // This is the same weak-owner contract used by AudioPlayer's event closures.
  std::thread worker([&] {
    EXPECT_TRUE(dispatcher.Post([life, &calls] {
      if (life.expired()) return;
      ++calls;
    }));
  });
  worker.join();
  owner.reset();
  PumpMessages();
  EXPECT_EQ(calls, 0);
}

TEST(PlatformThreadDispatcher, DestructionDropsPendingWorkAndReleasesCaptures) {
  int calls = 0;
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> weak_capture = capture;
  {
    PlatformThreadDispatcher dispatcher;
    ASSERT_TRUE(dispatcher.available());
    ASSERT_TRUE(dispatcher.Post([capture, &calls] { ++calls; }));
    capture.reset();
    EXPECT_FALSE(weak_capture.expired());
  }
  EXPECT_TRUE(weak_capture.expired());
  PumpMessages();
  EXPECT_EQ(calls, 0);
}

TEST(PlatformThreadDispatcher, ShutdownDropsPendingWorkAndRejectsWorkerPosts) {
  PlatformThreadDispatcher dispatcher;
  ASSERT_TRUE(dispatcher.available());
  int calls = 0;
  ASSERT_TRUE(dispatcher.Post([&] { ++calls; }));
  dispatcher.Shutdown();
  EXPECT_FALSE(dispatcher.available());
  bool posted = true;
  std::thread worker([&] { posted = dispatcher.Post([&] { ++calls; }); });
  worker.join();
  EXPECT_FALSE(posted);
  dispatcher.Shutdown();
  PumpMessages();
  EXPECT_EQ(calls, 0);
}

TEST(PlatformThreadDispatcher, ShutdownInsideCallbackDropsRestOfDrainedBatch) {
  PlatformThreadDispatcher dispatcher;
  ASSERT_TRUE(dispatcher.available());
  int calls = 0;
  ASSERT_TRUE(dispatcher.Post([&] { dispatcher.Shutdown(); }));
  ASSERT_TRUE(dispatcher.Post([&] { ++calls; }));
  PumpMessages();
  EXPECT_FALSE(dispatcher.available());
  EXPECT_EQ(calls, 0);
}
}  // namespace
