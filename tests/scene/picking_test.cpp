// picking 单测:screenRay 方向、三角形求交命中/未中/最近胜出、蒙皮绑定姿态。
#include <gtest/gtest.h>
#include "common/skinned_gen.h"
#include "resource/gltf_loader.h"
#include "scene/picking.h"
#include <cmath>
#include <cstring>
#include <filesystem>

namespace {
// 双三角形模型:z=0 近三角 + z=-2 远三角;验证最近胜出
rd::ModelAsset makeTwoTri() {
  rd::ModelAsset m;
  auto addTri = [&](float z, float x0, float x1, const char* name) {
    rd::MeshData mesh;
    mesh.name = name;
    const float v[3][12] = {
        {x0, -0.5f, z, 0, 0, 1, 1, 0, 0, 1, 0, 0},
        {x1, -0.5f, z, 0, 0, 1, 1, 0, 0, 1, 1, 0},
        {(x0 + x1) * 0.5f, 0.5f, z, 0, 0, 1, 1, 0, 0, 1, 0.5f, 1},
    };
    mesh.vertices.assign(&v[0][0], &v[0][0] + 36);
    const uint16_t idx[3] = {0, 1, 2};
    mesh.indices.resize(6);
    memcpy(mesh.indices.data(), idx, 6);
    mesh.indexCount = 3;
    m.meshes.push_back(std::move(mesh));
  };
  addTri(-2.0f, -1.0f, 1.0f, "far");   // 先注册远的(验证不是"先中先得")
  addTri(0.0f, -1.0f, 1.0f, "near");
  m.boundingRadius = 2.5f;
  return m;
}
} // namespace

TEST(Picking, ScreenRayCenter) {
  rd::scene::Camera cam;
  cam.lookAt({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
  float o[3], d[3];
  rd::scene::screenRay(cam, 256, 256, 512, 512, o, d);
  // 中心:方向应指向 -Z(相机看向原点)
  EXPECT_NEAR(d[2], -1.0f, 1e-3f);
  EXPECT_NEAR(d[0], 0.0f, 1e-3f);
  EXPECT_NEAR(d[1], 0.0f, 1e-3f);
  EXPECT_NEAR(o[2], 2.9f, 1e-3f);  // 原点在近平面(z=3 - zNear 0.1)
  // 左上像素:dir.x<0 且 dir.y>0(屏幕 y 向下 → NDC y 向上)
  rd::scene::screenRay(cam, 0, 0, 512, 512, o, d);
  EXPECT_LT(d[0], 0.0f);
  EXPECT_GT(d[1], 0.0f);
}

TEST(Picking, HitMissNearest) {
  auto model = makeTwoTri();
  const float o[3] = {0, 0, 3}, d[3] = {0, 0, -1};
  auto r = rd::scene::pickModel(model, rd::math::Mat4(1.0f), o, d);
  ASSERT_TRUE(r.hit);
  EXPECT_EQ(r.meshIndex, 1) << "应命中近三角(near)";
  EXPECT_NEAR(r.distance, 3.0f, 1e-3f);
  EXPECT_NEAR(r.point[2], 0.0f, 1e-3f);
  // 未命中:射线偏离
  const float dm[3] = {0, 1, 0};
  const float om[3] = {0, 5, 3};
  auto r2 = rd::scene::pickModel(model, rd::math::Mat4(1.0f), om, dm);
  EXPECT_FALSE(r2.hit);
}

// 蒙皮模型按绑定姿态拾取(已知限制)
TEST(Picking, SkinnedBindPose) {
  const std::string dir =
      (std::filesystem::temp_directory_path() / "rd_pick_skin").string();
  auto model = rd::loadGltf(rd::test::writeSkinnedQuad(dir).c_str());
  ASSERT_TRUE(model.valid());
  // quad 在 z=0 平面(x∈[-0.5,0.5],y∈[0,2]);从 +Z 向 -Z 射
  const float o[3] = {0, 1, 3}, d[3] = {0, 0, -1};
  auto r = rd::scene::pickModel(model, rd::math::Mat4(1.0f), o, d);
  ASSERT_TRUE(r.hit);
  EXPECT_NEAR(r.point[1], 1.0f, 1e-3f);
}
