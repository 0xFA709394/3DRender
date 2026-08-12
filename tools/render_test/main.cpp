// ============================================================================
// render_test：手动渲染验证工具（host）。
// 离屏渲染旋转立方体并保存 PNG，用于本地快速验证后端渲染正确性。
//
// 用法: render_test --backend metal|vulkan [--angle 45] [--out cube.png]
// 退出码: 0=成功；1=失败（后端不可用/初始化/readback/保存）。
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "common/image.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "scene/cube_scene.h"
#include "rd_shader_dir.h"

int main(int argc, char** argv) {
  // ---- 参数解析（默认 metal、45°、输出 cube.png）----
  rd::Backend backend = rd::Backend::Metal;
  float angleDeg = 45.0f;
  std::string out = "cube.png";
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
      backend = !strcmp(argv[++i], "vulkan") ? rd::Backend::Vulkan : rd::Backend::Metal;
    } else if (!strcmp(argv[i], "--angle") && i + 1 < argc) {
      angleDeg = float(atof(argv[++i]));
    } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
      out = argv[++i];
    }
  }

  // ---- 设备与离屏目标 ----
  rd::DeviceDesc desc;
  desc.backend = backend;
  auto device = rd::createDevice(desc);
  if (!device) {
    fprintf(stderr, "后端不可用\n");
    return 1;
  }
  constexpr uint32_t kW = 512, kH = 512;
  auto target = device->createOffscreenTarget({kW, kH});
  // shader 按后端从 RD_SHADER_DIR 加载（SPIR-V/metallib，入口名一并返回）
  rd::demo::CubeScene cube;
  auto code = rd::test::loadCubeShaderCode(backend, RD_SHADER_DIR);
  if (!target.valid() || !cube.init(*device, code.vs.data(), code.vs.size(), code.fs.data(),
                                    code.fs.size(), code.entry.c_str())) {
    fprintf(stderr, "初始化失败\n");
    return 1;
  }
  // ---- 渲染 → readback → 保存 PNG ----
  cube.render(*device, target, kW, kH, angleDeg * 0.0174532925f);  // 角度制 → 弧度
  std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4);
  if (!device->readbackTarget(target, pixels.data(), pixels.size())) {
    fprintf(stderr, "readback 失败\n");
    return 1;
  }
  cube.shutdown(*device);
  device->destroyTarget(target);
  if (!rd::test::savePNG(out, kW, kH, pixels.data())) {
    fprintf(stderr, "保存失败\n");
    return 1;
  }
  printf("已保存 %s (%ux%u)\n", out.c_str(), kW, kH);
  return 0;
}
