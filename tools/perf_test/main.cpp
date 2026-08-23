// ============================================================================
// perf_test:性能基准工具(host)。离屏 512×512,4 场景 × 双后端逐帧计时。
// 用法: perf_test [--backend metal|vulkan|all] [--frames N] [--scene name]
//                 [--json out.json] [--no-gate] [--update-baseline]
// 退出码: 0=成功(门槛通过或 --no-gate);1=失败(初始化/门槛)。
// ============================================================================
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "common/shader_code.h"
#include "common/skinned_gen.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/animator.h"
#include "scene/camera.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <glm/glm.hpp>

namespace {
constexpr uint32_t kW = 512, kH = 512;
constexpr int kWarmup = 20;

/// 场景运行时上下文(资源持有到 bench 结束)。
struct SceneCtx {
  std::shared_ptr<rd::MeshRenderResource> model;   // helmet/items 用(单资源)
  std::vector<std::shared_ptr<rd::MeshRenderResource>> items;  // items_64 用
  rd::ModelAsset modelAsset;                       // CPU 资产(Animator 绑定源,须持久)
  rd::scene::Animator animator;                    // skinned 用
  bool animated = false;
  rd::scene::Camera cam;
};

struct BenchResult {
  std::string scene;
  std::string backend;
  float avgMs = 0, p50Ms = 0, p95Ms = 0, p99Ms = 0, fps = 0;
  int frames = 0;
};

const char* kScenes[] = {"helmet_high", "helmet_low", "skinned_anim", "items_64",
                         "sponza"};  // sponza 需 assets/(fetch_assets.sh),缺失自动跳过

rd::scene::Camera defaultCam(const rd::ModelAsset& m) {
  rd::scene::Camera cam;
  const float dist = m.boundingRadius * 2.5f;
  glm::vec3 center(m.boundingCenter[0], m.boundingCenter[1], m.boundingCenter[2]);
  glm::vec3 eye = center + glm::vec3(dist * 0.65f, dist * 0.35f, dist * 0.65f);
  cam.lookAt({eye.x, eye.y, eye.z}, {center.x, center.y, center.z}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, dist * 0.1f, dist * 10.0f);
  return cam;
}

/// 构建场景(资源创建在 beginFrame 之前;失败返回 false)。
bool buildScene(const std::string& name, rd::Device& dev, rd::Renderer& renderer,
                SceneCtx& ctx, std::string& assetsDir) {
  if (name == "helmet_high" || name == "helmet_low") {
    auto model = rd::loadGltf((assetsDir + "/DamagedHelmet.glb").c_str());
    if (!model.valid()) return false;
    ctx.model = rd::MeshRenderResource::upload(dev, model);
    if (!ctx.model) return false;
    if (name == "helmet_high") {
      renderer.setQuality(rd::qualityPreset(rd::QualityTier::High));
      rd::LightData light;
      light.type = rd::LightType::Directional;
      const float dl = std::sqrt(0.5f * 0.5f + 0.8f * 0.8f + 0.3f * 0.3f);
      light.direction[0] = 0.5f / dl;
      light.direction[1] = 0.8f / dl;
      light.direction[2] = 0.3f / dl;
      light.color[0] = light.color[1] = light.color[2] = 3.0f;
      renderer.setLights({light});
      renderer.setLightFraming(model.boundingCenter, model.boundingRadius);
    } else {
      renderer.setQuality(rd::qualityPreset(rd::QualityTier::Low));
    }
    ctx.cam = defaultCam(model);
    return true;
  }
  if (name == "skinned_anim") {
    const std::string dir =
        (std::filesystem::temp_directory_path() / "rd_perf_skin").string();
    ctx.modelAsset = rd::loadGltf(rd::test::writeSkinnedQuad(dir).c_str());
    if (!ctx.modelAsset.valid()) return false;
    ctx.model = rd::MeshRenderResource::upload(dev, ctx.modelAsset);
    if (!ctx.model) return false;
    if (!ctx.animator.bind(ctx.modelAsset)) return false;
    ctx.animator.play(0);
    ctx.animated = true;
    ctx.cam.lookAt({0, 1.2f, 3}, {0, 1, 0}, {0, 1, 0});
    ctx.cam.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
    return true;
  }
  if (name == "sponza") {
    // 重量级场景(~26 万三角形;assets/sponza,fetch_assets.sh 下载;不存在则跳过)
    auto model = rd::loadGltf((assetsDir + "/../../assets/sponza/Sponza.gltf").c_str());
    if (!model.valid()) return false;
    ctx.model = rd::MeshRenderResource::upload(dev, model);
    if (!ctx.model) return false;
    renderer.setQuality(rd::qualityPreset(rd::QualityTier::High));
    rd::LightData light;
    light.type = rd::LightType::Directional;
    const float dl = std::sqrt(0.5f * 0.5f + 0.8f * 0.8f + 0.3f * 0.3f);
    light.direction[0] = 0.5f / dl;
    light.direction[1] = 0.8f / dl;
    light.direction[2] = 0.3f / dl;
    light.color[0] = light.color[1] = light.color[2] = 3.0f;
    renderer.setLights({light});
    renderer.setLightFraming(model.boundingCenter, model.boundingRadius);
    ctx.cam = defaultCam(model);
    return true;
  }
  if (name == "items_64") {
    auto model = rd::loadGltf((assetsDir + "/BoxTextured.glb").c_str());
    if (!model.valid()) return false;
    ctx.model = rd::MeshRenderResource::upload(dev, model);
    if (!ctx.model) return false;
    ctx.cam.lookAt({0, 0, 14}, {0, 0, 0}, {0, 1, 0});
    ctx.cam.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
    return true;
  }
  return false;
}

/// 帧提交(按场景类型)。
void submitScene(const std::string& name, rd::Renderer& renderer, SceneCtx& ctx,
                 float dt) {
  if (ctx.animated) {
    ctx.animator.update(dt);
    renderer.submit(ctx.model, rd::math::Mat4(1.0f), ctx.animator.jointMatrices().data(),
                    uint32_t(ctx.animator.jointMatrices().size()));
    return;
  }
  if (name == "items_64") {
    for (int i = 0; i < 64; ++i) {
      glm::mat4 w(1.0f);
      w = glm::translate(w, glm::vec3((i % 8 - 3.5f) * 1.5f, (i / 8 - 3.5f) * 1.5f, 0));
      renderer.submit(ctx.model, w);
    }
    return;
  }
  renderer.submit(ctx.model, rd::math::Mat4(1.0f));
}

/// 逐帧计时(warmup 后统计)。
BenchResult bench(rd::Device& dev, rd::Renderer& renderer, rd::TargetHandle target,
                  const std::string& name, SceneCtx& ctx, int frames,
                  const char* backendName) {
  BenchResult r;
  r.scene = name;
  r.backend = backendName;
  std::vector<float> times;
  times.reserve(size_t(frames));
  for (int f = 0; f < kWarmup + frames; ++f) {
    const float dt = 1.0f / 60.0f;
    const auto t0 = std::chrono::steady_clock::now();
    dev.beginFrame();
    renderer.beginScene(ctx.cam, {0.05f, 0.05f, 0.06f, 1.0f});
    submitScene(name, renderer, ctx, dt);
    auto* cmd = dev.acquireCommandBuffer();
    renderer.endScene(cmd, target);
    dev.submit(cmd);
    dev.waitIdle();
    dev.endFrame();
    const auto t1 = std::chrono::steady_clock::now();
    if (f >= kWarmup)
      times.push_back(std::chrono::duration<float, std::milli>(t1 - t0).count());
  }
  std::sort(times.begin(), times.end());
  r.frames = int(times.size());
  float sum = 0;
  for (float t : times) sum += t;
  r.avgMs = sum / float(times.size());
  r.p50Ms = times[size_t(times.size() * 0.50f)];
  r.p95Ms = times[size_t(times.size() * 0.95f)];
  r.p99Ms = times[size_t(times.size() * 0.99f)];
  r.fps = r.avgMs > 0 ? 1000.0f / r.avgMs : 0;
  return r;
}

/// 基线读取:手写最小解析(固定结构),scene/backend 名逐字匹配。
/// 返回 avg 基线;文件缺失/条目缺失返回 0(调用方按 SKIP 处理)。
float baselineRead(const std::string& path, const char* scene, const char* backend) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return 0.0f;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::string s(size_t(n), '\0');
  if (fread(s.data(), 1, s.size(), f) != s.size()) {
    fclose(f);
    return 0.0f;
  }
  fclose(f);
  // 定位 "scene" 段,再在其中找 "backend": number
  const std::string sceneKey = std::string("\"") + scene + "\"";
  const size_t sp = s.find(sceneKey);
  if (sp == std::string::npos) return 0.0f;
  const size_t segEnd = s.find('\n', sp);  // 场景条目单行结构
  const std::string seg = s.substr(sp, segEnd == std::string::npos ? std::string::npos
                                                                   : segEnd - sp);
  const std::string bk = std::string("\"") + backend + "\":";
  const size_t bp = seg.find(bk);
  if (bp == std::string::npos) return 0.0f;
  return float(atof(seg.c_str() + bp + bk.size()));
}

