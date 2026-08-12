// TaskQueue 的单元测试：FIFO 顺序、阻塞消费语义。
#include <gtest/gtest.h>
#include "foundation/task_queue.h"
#include <thread>
#include <vector>

// 多任务 post 后按 FIFO 顺序执行
TEST(TaskQueue, ExecutesInFifoOrder) {
  rd::TaskQueue q;
  std::vector<int> order;
  for (int i = 0; i < 5; ++i) q.post([&order, i] { order.push_back(i); });
  while (q.tryPop()) {
  }
  ASSERT_EQ(order.size(), 5u);
  for (int i = 0; i < 5; ++i) EXPECT_EQ(order[i], i);
}

// waitAndPop 在队列空时阻塞，post 后被唤醒执行
TEST(TaskQueue, WaitAndPopBlocksUntilWork) {
  rd::TaskQueue q;
  int ran = 0;
  std::thread consumer([&] { q.waitAndPop(); ran = 1; });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(ran, 0); // 无任务时应阻塞
  q.post([&] {});
  consumer.join();
  EXPECT_EQ(ran, 1);
}
