// Animator 的实现:clip 采样(二分区间+线性/slerp)+ 交叉淡入 + 全局/关节矩阵。
#include "scene/animator.h"
#include "foundation/log.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/quaternion.hpp>

namespace rd::scene {

bool Animator::bind(const ModelAsset& model) {
  if (model.nodes.empty()) {
    RD_LOGW("scene.anim", "模型无节点层级,无法动画");
    return false;
  }
  model_ = &model;
  clips_ = model.animations.size();
  const size_t n = model.nodes.size();
  localT_.resize(n);
  localR_.resize(n);
  localS_.resize(n);
  nodeGlobals_.resize(n);
  // 初始:绑定姿态(节点静态 TRS)
  for (size_t i = 0; i < n; ++i) {
    const auto& nd = model.nodes[i];
    localT_[i] = math::Vec3(nd.translation[0], nd.translation[1], nd.translation[2]);
    localR_[i] = math::Quat(nd.rotation[3], nd.rotation[0], nd.rotation[1], nd.rotation[2]);
    localS_[i] = math::Vec3(nd.scale[0], nd.scale[1], nd.scale[2]);
  }
  // morph:首个 morph mesh 的静态权重(节点用于匹配 weights 动画通道)
  morphWeights_.clear();
  morphNode_ = -1;
  morphComps_ = 0;
  for (const auto& mesh : model.meshes) {
    if (mesh.morph) {
      morphWeights_ = mesh.morphWeights;
      morphNode_ = mesh.nodeIndex;
      morphComps_ = std::min<uint32_t>(uint32_t(mesh.morphWeights.size()), 8);
      morphWeights_.resize(morphComps_, 0.0f);
      break;
    }
  }
  computeGlobals();
  computeJoints();
  return true;
}

void Animator::play(uint32_t clipIndex) {
  if (clipIndex >= clips_) {
    RD_LOGW("scene.anim", "clip 越界 %u", clipIndex);
    return;
  }
  active_.index = int32_t(clipIndex);
  active_.time = 0;
  fadeIn_.index = -1;
  playing_ = true;
  paused_ = false;
}

void Animator::playWithFade(uint32_t clipIndex, float fadeSec) {
  if (clipIndex >= clips_) {
    RD_LOGW("scene.anim", "clip 越界 %u", clipIndex);
    return;
  }
  if (active_.index < 0) {  // 无旧 clip,直接播放
    play(clipIndex);
    return;
  }
  fadeIn_.index = int32_t(clipIndex);
  fadeIn_.time = 0;
  fadeDuration_ = std::max(fadeSec, 1e-3f);
  fadeElapsed_ = 0;
  playing_ = true;
  paused_ = false;
}

namespace {
/// 二分查找时间区间;输出 (i, frac):times[i]..times[i+1],frac∈[0,1]。
void findSegment(const std::vector<float>& times, float t, uint32_t& i, float& frac) {
  if (times.size() <= 1 || t <= times.front()) {
    i = 0;
    frac = 0;
    return;
  }
  if (t >= times.back()) {
    i = uint32_t(times.size()) - 2;
    frac = 1.0f;
    return;
  }
  auto it = std::upper_bound(times.begin(), times.end(), t);
  i = uint32_t(std::distance(times.begin(), it)) - 1;
  frac = (t - times[i]) / (times[i + 1] - times[i]);
}
} // namespace

void Animator::sampleClip(const AnimClipData& clip, float time,
                          std::vector<math::Vec3>& outT, std::vector<math::Quat>& outR,
                          std::vector<math::Vec3>& outS) {
  const float t = clip.duration > 0 ? std::fmod(time, clip.duration) : 0.0f;
  for (const auto& ch : clip.channels) {
    if (ch.path == 3) continue;  // morph weights → sampleWeights 处理
    uint32_t i = 0;
    float frac = 0;
    findSegment(ch.times, t, i, frac);
    const uint32_t comps = ch.path == 1 ? 4 : 3;
    const size_t o0 = size_t(i) * comps;
    const size_t o1 = size_t(i + 1) * comps;
    const float* v0 = &ch.values[o0];
    const float* v1 = o1 < ch.values.size() ? &ch.values[o1] : v0;  // 末段钳到末帧
    if (ch.path == 1) {
      math::Quat q0(v0[3], v0[0], v0[1], v0[2]);
      math::Quat q1(v1[3], v1[0], v1[1], v1[2]);
      outR[ch.node] = glm::slerp(q0, q1, frac);
    } else {
      const math::Vec3 a(v0[0], v0[1], v0[2]);
      const math::Vec3 b(v1[0], v1[1], v1[2]);
      const math::Vec3 r = glm::mix(a, b, frac);
      if (ch.path == 0) outT[ch.node] = r;
      else outS[ch.node] = r;
    }
  }
}

void Animator::sampleWeights(const AnimClipData& clip, float time, float blend) {
  if (morphNode_ < 0 || morphComps_ == 0) return;
  const float t = clip.duration > 0 ? std::fmod(time, clip.duration) : 0.0f;
  for (const auto& ch : clip.channels) {
    if (ch.path != 3 || ch.node != morphNode_) continue;
    uint32_t i = 0;
    float frac = 0;
    findSegment(ch.times, t, i, frac);
    const size_t o0 = size_t(i) * morphComps_;
    const size_t o1 = size_t(i + 1) * morphComps_;
    for (uint32_t c = 0; c < morphComps_; ++c) {
      const float v0 = o0 + c < ch.values.size() ? ch.values[o0 + c] : 0.0f;
      const float v1 = o1 + c < ch.values.size() ? ch.values[o1 + c] : v0;
      const float v = v0 + (v1 - v0) * frac;
      morphWeights_[c] = morphWeights_[c] + (v - morphWeights_[c]) * blend;
    }
  }
}

void Animator::update(float dt) {
  if (!model_ || !playing_ || paused_) return;
  active_.time += dt;
  sampleClip(model_->animations[size_t(active_.index)], active_.time, localT_, localR_,
             localS_);
  sampleWeights(model_->animations[size_t(active_.index)], active_.time, 1.0f);
  if (fadeIn_.index >= 0) {
    fadeIn_.time += dt;
    fadeElapsed_ += dt;
    const float w = std::min(fadeElapsed_ / fadeDuration_, 1.0f);
    // 淡入 clip 采样到临时数组,再按权重混合
    std::vector<math::Vec3> t2 = localT_;
    std::vector<math::Quat> r2 = localR_;
    std::vector<math::Vec3> s2 = localS_;
    sampleClip(model_->animations[size_t(fadeIn_.index)], fadeIn_.time, t2, r2, s2);
    // weights 淡入:淡入 clip 权重按 w 混合
    {
      std::vector<float> w0 = morphWeights_;
      sampleWeights(model_->animations[size_t(fadeIn_.index)], fadeIn_.time, 1.0f);
      std::vector<float> w1 = morphWeights_;
      for (size_t c = 0; c < w0.size(); ++c)
        morphWeights_[c] = w0[c] + (w1[c] - w0[c]) * w;
    }
    for (size_t i = 0; i < localT_.size(); ++i) {
      localT_[i] = glm::mix(localT_[i], t2[i], w);
      localR_[i] = glm::slerp(localR_[i], r2[i], w);
      localS_[i] = glm::mix(localS_[i], s2[i], w);
    }
    if (w >= 1.0f) {  // 淡入完成:切换
      active_ = fadeIn_;
      fadeIn_.index = -1;
    }
  }
  computeGlobals();
  computeJoints();
}

void Animator::computeGlobals() {
  const auto& nodes = model_->nodes;
  for (size_t i = 0; i < nodes.size(); ++i) {
    const math::Mat4 local =
        glm::translate(math::Mat4(1.0f), localT_[i]) * glm::mat4_cast(localR_[i]) *
        glm::scale(math::Mat4(1.0f), localS_[i]);
    if (nodes[i].parent >= 0)
      nodeGlobals_[i] = nodeGlobals_[size_t(nodes[i].parent)] * local;
    else
      nodeGlobals_[i] = local;
  }
}

void Animator::computeJoints() {
  jointMatrices_.clear();
  if (model_->skins.empty()) return;
  const auto& skin = model_->skins[0];
  jointMatrices_.resize(skin.joints.size());
  for (size_t j = 0; j < skin.joints.size(); ++j) {
    const auto& g = nodeGlobals_[size_t(skin.joints[j])];
    math::Mat4 ibm;
    std::memcpy(&ibm, &skin.inverseBindMatrices[j * 16], 64);
    jointMatrices_[j] = g * ibm;
  }
}

} // namespace rd::scene