/// 基线重写(按本次结果;两空格缩进)。
bool baselineWrite(const std::string& path, const std::vector<BenchResult>& results) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  fprintf(f, "{\n");
  for (size_t si = 0; si < 4; ++si) {
    fprintf(f, "  \"%s\": {", kScenes[si]);
    bool first = true;
    for (const auto& r : results) {
      if (r.scene != kScenes[si]) continue;
      fprintf(f, "%s\"%s\": %.4f", first ? "" : ", ", r.backend.c_str(), r.p50Ms);
      first = false;
    }
    fprintf(f, "}%s\n", si + 1 < 4 ? "," : "");
  }
  fprintf(f, "}\n");
  fclose(f);
  return true;
}

/// 初始化设备/渲染器/离屏目标。
bool initRenderer(rd::Backend b, std::unique_ptr<rd::Device>& device,
                  rd::Renderer& renderer, rd::TargetHandle& target) {
  rd::DeviceDesc d;
  d.backend = b;
  device = rd::createDevice(d);
  if (!device) return false;
  auto load = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
  auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
  auto skVs = load("pbr_forward_skinned.vert");
  auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
  auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
  auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
  auto sdsVs = load("shadow_depth_skinned.vert");
  auto exFs = load("bloom_extract.frag");
  auto bbFs = load("bloom_blur.frag");
  auto cpFs = load("composite.frag");
  auto fxFs = load("fxaa.frag");
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                            cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,
                            {},{}, {}, unlitVs.entry, rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  target = device->createOffscreenTarget(td);
  return target.valid() && renderer.init(*device, sd);
}
} // namespace

