// scene 层单测:Node TRS/局部矩阵、父子 world 复合、Camera view/proj。
#include <gtest/gtest.h>
#include "scene/node.h"
#include "scene/camera.h"
#include "foundation/math.h"
#include <glm/glm.hpp>

namespace {
bool near4(const rd::math::Mat4& a, const rd::math::Mat4& b, float eps = 1e-5f) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      if (std::abs(a[c][r] - b[c][r]) > eps) return false;
  return true;
}
} // namespace

TEST(SceneNode, LocalMatrixTRS) {
  rd::scene::Node n;
  n.setTRS({1, 2, 3}, {0, 0, 0}, {2, 2, 2});
  auto expect = glm::scale(glm::translate(rd::math::Mat4(1.0f), glm::vec3(1, 2, 3)),
                           glm::vec3(2, 2, 2));
  EXPECT_TRUE(near4(n.localMatrix(), expect));
}

TEST(SceneNode, WorldMatrixParentChain) {
  rd::scene::Node parent;
  parent.setTRS({10, 0, 0}, {0, 0, 0}, {1, 1, 1});
  auto child = std::make_unique<rd::scene::Node>();
  child->setTRS({1, 0, 0}, {0, 0, 0}, {1, 1, 1});
  auto* childPtr = child.get();
  parent.addChild(std::move(child));
  // world = parent.local × child.local → 平移 (11,0,0)
  auto w = childPtr->worldMatrix();
  EXPECT_NEAR(w[3][0], 11.0f, 1e-5f);
}

TEST(SceneCamera, ViewProj) {
  rd::scene::Camera cam;
  cam.lookAt({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
  cam.setPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
  auto v = cam.viewMatrix();
  EXPECT_NEAR(v[3][2], -3.0f, 1e-5f);  // 视图平移 -3(z)
  auto p = cam.projMatrix();
  // GLM_FORCE_DEPTH_ZERO_TO_ONE:zNear 点映射到 NDC z=0
  glm::vec4 nearPt = p * glm::vec4(0, 0, -0.1f, 1);
  EXPECT_NEAR(nearPt.z / nearPt.w, 0.0f, 1e-4f);
}
