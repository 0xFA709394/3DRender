/**
 * @file task_queue.h
 * @brief 多生产者单消费者（MPSC）任务队列。
 *
 * 渲染线程模型的基础件：任意线程 post 任务，渲染线程 tryPop/waitAndPop 逐个消费。
 * 例如平台层把「创建纹理」「resize swapchain」等调用从 UI 线程转交给渲染线程执行，
 * 以满足「同一 Device 的所有调用在同一线程」的内核线程约束。
 */
#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>

namespace rd {

/**
 * @brief 线程安全的任务队列（多生产者单消费者）。
 *
 * - post 可从任意线程调用（多生产者）。
 * - tryPop/waitAndPop 只能由唯一消费者线程调用；任务在消费者线程上执行。
 * - 任务类型为 std::function<void()>，无返回值；需要结果时由任务内部自行回传。
 */
class TaskQueue {
public:
  /// 入队一个任务；唤醒可能阻塞在 waitAndPop 的消费者。
  void post(std::function<void()> fn);
  /// 非阻塞消费：有任务则取出一个并执行、返回 true；队列为空返回 false。
  bool tryPop();
  /// 阻塞消费：挂起直到队列非空，取出一个任务并执行。
  void waitAndPop();

private:
  std::mutex mutex_;                          ///< 保护 queue_ 的互斥锁
  std::condition_variable cv_;                ///< 队列非空时通知消费者
  std::queue<std::function<void()>> queue_;   ///< FIFO 任务队列
};

} // namespace rd
