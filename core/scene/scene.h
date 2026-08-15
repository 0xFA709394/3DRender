/**
 * @file scene.h
 * @brief 场景:root Node + 深度优先遍历收集渲染项投给 Renderer。
 */
#pragma once
#include "scene/node.h"

namespace rd {
class Renderer;
}

namespace rd::scene {

class Scene {
public:
  /// 根节点(挂载/编辑场景结构的入口)。
  Node& root() { return root_; }
  /// 深度优先遍历,对每个节点调用 collect(renderer)。
  void collect(Renderer& renderer);

private:
  Node root_;
};

} // namespace rd::scene
