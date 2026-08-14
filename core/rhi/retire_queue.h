/**
 * @file retire_queue.h
 * @brief 资源退休队列:修复"destroy 时 GPU 可能仍在读"的隐患。
 *
 * 语义:destroyXxx 使句柄立即失效(调用方不得再用),底层资源的释放闭包
 * 打上当前帧序号入队;待该帧被后端的帧完成机制确认后才真正执行释放。
 * waitIdle()/设备析构 会清空队列(flushAll)。
 */
#pragma once
#include <cstdint>
#include <deque>
#include <functional>

namespace rd {

class RetireQueue {
public:
  /// 标记帧完成(≤ frame 的退休项全部执行);由后端帧完成机制驱动。
  void onFrameComplete(uint64_t frame) {
    if (frame > completedFrame_) completedFrame_ = frame;
    while (!queue_.empty() && queue_.front().first <= completedFrame_) {
      queue_.front().second();
      queue_.pop_front();
    }
  }
  /// 提交退休释放动作;frame 为当前帧序号。
  void retire(uint64_t frame, std::function<void()> fn) {
    queue_.emplace_back(frame, std::move(fn));
  }
  /// 强制全部执行(waitIdle/析构路径);帧序号单调不回退。
  void flushAll() {
    while (!queue_.empty()) {
      queue_.front().second();
      queue_.pop_front();
    }
  }
  /// 待退休数量(诊断/测试用)。
  size_t pending() const { return queue_.size(); }

private:
  uint64_t completedFrame_ = 0;
  std::deque<std::pair<uint64_t, std::function<void()>>> queue_;
};

} // namespace rd
