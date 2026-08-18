// demo 场景的实现:程序场景经 primitives 组装;知名 glb 从 assets/ 加载。
#include "tools/render_test/scenes.h"
#include "common/skinned_gen.h"
#include "foundation/log.h"
#include "resource/primitives.h"
#include <cmath>
#include <filesystem>
#include <glm/glm.hpp>

namespace rd::tool {
namespace {

const char* const kNames[] = {"material_balls", "cornell_box", "light_playground",
                              "skinned_demo",   "instanced_field",
                              "sponza",         "cesium_man"};

/// 单 mesh ModelAsset 包装(材质参数由调用方设)。
ModelAsset wrapMesh(MeshData&& mesh) {
  ModelAsset m;
  m.meshes.push_back(std::move(mesh));
  m.boundingRadius = 1.0f;
  return m;
}

bool uploadInto(Device& dev, ModelAsset&& model, DemoScene& out,
                const math::Mat4& world) {
  auto res = MeshRenderResource::upload(dev, model);
  if (!res) return false;
  out.resources.push_back(res);
  out.worlds.push_back(world);
  return true;
}

void buildMaterialBalls(Device& dev, DemoScene& out) {
  // 5×5 球阵:metallic=i/4,roughness=j/4;间距 1.0 居中于原点
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 5; ++j) {
      auto mesh = primitives::makeSphere(0.4f, 32, 16);
      mesh.material.metallicFactor = float(i) / 4.0f;
      mesh.material.roughnessFactor = 0.05f + float(j) / 4.0f * 0.95f;
      math::Mat4 w = glm::translate(math::Mat4(1.0f),
                                    math::Vec3((i - 2) * 1.0f, (j - 2) * 1.0f, 0));
      uploadInto(dev, wrapMesh(std::move(mesh)), out, w);
    }
  out.camera.lookAt({0, 0, 7.5f}, {0, 0, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
  out.framingRadius = 3.0f;
}

void buildCornellBox(Device& dev, DemoScene& out) {
  // 五面墙(薄板盒)+ 两内盒;左红右绿后白顶白底白
  auto wall = [&](float r, float g, float b) {
    auto mesh = primitives::makeBox(1, 1, 1);
    mesh.material.baseColorFactor[0] = r;
    mesh.material.baseColorFactor[1] = g;
    mesh.material.baseColorFactor[2] = b;
    mesh.material.roughnessFactor = 0.9f;
    mesh.material.metallicFactor = 0.0f;
    return wrapMesh(std::move(mesh));
  };
  const float S = 1.5f;  // 半房间尺寸
  auto put = [&](ModelAsset&& m, math::Mat4 w) { uploadInto(dev, std::move(m), out, w); };
  // 底/顶/后/左/右
  put(wall(0.9f, 0.9f, 0.9f), glm::translate(math::Mat4(1.0f), math::Vec3(0, -S, 0)) *
                                  glm::scale(math::Mat4(1.0f), math::Vec3(2 * S, 0.05f, 2 * S)));
  put(wall(0.9f, 0.9f, 0.9f), glm::translate(math::Mat4(1.0f), math::Vec3(0, S, 0)) *
                                  glm::scale(math::Mat4(1.0f), math::Vec3(2 * S, 0.05f, 2 * S)));
  put(wall(0.9f, 0.9f, 0.9f), glm::translate(math::Mat4(1.0f), math::Vec3(0, 0, -S)) *
                                  glm::scale(math::Mat4(1.0f), math::Vec3(2 * S, 2 * S, 0.05f)));
  put(wall(0.85f, 0.2f, 0.2f), glm::translate(math::Mat4(1.0f), math::Vec3(-S, 0, 0)) *
                                   glm::scale(math::Mat4(1.0f), math::Vec3(0.05f, 2 * S, 2 * S)));
  put(wall(0.2f, 0.8f, 0.25f), glm::translate(math::Mat4(1.0f), math::Vec3(S, 0, 0)) *
                                   glm::scale(math::Mat4(1.0f), math::Vec3(0.05f, 2 * S, 2 * S)));
  // 两内盒(一高一矮,各旋转)
  put(wall(0.85f, 0.85f, 0.88f),
      glm::translate(math::Mat4(1.0f), math::Vec3(-0.5f, -0.5f, -0.2f)) *
          glm::rotate(math::Mat4(1.0f), 0.26f, math::Vec3(0, 1, 0)) *
          glm::scale(math::Mat4(1.0f), math::Vec3(0.6f, 1.6f, 0.6f)));
  put(wall(0.85f, 0.85f, 0.88f),
      glm::translate(math::Mat4(1.0f), math::Vec3(0.55f, -0.95f, 0.35f)) *
          glm::rotate(math::Mat4(1.0f), -0.31f, math::Vec3(0, 1, 0)) *
          glm::scale(math::Mat4(1.0f), math::Vec3(0.6f, 0.7f, 0.6f)));
  out.camera.lookAt({0, 0.1f, 5.4f}, {0, 0, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
  // 方向光(斜上方)+ 阴影取景
  LightData dl;
  dl.type = LightType::Directional;
  const float n = std::sqrt(0.3f * 0.3f + 1.0f + 0.16f);
  dl.direction[0] = 0.3f / n;
  dl.direction[1] = 1.0f / n;
  dl.direction[2] = 0.4f / n;
  dl.color[0] = dl.color[1] = dl.color[2] = 3.0f;
  out.lights.push_back(dl);
  out.framingCenter[1] = 0.0f;
  out.framingRadius = 2.2f;
}

void buildLightPlayground(Device& dev, DemoScene& out) {
  // 平面 + 3 球;点光×2(红左/蓝右)+ 聚光×1(顶向下)
  uploadInto(dev, wrapMesh(primitives::makePlane(8.0f, 1.0f)), out, math::Mat4(1.0f));
  for (int i = 0; i < 3; ++i) {
    auto mesh = primitives::makeSphere(0.5f, 32, 16);
    mesh.material.roughnessFactor = 0.3f + 0.3f * float(i);
    mesh.material.metallicFactor = 0.2f;
    uploadInto(dev, wrapMesh(std::move(mesh)), out,
               glm::translate(math::Mat4(1.0f), math::Vec3((i - 1) * 1.6f, 0.5f, 0)));
  }
  LightData pl0;
  pl0.type = LightType::Point;
  pl0.position[0] = -2.5f;
  pl0.position[1] = 2.0f;
  pl0.position[2] = 1.5f;
  pl0.range = 10.0f;
  pl0.color[0] = 4.0f;
  pl0.color[1] = 0.6f;
  pl0.color[2] = 0.4f;
  out.lights.push_back(pl0);
  LightData pl1;
  pl1.type = LightType::Point;
  pl1.position[0] = 2.5f;
  pl1.position[1] = 2.0f;
  pl1.position[2] = 1.5f;
  pl1.range = 10.0f;
  pl1.color[0] = 0.4f;
  pl1.color[1] = 0.6f;
  pl1.color[2] = 4.0f;
  out.lights.push_back(pl1);
  LightData spot;
  spot.type = LightType::Spot;
  spot.position[1] = 3.5f;
  spot.position[2] = 0.5f;
  spot.direction[1] = -1.0f;
  spot.range = 12.0f;
  spot.innerCone = 0.3f;
  spot.outerCone = 0.6f;
  spot.color[0] = spot.color[1] = spot.color[2] = 3.0f;
  out.lights.push_back(spot);
  out.camera.lookAt({0, 2.2f, 5.5f}, {0, 0.5f, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
  out.framingRadius = 4.0f;
}

bool buildSkinnedDemo(Device& dev, DemoScene& out, ModelAsset& storage) {
  const std::string dir =
      (std::filesystem::temp_directory_path() / "rd_demo_skin").string();
  storage = loadGltf(test::writeSkinnedQuad(dir).c_str());
  if (!storage.valid()) return false;
  out.skinnedRes = MeshRenderResource::upload(dev, storage);
  if (!out.skinnedRes) return false;
  out.animator.bind(storage);
  out.animator.play(0);
  out.animated = true;
  out.camera.lookAt({0, 1.2f, 3}, {0, 1, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
  return true;
}

void buildInstancedField(Device& dev, DemoScene& out) {
  // 16×16 球阵,正弦相位波动(N submit;RHI instancing 契约已覆盖,Renderer 级归 P4)
  auto mesh = primitives::makeSphere(0.22f, 16, 8);
  ModelAsset model = wrapMesh(std::move(mesh));
  model.meshes[0].material.roughnessFactor = 0.35f;
  model.meshes[0].material.metallicFactor = 0.8f;
  auto res = MeshRenderResource::upload(dev, model);
  out.resources.push_back(res);
  for (int i = 0; i < 256; ++i) {
    const float x = (i % 16 - 7.5f) * 0.55f;
    const float z = (i / 16 - 7.5f) * 0.55f;
    out.worlds.push_back(glm::translate(math::Mat4(1.0f), math::Vec3(x, 0, z)));
  }
  out.waveField = true;
  out.camera.lookAt({0, 3.5f, 6.5f}, {0, 0, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
  out.framingRadius = 6.0f;
}

bool buildFamousGlb(Device& dev, DemoScene& out, ModelAsset& storage,
                    const char* relPath, bool anim) {
  const std::string path = std::string("assets/") + relPath;
  if (!std::filesystem::exists(path)) {
    RD_LOGW("demo.scene", "资产缺失(scripts/fetch_assets.sh 下载): %s", path.c_str());
    return false;
  }
  storage = loadGltf(path.c_str());
  if (!storage.valid()) return false;
  out.skinnedRes = MeshRenderResource::upload(dev, storage);
  if (!out.skinnedRes) return false;
  if (anim && !storage.animations.empty() && !storage.skins.empty()) {
    out.animator.bind(storage);
    out.animator.play(0);
    out.animated = true;
  } else {
    out.resources.push_back(out.skinnedRes);
    out.worlds.push_back(math::Mat4(1.0f));
    out.skinnedRes = {};
  }
  // 包围球取景
  const float dist = storage.boundingRadius * 2.5f;
  glm::vec3 c(storage.boundingCenter[0], storage.boundingCenter[1],
              storage.boundingCenter[2]);
  glm::vec3 eye = c + glm::vec3(dist * 0.65f, dist * 0.35f, dist * 0.65f);
  out.camera.lookAt({eye.x, eye.y, eye.z}, {c.x, c.y, c.z}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, dist * 0.1f, dist * 10.0f);
  out.framingCenter[0] = c.x;
  out.framingCenter[1] = c.y;
  out.framingCenter[2] = c.z;
  out.framingRadius = storage.boundingRadius;
  return true;
}

} // namespace

const char* const* demoSceneNames(uint32_t& count) {
  count = uint32_t(sizeof(kNames) / sizeof(kNames[0]));
  return kNames;
}

bool buildDemoScene(const char* name, Device& dev, Renderer& renderer, DemoScene& out,
                    ModelAsset& modelStorage) {
  const std::string n = name;
  if (n == "material_balls") {
    buildMaterialBalls(dev, out);
  } else if (n == "cornell_box") {
    buildCornellBox(dev, out);
  } else if (n == "light_playground") {
    buildLightPlayground(dev, out);
  } else if (n == "skinned_demo") {
    if (!buildSkinnedDemo(dev, out, modelStorage)) return false;
  } else if (n == "instanced_field") {
    buildInstancedField(dev, out);
  } else if (n == "sponza") {
    if (!buildFamousGlb(dev, out, modelStorage, "sponza/Sponza.gltf", false)) return false;
  } else if (n == "cesium_man") {
    if (!buildFamousGlb(dev, out, modelStorage, "CesiumMan.glb", true)) return false;
  } else {
    RD_LOGE("demo.scene", "未知场景: %s", name);
    return false;
  }
  if (!out.lights.empty()) renderer.setLights(out.lights);
  renderer.setLightFraming(out.framingCenter, out.framingRadius);
  return true;
}

void submitDemoScene(DemoScene& s, Renderer& renderer, float dt) {
  s.animTime += dt;
  if (s.animated) {
    s.animator.update(dt);
    renderer.submit(s.skinnedRes, math::Mat4(1.0f), s.animator.jointMatrices().data(),
                    uint32_t(s.animator.jointMatrices().size()));
    return;
  }
  if (s.waveField) {  // 共享资源 × N worlds,正弦波动
    for (size_t i = 0; i < s.worlds.size(); ++i) {
      math::Mat4 w = s.worlds[i];
      w[3][1] = std::sin(s.animTime * 2.0f + float(i) * 0.35f) * 0.35f + 0.35f;
      renderer.submit(s.resources[0], w);
    }
    return;
  }
  for (size_t i = 0; i < s.resources.size(); ++i) renderer.submit(s.resources[i], s.worlds[i]);
}

void destroyDemoScene(DemoScene& s, Device& dev) {
  for (auto& r : s.resources)
    if (r) r->destroy(dev);
  if (s.skinnedRes && s.skinnedRes.use_count() > 0 &&
      std::find(s.resources.begin(), s.resources.end(), s.skinnedRes) == s.resources.end())
    s.skinnedRes->destroy(dev);
  s.resources.clear();
  s.worlds.clear();
  s.skinnedRes = {};
}

} // namespace rd::tool
