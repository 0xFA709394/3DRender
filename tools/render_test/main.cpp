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
#include <filesystem>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "common/image.h"
#include "common/shader_code.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "rhi/rhi_device.h"
#include "scene/cube_scene.h"
#include "tools/render_test/scenes.h"
#include "rd_shader_dir.h"
#if defined(__APPLE__)
#include "tools/render_test/interactive.h"
#endif


namespace {
/// pipeline 缓存目录接线(与 engine 同约定:<dir>/pipelines/<backend>.bin)。
void applyPipelineCache(rd::Device& device, rd::Backend backend, const std::string& dir) {
  if (dir.empty()) return;
  const std::string pdir = dir + "/pipelines";
  std::filesystem::create_directories(pdir);
  const char* bn = backend == rd::Backend::Vulkan ? "vulkan" : "metal";
  device.setPipelineCachePath((pdir + "/" + bn + ".bin").c_str());
}
} // namespace

int main(int argc, char** argv) {
  // ---- 参数解析（默认 metal、45°、输出 cube.png）----
  rd::Backend backend = rd::Backend::Metal;
  float angleDeg = 45.0f;
  std::string out = "cube.png";
  std::string model;
  bool pbr = false;  // --pbr:包围球取景(任意模型自动取景);默认固定机位 (0,0,3)
  bool interactive = false;  // --interactive:GLFW 窗口交互(Metal,macOS)
  std::string sceneName;     // --scene:demo 场景(与 --model 互斥,scene 优先)
  std::string recordPath;    // --record:指针事件录制输出
  std::string playPath;      // --play:按日志回放
  std::string cacheDir;      // --cache-dir:IBL 预滤波磁盘缓存目录
  std::string scriptPath;    // --script:命令脚本(交互模式启动后执行)
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
      backend = !strcmp(argv[++i], "vulkan") ? rd::Backend::Vulkan : rd::Backend::Metal;
    } else if (!strcmp(argv[i], "--angle") && i + 1 < argc) {
      angleDeg = float(atof(argv[++i]));
    } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
      out = argv[++i];
    } else if (!strcmp(argv[i], "--model") && i + 1 < argc) {
      model = argv[++i];
    } else if (!strcmp(argv[i], "--pbr")) {
      pbr = true;
    } else if (!strcmp(argv[i], "--scene") && i + 1 < argc) {
      sceneName = argv[++i];
    } else if (!strcmp(argv[i], "--interactive")) {
      interactive = true;
    } else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
      recordPath = argv[++i];
    } else if (!strcmp(argv[i], "--play") && i + 1 < argc) {
      playPath = argv[++i];
    } else if (!strcmp(argv[i], "--cache-dir") && i + 1 < argc) {
      cacheDir = argv[++i];
    } else if (!strcmp(argv[i], "--script") && i + 1 < argc) {
      scriptPath = argv[++i];
    }
  }

#if defined(__APPLE__)
  if (interactive)
    return rd::tool::runInteractive(model.empty() ? nullptr : model.c_str(),
                                    sceneName.empty() ? nullptr : sceneName.c_str(),
                                    recordPath.empty() ? nullptr : recordPath.c_str(),
                                    playPath.empty() ? nullptr : playPath.c_str(),
                                    scriptPath.empty() ? nullptr : scriptPath.c_str());
#else
  (void)interactive;
