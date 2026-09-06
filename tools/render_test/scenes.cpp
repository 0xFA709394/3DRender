// demo 场景的实现:程序场景经 primitives 组装;知名 glb 从 assets/ 加载。
#include "tools/render_test/scenes.h"
#include "common/ktx2_gen.h"
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
                              "sponza",         "cesium_man",
                              "emissive_bloom", "normal_map_wall",
                              "shadow_gallery", "ktx2_gallery", "alpha_blend",
                              "fox_anim",       "material_ext_gallery",
                              "transmission_gallery", "morph_demo"};

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

void buildEmissiveBloom(Device& dev, DemoScene& out) {
  static const QualityPreset kHigh = {1.0f, 4, 256, 6, 4096, 2048, 1, 0, 1};
  out.quality = &kHigh;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      auto mesh = primitives::makeSphere(0.3f, 32, 16);
      mesh.material.roughnessFactor = 0.4f;
      mesh.material.metallicFactor = 0.0f;
      mesh.material.baseColorFactor[0] = mesh.material.baseColorFactor[1] =
          mesh.material.baseColorFactor[2] = 0.15f;
      const float e = 0.5f + float(i + j) / 6.0f * 7.5f;
      mesh.material.emissiveFactor[0] = e * (i % 2 ? 1.0f : 0.25f);
      mesh.material.emissiveFactor[1] = e * (j % 2 ? 1.0f : 0.4f);
      mesh.material.emissiveFactor[2] = e * ((i + j) % 3 ? 0.6f : 1.0f);
      uploadInto(dev, wrapMesh(std::move(mesh)), out,
                 glm::translate(math::Mat4(1.0f),
                                math::Vec3((i - 1.5f) * 0.85f, (j - 1.5f) * 0.85f, 0)));
    }
  out.camera.lookAt({0, 0, 5.0f}, {0, 0, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
  out.framingRadius = 2.0f;
}

/// 程序化砖墙法线贴图:8 行砖+错缝,砖面凸起/灰浆凹槽 → 高度场转法线。
ImageData makeBrickNormalMap() {
  const uint32_t W = 256, H = 256;
  std::vector<float> height(size_t(W) * H, 0.0f);
  const uint32_t rowH = H / 8, brickW = W / 4;
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      const uint32_t row = y / rowH;
      const uint32_t off = (row % 2) ? brickW / 2 : 0;
      const bool mortarY = (y % rowH) < 3;
      const bool mortarX = ((x + off) % brickW) < 3;
      height[size_t(y) * W + x] = (mortarY || mortarX) ? 0.0f : 1.0f;
    }
  ImageData img;
  img.width = W;
  img.height = H;
  img.pixels.resize(size_t(W) * H * 4);
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      const float hl = height[size_t(y) * W + (x ? x - 1 : 0)];
      const float hr = height[size_t(y) * W + (x + 1 < W ? x + 1 : x)];
      const float hd = height[size_t(y ? y - 1 : 0) * W + x];
      const float hu = height[size_t(y + 1 < H ? y + 1 : y) * W + x];
      const float nx = (hl - hr) * 2.0f, ny = (hd - hu) * 2.0f, nz = 1.0f;
      const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
      uint8_t* p = img.pixels.data() + (size_t(y) * W + x) * 4;
      p[0] = uint8_t((nx * inv * 0.5f + 0.5f) * 255);
      p[1] = uint8_t((ny * inv * 0.5f + 0.5f) * 255);
      p[2] = uint8_t((nz * inv * 0.5f + 0.5f) * 255);
      p[3] = 255;
    }
  return img;
}

