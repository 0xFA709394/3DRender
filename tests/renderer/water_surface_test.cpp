// WaterSurface 创建/注入/步进冒烟(不渲染最终图像——语义判据在 Renderer 集成后)。
#include <gtest/gtest.h>
#include "common/shader_code.h"
#include "renderer/water.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

static rd::WaterSurface* makeWater(rd::Device& dev) {
  auto blitVs = rd::test::loadShaderCode(dev.backend(), RD_SHADER_DIR, "blit.vert");
  auto stepFs = rd::test::loadShaderCode(dev.backend(), RD_SHADER_DIR, "water_step.frag");
  auto cauFs = rd::test::loadShaderCode(dev.backend(), RD_SHADER_DIR, "water_caustics.frag");
  auto* w = new rd::WaterSurface();
  rd::WaterDesc d;
  d.simSize = 64;
  if (!w->create(dev, d, blitVs.code, stepFs.code, cauFs.code, blitVs.entry)) {
    delete w;
    return nullptr;
  }
  return w;
}

TEST(WaterSurface, CreateStepSmoke) {
#if defined(__APPLE__)
  rd::DeviceDesc dd;
  dd.backend = rd::Backend::Metal;
  auto dev = rd::createDevice(dd);
  ASSERT_TRUE(dev);
  auto* w = makeWater(*dev);
  ASSERT_NE(w, nullptr);
  EXPECT_TRUE(w->waveTex().valid());
  EXPECT_TRUE(w->causticsTex().valid());
  w->disturb(0.5f, 0.5f, 0.05f, 3.0f);
  w->tick(1.0f / 60.0f);
  dev->beginFrame();
  auto* cmd = dev->acquireCommandBuffer();
  w->step(cmd);
  dev->submit(cmd);
  dev->waitIdle();
  dev->endFrame();
  w->destroy(*dev);
  delete w;
#endif
}
