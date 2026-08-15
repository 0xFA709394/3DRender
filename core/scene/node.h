/**
 * @file node.h
 * @brief 场景节点:局部 TRS + 子节点树;world 矩阵经父链复合。
 * MeshNode 挂渲染资源(由 Scene::collect 遍历提交给 Renderer)。
 */
#pragma once
#include "foundation/math.h"
#include <memory>
#include <vector>

namespace rd {
class Renderer;
class MeshRenderResource;
} // namespace rd

namespace rd::scene {

class Node {
public:
  virtual ~Node() = default;

  /// 设置局部 TRS(平移/欧拉弧度 XYZ/缩放)。
  void setTRS(const math::Vec3& t, const math::Vec3& eulerRad, const math::Vec3& s);
  /// 局部矩阵:T × R(euler Z→Y→X) × S。
  math::Mat4 localMatrix() const;
  /// 世界矩阵:沿父链左乘到根。
  math::Mat4 worldMatrix() const;

  /// 挂载子节点(接管所有权);返回子节点引用便于继续挂接。
  Node& addChild(std::unique_ptr<Node> child);
  const std::vector<std::unique_ptr<Node>>& children() const { return children_; }

  /// 收集渲染项(基类空实现;MeshNode 重写)。由 Scene::collect 遍历调用。
  virtual void collect(Renderer& renderer) const {}

private:
  math::Vec3 t_{0.0f}, euler_{0.0f}, s_{1.0f};
  Node* parent_ = nullptr;  // 非拥有
  std::vector<std::unique_ptr<Node>> children_;
};

/// 挂网格渲染资源的节点(资源为 shared_ptr 持久持有,节点只引用)。
class MeshNode : public Node {
public:
  std::shared_ptr<MeshRenderResource> mesh;
  void collect(Renderer& renderer) const override;
};

} // namespace rd::scene