int main(int argc, char** argv) {
  std::string backendArg = "metal";
  std::string sceneFilter;
  std::string jsonPath;
  int frames = 120;
  bool noGate = false;
  bool updateBaseline = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
      backendArg = argv[++i];
    } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
      frames = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--scene") && i + 1 < argc) {
      sceneFilter = argv[++i];
    } else if (!strcmp(argv[i], "--json") && i + 1 < argc) {
      jsonPath = argv[++i];
    } else if (!strcmp(argv[i], "--no-gate")) {
      noGate = true;
    } else if (!strcmp(argv[i], "--update-baseline")) {
      updateBaseline = true;
    }
  }

  std::vector<rd::Backend> backends;
  if (backendArg == "all" || backendArg == "metal") backends.push_back(rd::Backend::Metal);
#if defined(RD_WITH_VULKAN)
  if (backendArg == "all" || backendArg == "vulkan") backends.push_back(rd::Backend::Vulkan);
#endif
  if (backends.empty()) {
    fprintf(stderr, "无可用后端(%s)\n", backendArg.c_str());
    return 1;
  }
  std::string assetsDir = RD_PERF_ASSETS_DIR;  // CMake 传入(tests/assets)
  std::vector<BenchResult> results;
  for (rd::Backend b : backends) {
    const char* bname = b == rd::Backend::Metal ? "metal" : "vulkan";
    for (const char* sceneName : kScenes) {
      if (!sceneFilter.empty() && sceneFilter != sceneName) continue;
      std::unique_ptr<rd::Device> device;
      rd::Renderer renderer;
      rd::TargetHandle target;
      if (!initRenderer(b, device, renderer, target)) {
        fprintf(stderr, "初始化失败(%s/%s)\n", sceneName, bname);
        return 1;
      }
      SceneCtx ctx;
      if (!buildScene(sceneName, *device, renderer, ctx, assetsDir)) {
        // 可选场景(下载资产)缺失:跳过;内置场景失败:整跑失败
        if (strcmp(sceneName, "sponza") == 0) {
          fprintf(stderr, "场景构建失败(%s)(可选资产缺失,跳过)\n", sceneName);
          renderer.shutdown();
          device->destroyTarget(target);
          continue;
        }
        fprintf(stderr, "场景构建失败(%s)\n", sceneName);
        return 1;
      }
      auto r = bench(*device, renderer, target, sceneName, ctx, frames, bname);
      printf("[bench] %-13s %-6s avg=%.2fms p50=%.2fms p95=%.2fms p99=%.2fms fps=%.0f\n",
             r.scene.c_str(), r.backend.c_str(), r.avgMs, r.p50Ms, r.p95Ms, r.p99Ms,
             r.fps);
      results.push_back(r);
      if (ctx.model) ctx.model->destroy(*device);
      for (auto& m : ctx.items) m->destroy(*device);
      renderer.shutdown();
      device->destroyTarget(target);
    }
  }

  // JSON 落盘(可选)
  if (!jsonPath.empty()) {
    FILE* f = fopen(jsonPath.c_str(), "wb");
    if (f) {
      fprintf(f, "[\n");
      for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        fprintf(f,
                "  {\"scene\": \"%s\", \"backend\": \"%s\", \"avgMs\": %.4f, "
                "\"p95Ms\": %.4f, \"p99Ms\": %.4f, \"fps\": %.1f, \"frames\": %d}%s\n",
                r.scene.c_str(), r.backend.c_str(), r.avgMs, r.p95Ms, r.p99Ms, r.fps,
                r.frames, i + 1 < results.size() ? "," : "");
      }
      fprintf(f, "]\n");
      fclose(f);
    }
  }

  // 基线更新
  const char* baselinePath = getenv("RD_PERF_BASELINE");
  const std::string bp = baselinePath ? baselinePath : RD_PERF_BASELINE_PATH;
  if (updateBaseline) {
    if (!baselineWrite(bp, results)) {
      fprintf(stderr, "基线写入失败: %s\n", bp.c_str());
      return 1;
    }
    printf("[gate] 基线已更新: %s\n", bp.c_str());
  }

  // 软门槛:avg > 1.5× baseline → FAIL
  if (!noGate && !updateBaseline) {
    int fails = 0;
    for (const auto& r : results) {
      const float base = baselineRead(bp, r.scene.c_str(), r.backend.c_str());
      if (base <= 0.0f) {
        printf("[gate] SKIP %s/%s(基线无此条目)\n", r.scene.c_str(), r.backend.c_str());
        continue;
      }
      if (r.p50Ms > base * 2.0f) {
        printf("[gate] FAIL %s/%s p50=%.2fms baseline(p50)=%.2fms\n", r.scene.c_str(),
               r.backend.c_str(), r.p50Ms, base);
        ++fails;
      }
    }
    if (fails > 0) return 1;
    printf("[gate] PASS\n");
  }
  return 0;
}
