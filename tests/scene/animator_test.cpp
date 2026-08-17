// Animator 单测:clip 采样中点、交叉淡入权重、jointMatrices = global × IBM。
#include <gtest/gtest.h>
#include "common/skinned_gen.h"
#include "resource/gltf_loader.h"
#include "scene/animator.h"
#include <filesystem>
#include <glm/glm.hpp>

namespace {
rd::ModelAsset loadQuad() {
  const std::string dir =
      (std::filesystem::temp_directory_path() / "rd_anim_test").string();
  return rd::loadGltf(rd::test::writeSkinnedQuad(dir).c_str());
}
} // namespace

TEST(Animator, BindAndPlay) {
  auto model = loadQuad();
  ASSERT_TRUE(model.valid());
  rd::scene::Animator anim;
  ASSERT_TRUE(anim.bind(model));
  EXPECT_EQ(anim.clipCount(), 1u);
  EXPECT_FALSE(anim.playing());
  anim.play(0);
  EXPECT_TRUE(anim.playing());
}

// t=0.5 时 joint1 绕 Z 约 45°(slerp);joint0 恒为单位阵
TEST(Animator, SampleMidpoint) {
  auto model = loadQuad();
  rd::scene::Animator anim;
  ASSERT_TRUE(anim.bind(model));
  anim.play(0);
  anim.update(0.5f);
  const auto& joints = anim.jointMatrices();
  ASSERT_EQ(joints.size(), 2u);
  // joint0 = 单位(global identity × IBM identity)
  EXPECT_NEAR(joints[0][0][0], 1.0f, 1e-4f);
  EXPECT_NEAR(joints[0][3][3], 1.0f, 1e-4f);
  // joint1:global = T(0,1,0)·Rz(45°),IBM = T(0,-1,0)
  // jointMat 作用于关节原点 (0,1,0) 不动;点 (0,2,0) 绕 Z 弯折约 45°
  const auto& jm = joints[1];
  glm::vec4 p0 = jm * glm::vec4(0, 1, 0, 1);
  EXPECT_NEAR(p0.y, 1.0f, 1e-3f);
  glm::vec4 p1 = jm * glm::vec4(0, 2, 0, 1);
  EXPECT_LT(p1.x, -0.5f) << "弯折应向 -X 偏(rotZ 正向)";
  EXPECT_NEAR(p1.y, 1.0f + 0.7071f, 0.02f);
}

// 交叉淡入:fade 中途双 clip 按权重混合;fade 结束切到新 clip 的当前时间
TEST(Animator, Crossfade) {
  auto model = loadQuad();
  rd::scene::Animator anim;
  ASSERT_TRUE(anim.bind(model));
  anim.play(0);
  anim.update(0.5f);
  anim.playWithFade(0, 0.2f);  // 新 clip 从 0 淡入
  anim.update(0.1f);  // fade 中点:active@0.6(≈54°)与 fadeIn@0.1(≈9°)按 w=0.5 混合
  EXPECT_TRUE(anim.playing());
  const float mid = anim.jointMatrices()[1][0][1];
  EXPECT_GT(mid, std::sin(9.0f * 3.14159265f / 180.0f)) << "混合应超过新 clip 角度";
  EXPECT_LT(mid, std::sin(54.0f * 3.14159265f / 180.0f)) << "混合应小于旧 clip 角度";
  anim.update(0.2f);  // fade 完成(0.3≥0.2):新 clip time=0.3 → ≈27°
  const float done = anim.jointMatrices()[1][0][1];
  EXPECT_NEAR(done, std::sin(27.0f * 3.14159265f / 180.0f), 0.02f);
}

// 越界 clip:警告 no-op 不崩
TEST(Animator, OutOfRangeSafe) {
  auto model = loadQuad();
  rd::scene::Animator anim;
  ASSERT_TRUE(anim.bind(model));
  anim.play(9);
  anim.playWithFade(9, 0.1f);
  anim.update(0.1f);
  EXPECT_FALSE(anim.playing());
}
