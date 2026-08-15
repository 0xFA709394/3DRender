// Scene 的实现:深度优先遍历收集渲染项。
#include "scene/scene.h"
#include "renderer/renderer.h"

namespace rd::scene {
namespace {
void collectRecursive(const Node& n, Renderer& renderer) {
  n.collect(renderer);
  for (const auto& c : n.children()) collectRecursive(*c, renderer);
}
} // namespace

void Scene::collect(Renderer& renderer) { collectRecursive(root_, renderer); }

} // namespace rd::scene