void buildNormalMapWall(Device& dev, DemoScene& out) {
  auto mesh = primitives::makePlane(4.0f, 2.0f);
  mesh.material.normal = makeBrickNormalMap();
  mesh.material.normalScale = 1.0f;
  mesh.material.roughnessFactor = 0.85f;
  mesh.material.metallicFactor = 0.0f;
  mesh.material.baseColorFactor[0] = 0.72f;
  mesh.material.baseColorFactor[1] = 0.45f;
  mesh.material.baseColorFactor[2] = 0.35f;
  // 平面立起面向 +Z(绕 X 轴 -90°),加斜向方向光
  uploadInto(dev, wrapMesh(std::move(mesh)), out,
             glm::rotate(math::Mat4(1.0f), -1.5707963f, math::Vec3(1, 0, 0)));
  LightData dl;
  dl.type = LightType::Directional;
  const float n = std::sqrt(0.5f * 0.5f + 0.5f * 0.5f + 0.5f * 0.5f);
  dl.direction[0] = 0.5f / n;
  dl.direction[1] = 0.5f / n;
  dl.direction[2] = 0.5f / n;
  dl.color[0] = dl.color[1] = dl.color[2] = 3.0f;
  out.lights.push_back(dl);
  out.camera.lookAt({0, 0, 4.0f}, {0, 0, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
  out.framingRadius = 2.5f;
}

void buildShadowGallery(Device& dev, DemoScene& out) {
  static const QualityPreset kHigh = {1.0f, 4, 256, 6, 4096, 2048, 1, 0, 1};
  out.quality = &kHigh;
  uploadInto(dev, wrapMesh(primitives::makePlane(10.0f, 2.0f)), out, math::Mat4(1.0f));
  uploadInto(dev, wrapMesh(primitives::makeBox(1.6f, 0.2f, 1.6f)), out,
             glm::translate(math::Mat4(1.0f), math::Vec3(1.2f, 0.8f, -0.5f)));
  uploadInto(dev, wrapMesh(primitives::makeBox(0.7f, 0.7f, 0.7f)), out,
             glm::translate(math::Mat4(1.0f), math::Vec3(0.0f, 1.8f, 0.4f)));
  LightData dl;
  dl.type = LightType::Directional;
  const float n = std::sqrt(0.5f * 0.5f + 1.0f + 0.3f * 0.3f);
  dl.direction[0] = 0.5f / n;
  dl.direction[1] = 1.0f / n;
  dl.direction[2] = 0.3f / n;
  dl.color[0] = dl.color[1] = dl.color[2] = 3.0f;
  out.lights.push_back(dl);
  out.framingRadius = 3.5f;
  out.camera.lookAt({0, 2.5f, 6.0f}, {0, 0.8f, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
}

/// Ktx2Image → ImageData(字段直搬;压缩格式/mip 直通,资源层已支持)。
ImageData toImageData(Ktx2Image&& k) {
  ImageData img;
  img.width = k.width;
  img.height = k.height;
  img.pixels = std::move(k.data);
  img.format = k.format;
  img.mipLevels = k.mipLevels;
  return img;
}

void buildKtx2Gallery(Device& dev, DemoScene& out) {
  // 左右双 quad,同 ktx2 棋盘源:左 RGBA32 解码,右按 caps 转码压缩格式
  const auto ktxBytes = test::makeTestKtx2(256);
  const bool astc = dev.caps().supports(Capability::texture_compression_astc);
  const bool etc2 = dev.caps().supports(Capability::texture_compression_etc2);
  auto leftMesh = primitives::makePlane(1.6f, 1.0f);
  leftMesh.material.baseColor =
      toImageData(decodeKtx2(ktxBytes.data(), ktxBytes.size(), Ktx2Target::Rgba32));
  leftMesh.material.roughnessFactor = 0.9f;
  uploadInto(dev, wrapMesh(std::move(leftMesh)), out,
             glm::rotate(math::Mat4(1.0f), -1.5707963f, math::Vec3(1, 0, 0)) *
                 glm::translate(math::Mat4(1.0f), math::Vec3(-0.9f, 0, 0)));
  auto rightMesh = primitives::makePlane(1.6f, 1.0f);
  rightMesh.material.baseColor = toImageData(
      decodeKtx2(ktxBytes.data(), ktxBytes.size(), pickTranscodeTarget(astc, etc2)));
  rightMesh.material.roughnessFactor = 0.9f;
  uploadInto(dev, wrapMesh(std::move(rightMesh)), out,
             glm::rotate(math::Mat4(1.0f), -1.5707963f, math::Vec3(1, 0, 0)) *
                 glm::translate(math::Mat4(1.0f), math::Vec3(0.9f, 0, 0)));
  out.camera.lookAt({0, 0, 2.6f}, {0, 0, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
  out.framingRadius = 2.0f;
}

void buildAlphaBlend(Device& dev, DemoScene& out) {
  // 后排:3 彩色盒;前排:3 玻璃板(alpha 0.8/0.5/0.3)
  const float cols[3][3] = {{0.9f, 0.3f, 0.3f}, {0.3f, 0.9f, 0.4f}, {0.3f, 0.5f, 0.95f}};
  for (int i = 0; i < 3; ++i) {
    auto mesh = primitives::makeBox(0.9f, 0.9f, 0.9f);
    memcpy(mesh.material.baseColorFactor, cols[i], 12);
    mesh.material.roughnessFactor = 0.4f;
    uploadInto(dev, wrapMesh(std::move(mesh)), out,
               glm::translate(math::Mat4(1.0f),
                              math::Vec3((i - 1) * 1.2f, 0, -1.0f - i * 0.6f)));
  }
  const float alpha[3] = {0.8f, 0.5f, 0.3f};
  for (int i = 0; i < 3; ++i) {
    auto mesh = primitives::makePlane(1.6f, 1.0f);
    memcpy(mesh.material.baseColorFactor, cols[i], 12);
    mesh.material.baseColorFactor[3] = alpha[i];
    mesh.material.alphaBlend = true;  // 引擎混合路径
    uploadInto(dev, wrapMesh(std::move(mesh)), out,
               glm::rotate(math::Mat4(1.0f), -1.5707963f, math::Vec3(1, 0, 0)) *
                   glm::translate(math::Mat4(1.0f),
                                  math::Vec3((i - 1) * 1.3f, 0, 0.5f + i * 0.3f)));
  }
  out.camera.lookAt({0, 0.5f, 5.0f}, {0, 0, -0.5f}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
  out.framingRadius = 3.0f;
}

bool buildFamousGlb(Device& dev, DemoScene& out, ModelAsset& storage,
                    const char* relPath, bool anim) {
  // 资产定位:CWD 相对 assets/ 优先;否则 RD_ASSETS_DIR 环境变量
  std::string path = std::string("assets/") + relPath;
  if (!std::filesystem::exists(path)) {
    const char* alt = getenv("RD_ASSETS_DIR");
    if (alt) path = std::string(alt) + "/" + relPath;
  }
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
    out.animator.update(0.0f);  // 采样到 clip 首帧姿态(动画可能含缩放,取景依赖)
    out.animated = true;
  } else {
    out.resources.push_back(out.skinnedRes);
    out.worlds.push_back(math::Mat4(1.0f));
    out.skinnedRes = {};
  }
  // 包围球取景;动画模型的 nodeGlobals 含节点缩放(如 Fox 根节点 0.01),
  // 而 boundingRadius 按原始顶点算——取景须按关节缩放修正
  float scaleFactor = 1.0f;
  if (out.animated && !storage.skins.empty()) {
    const auto& globals = out.animator.nodeGlobals();
    for (int32_t j : storage.skins[0].joints) {
      const auto& g = globals[size_t(j)];
      const float s = glm::length(glm::vec3(g[0]));
      if (s > 1e-6f && s < scaleFactor) scaleFactor = s;  // 取最小显著缩放
    }
  }
  const float effRadius = storage.boundingRadius * scaleFactor;
  const float dist = effRadius * 2.5f;
  glm::vec3 c(storage.boundingCenter[0] * scaleFactor,
              storage.boundingCenter[1] * scaleFactor,
              storage.boundingCenter[2] * scaleFactor);
  glm::vec3 eye = c + glm::vec3(dist * 0.65f, dist * 0.35f, dist * 0.65f);
  out.camera.lookAt({eye.x, eye.y, eye.z}, {c.x, c.y, c.z}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, dist * 0.1f, dist * 10.0f);
  out.framingCenter[0] = c.x;
  out.framingCenter[1] = c.y;
  out.framingCenter[2] = c.z;
  out.framingRadius = effRadius;
  return true;
}

// morph 演示:双目标球(径向膨胀 + Y 压扁),权重正弦呼吸
void buildMorphDemo(Device& dev, DemoScene& out) {
  auto mesh = primitives::makeSphere(0.6f, 48, 24);
  const size_t vcount = mesh.vertices.size() / 12;
  mesh.material.metallicFactor = 0.1f;
  mesh.material.roughnessFactor = 0.35f;
  mesh.morph = true;
  mesh.morphPosDeltas.resize(vcount * 3 * 2);   // 2 目标
  mesh.morphNormalDeltas.assign(vcount * 3 * 2, 0.0f);
  for (size_t v = 0; v < vcount; ++v) {
    const float* p = &mesh.vertices[v * 12];
    const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    const float inv = len > 1e-5f ? 1.0f / len : 0.0f;
    // t0:径向膨胀 0.2
    mesh.morphPosDeltas[v * 3 + 0] = p[0] * inv * 0.2f;
    mesh.morphPosDeltas[v * 3 + 1] = p[1] * inv * 0.2f;
    mesh.morphPosDeltas[v * 3 + 2] = p[2] * inv * 0.2f;
    // t1:Y 压扁 0.3
    mesh.morphPosDeltas[vcount * 3 + v * 3 + 0] = 0.0f;
    mesh.morphPosDeltas[vcount * 3 + v * 3 + 1] = -p[1] * 0.3f;
    mesh.morphPosDeltas[vcount * 3 + v * 3 + 2] = 0.0f;
  }
  mesh.morphWeights.assign(2, 0.0f);
  mesh.morphTargetNames = {"swell", "squash"};
  uploadInto(dev, wrapMesh(std::move(mesh)), out, math::Mat4(1.0f));
  out.morphPulse = true;
  out.morphWeights.assign(2, 0.0f);
  LightData dir;
  dir.color[0] = dir.color[1] = dir.color[2] = 3.0f;
  out.lights.push_back(dir);
  out.camera.lookAt({0, 0.4f, 3.2f}, {0, 0, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
  out.framingRadius = 1.2f;
}

// KHR 扩展材质三模型并排(资产缺失返回 false → 调用方 skip)
bool buildMaterialExtGallery(Device& dev, DemoScene& out) {
  struct Entry {
    const char* rel;
    float x;
  };
  const Entry entries[] = {
      {"ClearCoatTest.glb", -1.6f}, {"SheenChair.glb", 0.0f}, {"SpecularTest.glb", 1.6f}};
  std::string base = "assets/";
  if (!std::filesystem::exists(base)) {
    if (const char* alt = getenv("RD_ASSETS_DIR")) base = std::string(alt) + "/";
  }
  for (const auto& e : entries) {
    const std::string path = base + e.rel;
    if (!std::filesystem::exists(path)) {
      RD_LOGW("demo.scene", "资产缺失(scripts/fetch_assets.sh 下载): %s", path.c_str());
      return false;
    }
    auto model = loadGltf(path.c_str());
    if (!model.valid()) return false;
    auto res = MeshRenderResource::upload(dev, model);
    if (!res) return false;
    const float s = 0.8f / std::max(model.boundingRadius, 1e-4f);  // 归一到 0.8 半径
    out.resources.push_back(res);
    out.worlds.push_back(glm::translate(math::Mat4(1.0f), math::Vec3(e.x, 0.8f, 0)) *
                         glm::scale(math::Mat4(1.0f), math::Vec3(s)));
  }
  LightData dir;  // 默认方向光 + 适度亮度,高光可见
  dir.color[0] = dir.color[1] = dir.color[2] = 3.0f;
  out.lights.push_back(dir);
  out.camera.lookAt({0, 0.9f, 4.2f}, {0, 0.6f, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 50.0f);
  out.framingCenter[1] = 0.8f;
  out.framingRadius = 2.6f;
  return true;
}

// 透射画廊:棋盘地板 + 三球(清玻璃/毛玻璃/红吸收);KHR transmission/volume 演示
void buildTransmissionGallery(Device& dev, DemoScene& out) {
  // 棋盘地板:6×6 黑白格(两种材质的薄盒;+3 球 = 39 项 < 64 上限)
  const float cell = 0.5f, y0 = -0.001f;
  for (int i = 0; i < 6; ++i)
    for (int j = 0; j < 6; ++j) {
      auto mesh = primitives::makeBox(cell * 0.98f, 0.02f, cell * 0.98f);
      const bool white = (i + j) % 2 == 0;
      mesh.material.baseColorFactor[0] = mesh.material.baseColorFactor[1] =
          mesh.material.baseColorFactor[2] = white ? 0.92f : 0.08f;
      mesh.material.roughnessFactor = 0.85f;
      mesh.material.metallicFactor = 0.0f;
      math::Mat4 w =
          glm::translate(math::Mat4(1.0f),
                         math::Vec3((i - 2.5f) * cell, y0, (j - 2.5f) * cell));
      uploadInto(dev, wrapMesh(std::move(mesh)), out, w);
    }
  // 三球:清玻璃(t=1,rough=0)/毛玻璃(t=1,rough=0.45)/红吸收
  // (t=1,rough=0,thickness=2,attenColor=(0.9,0.1,0.1),attenDist=0.5)
  struct Ball {
    float x;
    float rough;
    float thickness;
    float atten[3];
    float attenDist;
  };
  const Ball balls[] = {
      {-1.2f, 0.0f, 0.0f, {1, 1, 1}, 0.0f},
      {0.0f, 0.45f, 0.0f, {1, 1, 1}, 0.0f},
      {1.2f, 0.0f, 2.0f, {0.9f, 0.1f, 0.1f}, 0.5f},
  };
  for (const auto& b : balls) {
    auto mesh = primitives::makeSphere(0.5f, 48, 24);
    mesh.material.transmissionFactor = 1.0f;
    mesh.material.roughnessFactor = b.rough;
    mesh.material.metallicFactor = 0.0f;
    mesh.material.thicknessFactor = b.thickness;
    mesh.material.attenuationColor[0] = b.atten[0];
    mesh.material.attenuationColor[1] = b.atten[1];
    mesh.material.attenuationColor[2] = b.atten[2];
    mesh.material.attenuationDistance = b.attenDist;
    uploadInto(dev, wrapMesh(std::move(mesh)), out,
               glm::translate(math::Mat4(1.0f), math::Vec3(b.x, 0.5f, 0)));
  }
  LightData dir;  // 默认方向光,高光/透射对比可见
  dir.color[0] = dir.color[1] = dir.color[2] = 3.0f;
  out.lights.push_back(dir);
  out.camera.lookAt({0, 1.1f, 4.4f}, {0, 0.5f, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 50.0f);
  out.framingCenter[1] = 0.5f;
  out.framingRadius = 2.4f;
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
  } else if (n == "emissive_bloom") {
    buildEmissiveBloom(dev, out);
  } else if (n == "normal_map_wall") {
    buildNormalMapWall(dev, out);
  } else if (n == "shadow_gallery") {
    buildShadowGallery(dev, out);
  } else if (n == "ktx2_gallery") {
    buildKtx2Gallery(dev, out);
  } else if (n == "alpha_blend") {
    buildAlphaBlend(dev, out);
  } else if (n == "fox_anim") {
    if (!buildFamousGlb(dev, out, modelStorage, "Fox.glb", true)) return false;
  } else if (n == "material_ext_gallery") {
    if (!buildMaterialExtGallery(dev, out)) return false;
  } else if (n == "transmission_gallery") {
    buildTransmissionGallery(dev, out);
  } else if (n == "morph_demo") {
    buildMorphDemo(dev, out);
  } else {
    RD_LOGE("demo.scene", "未知场景: %s", name);
    return false;
  }
  if (!out.lights.empty()) renderer.setLights(out.lights);
  if (out.quality) renderer.setQuality(*out.quality);
  renderer.setLightFraming(out.framingCenter, out.framingRadius);
  return true;
}

void submitDemoScene(DemoScene& s, Renderer& renderer, float dt) {
  s.animTime += dt;
  if (s.animated) {
    s.animator.update(dt);
    if (getenv("RD_SCENE_DEBUG")) {
      const auto& jm = s.animator.jointMatrices();
      fprintf(stderr, "[scene] joints=%zu j0: |%.3f %.3f %.3f %.3f| j1: |%.3f %.3f %.3f %.3f|\n",
              jm.size(), jm[0][0][0], jm[0][1][0], jm[0][2][0], jm[0][3][0],
              jm[1][0][0], jm[1][1][0], jm[1][2][0], jm[1][3][0]);
      const auto& ng = s.animator.nodeGlobals();
      fprintf(stderr, "[scene] ng1 T=(%.2f,%.2f,%.2f) col0len=%.4f\n", ng[1][3][0],
              ng[1][3][1], ng[1][3][2], glm::length(glm::vec3(ng[1][0])));
    }
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
  if (s.morphPulse && !s.resources.empty()) {  // 权重正弦呼吸(t0/t1 反相)
    const float t = s.animTime;
    const float w[2] = {(std::sin(t * 1.5f) + 1.0f) * 0.5f,
                        (std::cos(t * 1.1f) + 1.0f) * 0.5f};
    s.morphWeights[0] = w[0];
    s.morphWeights[1] = w[1];
    renderer.submit(s.resources[0], s.worlds[0], nullptr, 0, w, 2);
    for (size_t i = 1; i < s.resources.size(); ++i)
      renderer.submit(s.resources[i], s.worlds[i]);
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
