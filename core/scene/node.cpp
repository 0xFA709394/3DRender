// Node/MeshNode 的实现:TRS 局部矩阵、父链 world 复合、渲染项收集。
#include "scene/node.h"
#include "renderer/renderer.h"
#include "resource/mesh_render_resource.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace rd::scene {

void Node::setTRS(const math::Vec3& t, const math::Vec3& eulerRad, const math::Vec3& s) {
  t_ = t;
  euler_ = eulerRad;
  s_ = s;
}

math::Mat4 Node::localMatrix() const {
  auto m = glm::translate(math::Mat4(1.0f), t_);
  m = glm::rotate(m, euler_.z, math::Vec3(0, 0, 1));
  m = glm::rotate(m, euler_.y, math::Vec3(0, 1, 0));
  m = glm::rotate(m, euler_.x, math::Vec3(1, 0, 0));
  return glm::scale(m, s_);
}

math::Mat4 Node::worldMatrix() const {
  math::Mat4 m = localMatrix();
  for (Node* p = parent_; p; p = p->parent_) m = p->localMatrix() * m;
  return m;
}

Node& Node::addChild(std::unique_ptr<Node> child) {
  child->parent_ = this;
  children_.push_back(std::move(child));
  return *children_.back();
}

void MeshNode::collect(Renderer& renderer) const {
  if (mesh) renderer.submit(mesh, worldMatrix());
}

} // namespace rd::scene
