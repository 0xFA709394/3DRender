// TaskQueue 的实现：标准 mutex + condition_variable 的 MPSC 队列。
// 注意：任务在「锁外」执行——持锁期间只做入队/出队，避免任务体阻塞生产者。
#include "foundation/task_queue.h"

namespace rd {

void TaskQueue::post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(std::move(fn));
  }
  // 锁外通知，减少惊群与锁竞争。
  cv_.notify_one();
}

bool TaskQueue::tryPop() {
  std::function<void()> fn;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return false;
    fn = std::move(queue_.front());
    queue_.pop();
  }
  fn();  // 锁外执行任务
  return true;
}

void TaskQueue::waitAndPop() {
  std::function<void()> fn;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    // 谓词等待，防虚假唤醒。
    cv_.wait(lock, [&] { return !queue_.empty(); });
    fn = std::move(queue_.front());
    queue_.pop();
  }
  fn();  // 锁外执行任务
}

} // namespace rd