#endif

  // ---- demo 场景模式(scenes 模块驱动)----
  if (!sceneName.empty()) {
    rd::DeviceDesc desc;
    desc.backend = backend;
    auto device = rd::createDevice(desc);
    if (!device) {
      fprintf(stderr, "后端不可用\n");
      return 1;
    }
    applyPipelineCache(*device, backend, cacheDir);
    constexpr uint32_t kW = 512, kH = 512;
    auto load = [&](const char* name) {
      return rd::test::loadShaderCode(backend, RD_SHADER_DIR, name);
    };
    auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
    auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
    auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
    auto eqFs = load("equirect_to_cube.frag");
    auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
    auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
    auto sdsVs = load("shadow_depth_skinned.vert");
    auto exFs = load("bloom_extract.frag");
    auto bbFs = load("bloom_blur.frag");
    auto cpFs = load("composite.frag");
    auto fxFs = load("fxaa.frag");
    auto skVs = load("pbr_forward_skinned.vert");
    rd::Renderer renderer;
    rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                              pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                              sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                              cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,
                              eqFs.code,   unlitVs.entry, rd::Format::RGBA8_UNORM};
    rd::OffscreenTargetDesc td;
    td.width = kW;
    td.height = kH;
    td.depth = true;
    auto target = device->createOffscreenTarget(td);
    if (!target.valid()) {
      fprintf(stderr, "初始化失败\n");
      return 1;
    }
    if (!cacheDir.empty()) renderer.setCacheDir(cacheDir.c_str());  // init 前(env 在 init 建)
    if (!renderer.init(*device, sd)) {
      fprintf(stderr, "初始化失败\n");
      return 1;
    }
    rd::ModelAsset storage;
    rd::tool::DemoScene scene;
    if (!rd::tool::buildDemoScene(sceneName.c_str(), *device, renderer, scene, storage)) {
      fprintf(stderr, "场景构建失败(资产缺失或名未知): %s\n", sceneName.c_str());
      return 1;
    }
    device->beginFrame();
    renderer.beginScene(scene.camera, {0.05f, 0.05f, 0.06f, 1.0f});
    rd::tool::submitDemoScene(scene, renderer, 1.0f / 60.0f);
    auto* cmd = device->acquireCommandBuffer();
    renderer.endScene(cmd, target);
    device->submit(cmd);
    device->waitIdle();
    device->endFrame();
    std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4);
    if (!device->readbackTarget(target, pixels.data(), pixels.size())) {
      fprintf(stderr, "readback 失败\n");
      return 1;
    }
    rd::tool::destroyDemoScene(scene, *device);
    renderer.shutdown();
    device->destroyTarget(target);
    if (!rd::test::savePNG(out, kW, kH, pixels.data())) {
      fprintf(stderr, "保存失败\n");
      return 1;
    }
    printf("已保存 %s (%ux%u,场景 %s)\n", out.c_str(), kW, kH, sceneName.c_str());
    return 0;
  }


  // ---- 设备与离屏目标 ----
  rd::DeviceDesc desc;
  desc.backend = backend;
  auto device = rd::createDevice(desc);
  if (!device) {
    fprintf(stderr, "后端不可用\n");
    return 1;
  }
  applyPipelineCache(*device, backend, cacheDir);
  constexpr uint32_t kW = 512, kH = 512;

  // ---- glTF 模型模式(renderer/scene/resource 新骨架驱动)----
  if (!model.empty()) {
    auto load = [&](const char* name) {
      return rd::test::loadShaderCode(backend, RD_SHADER_DIR, name);
    };
    auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
    auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
    auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
    auto eqFs = load("equirect_to_cube.frag");
    auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
  auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
  auto exFs = load("bloom_extract.frag");
  auto bbFs = load("bloom_blur.frag");
  auto cpFs = load("composite.frag");
  auto fxFs = load("fxaa.frag");
  auto skVs = load("pbr_forward_skinned.vert");
  auto sdsVs = load("shadow_depth_skinned.vert");
    rd::Renderer renderer;
    rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                              pfVs.code,   pfFs.code,   blitVs.code, blitFs.code, sdVs.code, sdFs.code,
                            exFs.code,   bbFs.code,   cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,   eqFs.code,
                              unlitVs.entry, rd::Format::RGBA8_UNORM};
    rd::OffscreenTargetDesc td;
    td.width = kW;
    td.height = kH;
    td.depth = true;
    auto target = device->createOffscreenTarget(td);
    auto m = rd::loadGltf(model.c_str());
    if (!target.valid() || !m.valid()) {
      fprintf(stderr, "初始化失败\n");
      return 1;
    }
    if (!cacheDir.empty()) renderer.setCacheDir(cacheDir.c_str());  // init 前
    if (!renderer.init(*device, sd)) {
      fprintf(stderr, "初始化失败\n");
      return 1;
    }
    auto res = rd::MeshRenderResource::upload(*device, m);
    if (!res) return 1;
    rd::scene::Scene scene;
    auto node = std::make_unique<rd::scene::MeshNode>();
    node->mesh = res;
    scene.root().addChild(std::move(node));
    rd::scene::Camera cam;
    if (pbr) {
      // 包围球取景:任意模型自动框取(45° 方位角、20° 仰角)
      const float dist = m.boundingRadius * 2.5f;
      glm::vec3 center(m.boundingCenter[0], m.boundingCenter[1], m.boundingCenter[2]);
      glm::vec3 eye = center + glm::vec3(dist * 0.65f, dist * 0.35f, dist * 0.65f);
      cam.lookAt({eye.x, eye.y, eye.z}, {center.x, center.y, center.z}, {0, 1, 0});
      cam.setPerspective(0.78539816f, 1.0f, dist * 0.1f, dist * 10.0f);
    } else {
      cam.lookAt({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
      cam.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
    }
    device->beginFrame();
    renderer.beginScene(cam, {0.2f, 0.2f, 0.25f, 1.0f});
    scene.collect(renderer);
    auto* cmd = device->acquireCommandBuffer();
    renderer.endScene(cmd, target);
    device->submit(cmd);
    device->waitIdle();
    device->endFrame();
    std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4);
    if (!device->readbackTarget(target, pixels.data(), pixels.size())) {
      fprintf(stderr, "readback 失败\n");
      return 1;
    }
    res->destroy(*device);
    renderer.shutdown();
    if (!rd::test::savePNG(out, kW, kH, pixels.data())) {
      fprintf(stderr, "保存失败\n");
      return 1;
    }
    printf("已保存 %s (%ux%u)\n", out.c_str(), kW, kH);
    return 0;
  }

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
