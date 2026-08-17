/**
 * @file animator.h
 * @brief Animator:glTF 动画播放(线性插值/rotation slerp)+ 双 clip 交叉淡入。
 * 纯 CPU 计算,不碰 GPU;输出节点全局矩阵与关节矩阵(渲染层上传 JointUBO)。
 */
#pragma once
#include "foundation/math.h"
#include "resource/gltf_loader.h"
#include <vector>

namespace rd::scene {

class Animator {
public:
  /// 绑定模型(须含 nodes;animations 可为空——bind 后 playing=false)。
  bool bind(const ModelAsset& model);
  void play(uint32_t clipIndex);                        ///< 立即切换(loop)
  void playWithFade(uint32_t clipIndex, float fadeSec); ///< 交叉淡入
  void pause(bool p) { paused_ = p; }                   ///< 暂停/继续
  /// 推进时间并计算节点全局矩阵 + jointMatrices(skin 0)。
  void update(float dt);
  /// 当前关节矩阵(globalJoint × inverseBind),渲染层上传 JointUBO。
  const std::vector<math::Mat4>& jointMatrices() const { return jointMatrices_; }
  /// 节点全局矩阵(mesh 节点变换用)。
  const std::vector<math::Mat4>& nodeGlobals() const { return nodeGlobals_; }
  bool playing() const { return playing_; }
  uint32_t clipCount() const { return uint32_t(clips_); }

private:
  struct ClipState {
    int32_t index = -1;
    float time = 0.0f;
  };
  void sampleClip(const AnimClipData& clip, float time, std::vector<math::Vec3>& outT,
                  std::vector<math::Quat>& outR, std::vector<math::Vec3>& outS);
  void computeGlobals();   // locals → nodeGlobals_(沿 parent 链)
  void computeJoints();    // nodeGlobals_ × IBM → jointMatrices_

  const ModelAsset* model_ = nullptr;
  ClipState active_, fadeIn_;
  float fadeDuration_ = 0.0f, fadeElapsed_ = 0.0f;
  bool playing_ = false, paused_ = false;
  size_t clips_ = 0;
  std::vector<math::Vec3> localT_;
  std::vector<math::Quat> localR_;
  std::vector<math::Vec3> localS_;
  std::vector<math::Mat4> nodeGlobals_;
  std::vector<math::Mat4> jointMatrices_;
};

} // namespace rd::scene
