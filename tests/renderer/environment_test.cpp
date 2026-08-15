// 环境 CPU 生成测试:cubemap 尺寸、SH 常亮环境方向无关、BRDF LUT 端点、
// CPU 采样与生成同约定。
#include <gtest/gtest.h>
#include "renderer/environment.h"
#include <array>

TEST(Environment, CubemapDimensions) {
  auto env = rd::renderer::buildEnvCubemap(64);
  EXPECT_EQ(env.faces.size(), 6u);
  EXPECT_EQ(env.faces[0].size(), 64u * 64 * 4u);
  EXPECT_EQ(env.size, 64u);
}

TEST(Environment, SHOfConstantEnvIsDirectionIndependent) {
  std::vector<std::vector<uint8_t>> faces(6, std::vector<uint8_t>(16 * 16 * 4, 128));
  auto sh = rd::renderer::projectToSH(faces, 16);
  float e1 = rd::renderer::evalSH(sh, 1, 0, 0);
  float e2 = rd::renderer::evalSH(sh, 0, 1, 0);
  float e3 = rd::renderer::evalSH(sh, 0, 0, 1);
  EXPECT_NEAR(e1, e2, 0.05f);
  EXPECT_NEAR(e2, e3, 0.05f);
  EXPECT_GT(e1, 0.3f);   // 常亮 0.5 环境 × π 量级
  EXPECT_LT(e1, 2.0f);
}

TEST(Environment, BrdfLutEndpoints) {
  auto lut = rd::renderer::integrateBrdfLut(32);
  ASSERT_EQ(lut.size(), 32u * 32u * 2u);
  // roughness≈0, NdotV≈1:A≈1, B≈0(镜面无粗糙时 Fresnel 缩放≈1)
  float a = lut[(0 * 32 + 31) * 2];
  float b = lut[(0 * 32 + 31) * 2 + 1];
  EXPECT_NEAR(a, 1.0f, 0.05f);
  EXPECT_NEAR(b, 0.0f, 0.05f);
  // roughness≈1, NdotV≈1:A 明显小于 1(参考实现 ≈0.33)
  float aR = lut[(31 * 32 + 31) * 2];
  EXPECT_LT(aR, a);
  EXPECT_GT(aR, 0.2f);
  // roughness≈1, 掠射(NdotV≈0.016):B > 0(掠射 Fresnel 显著,参考实现 ≈0.018)
  float bG = lut[(31 * 32 + 0) * 2 + 1];
  EXPECT_GT(bG, 0.01f);
}

TEST(Environment, SampleEnvMatchesGeneration) {
  // CPU 采样与生成同约定:+X 面心的采样值应等于生成的中心像素
  auto env = rd::renderer::buildEnvCubemap(16);
  auto c = rd::renderer::sampleEnv(env, 1.0f, 0.0f, 0.0f);
  const uint8_t* center = &env.faces[0][(8 * 16 + 8) * 4];
  EXPECT_NEAR(c[0], center[0] / 255.0f, 0.02f);
  EXPECT_NEAR(c[1], center[1] / 255.0f, 0.02f);
}
