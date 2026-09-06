// glTF 加载与资源上传测试:BoxTextured 结构断言、DamagedHelmet 多 mesh/32 位索引、
// 不存在文件返回空;GPU 上传(host Metal)句柄有效。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/ktx2_gen.h"
#include "common/skinned_gen.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "rhi/rhi_device.h"
#include <cstring>
#include <filesystem>

namespace {
const char* kAssets = RD_TEST_DATA_DIR "/assets";
} // namespace

TEST(Gltf, BoxTexturedStructure) {
  std::string path = std::string(kAssets) + "/BoxTextured.glb";
  auto model = rd::loadGltf(path.c_str());
  ASSERT_TRUE(model.valid());
  ASSERT_EQ(model.meshes.size(), 1u);
  const auto& m = model.meshes[0];
  EXPECT_EQ(m.indexCount, 36u);
  EXPECT_EQ(m.indexType, rd::IndexType::UInt16);
  EXPECT_EQ(m.vertices.size(), 24u * 12u);  // 24 顶点 × 12 float(stride 48)
  EXPECT_GT(m.material.baseColor.width, 0u);  // 内嵌纹理解码成功
  EXPECT_EQ(m.material.baseColor.pixels.size(),
            size_t(m.material.baseColor.width) * m.material.baseColor.height * 4u);
}

TEST(Gltf, DamagedHelmetStructure) {
  // DamagedHelmet 实际结构:1 mesh/1 primitive,14556 顶点,46356 个 u16 索引,
  // 属性 NORMAL/POSITION/TEXCOORD_0 全,5 张内嵌 JPEG(baseColor 可解码)。
  std::string path = std::string(kAssets) + "/DamagedHelmet.glb";
  auto model = rd::loadGltf(path.c_str());
  ASSERT_TRUE(model.valid());
  ASSERT_EQ(model.meshes.size(), 1u);
  const auto& m = model.meshes[0];
  EXPECT_EQ(m.indexCount, 46356u);
  EXPECT_EQ(m.indexType, rd::IndexType::UInt16);
  EXPECT_EQ(m.vertices.size(), 14556u * 12u);
  EXPECT_GT(m.material.baseColor.width, 0u);
}

TEST(Gltf, TetraU32IndicesAndMissingAttrs) {
  // 合成资产(生成脚本见提交记录):仅 POSITION 属性 + u32 索引,无纹理。
  // 覆盖:u32 索引路径、normal/uv 缺失补 0、无纹理时 baseColor 无效。
  std::string path = std::string(kAssets) + "/TetraU32.glb";
  auto model = rd::loadGltf(path.c_str());
  ASSERT_TRUE(model.valid());
  ASSERT_EQ(model.meshes.size(), 1u);
  const auto& m = model.meshes[0];
  EXPECT_EQ(m.indexCount, 12u);
  EXPECT_EQ(m.indexType, rd::IndexType::UInt32);
  EXPECT_EQ(m.indices.size(), 12u * 4u);
  EXPECT_EQ(m.vertices.size(), 4u * 12u);  // 12 float/顶点(stride 48)
  EXPECT_EQ(m.material.baseColor.width, 0u);  // 无纹理 → 无效 ImageData(上传走白占位)
}

TEST(Gltf, DamagedHelmetMaterials) {
  std::string path = std::string(kAssets) + "/DamagedHelmet.glb";
  auto model = rd::loadGltf(path.c_str());
  ASSERT_TRUE(model.valid());
  const auto& m = model.meshes[0].material;
  EXPECT_GT(m.baseColor.width, 0u);          // 内嵌 JPEG 全部解码
  EXPECT_GT(m.metallicRoughness.width, 0u);
  EXPECT_GT(m.normal.width, 0u);
  EXPECT_GT(m.emissive.width, 0u);
  EXPECT_GT(m.occlusion.width, 0u);
  EXPECT_FALSE(m.unlit);                     // DamagedHelmet 无 unlit 扩展
  EXPECT_NEAR(m.roughnessFactor, 1.0f, 0.01f);
}

TEST(Gltf, BoundingSphere) {
  std::string path = std::string(kAssets) + "/BoxTextured.glb";
  auto model = rd::loadGltf(path.c_str());
  ASSERT_TRUE(model.valid());
  EXPECT_NEAR(model.boundingRadius, 0.866f, 0.01f);  // Box ±0.5 → 半径 √3/2
  EXPECT_NEAR(model.boundingCenter[0], 0.0f, 0.01f);
}

TEST(Gltf, BoxHasTangents) {
  // 48B 布局含非零切线(12 float/顶点)
  std::string path = std::string(kAssets) + "/BoxTextured.glb";
  auto model = rd::loadGltf(path.c_str());
  ASSERT_TRUE(model.valid());
  EXPECT_EQ(model.meshes[0].vertices.size(), 24u * 12u);
  float t0 = model.meshes[0].vertices[6];  // 首顶点 tangent.x
  EXPECT_GT(std::abs(t0), 0.5f);
}

TEST(Gltf, MissingFileReturnsEmpty) {
  auto model = rd::loadGltf("nonexistent.glb");
  EXPECT_FALSE(model.valid());
}

TEST(Gltf, UploadGpuResourcesMetal) {
#if defined(__APPLE__)
  rd::DeviceDesc d;
  d.backend = rd::Backend::Metal;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  std::string path = std::string(kAssets) + "/BoxTextured.glb";
  auto model = rd::loadGltf(path.c_str());
  ASSERT_TRUE(model.valid());
  auto res = rd::MeshRenderResource::upload(*dev, model);
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(res->meshes().size(), 1u);
  EXPECT_TRUE(res->meshes()[0].vbo.valid());
  EXPECT_TRUE(res->meshes()[0].ibo.valid());
  EXPECT_TRUE(res->meshes()[0].baseColorTex.valid());
  EXPECT_TRUE(res->sampler().valid());
  res->destroy(*dev);
#endif
}

// KHR_texture_basisu + 外链 URI:运行时生成 gltf+bin+ktx2 到临时目录再加载。
TEST(Gltf, BasisuExternalUri) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "rd_gltf_basisu";
  fs::create_directories(dir);
  const std::string gltfPath = (dir / "tri.gltf").string();
  const std::string binPath = (dir / "tri.bin").string();
  const std::string ktxPath = (dir / "tex.ktx2").string();
  ASSERT_TRUE(rd::test::writeTestKtx2(ktxPath.c_str(), 8));

  // 单三角形:pos(36B)|uv(24B)|idx(6B) 三段布局写入 tri.bin(共 66B)
  const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  const float uv[6] = {0, 0, 1, 0, 0, 1};
  const uint16_t idx[3] = {0, 1, 2};
  {
    FILE* f = fopen(binPath.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(pos, 4, 9, f);
    fwrite(uv, 4, 6, f);
    fwrite(idx, 2, 3, f);
    fclose(f);
  }
  // gltf JSON:KHR_texture_basisu 引用外链 tex.ktx2;POSITION/UV 各一个 bufferView
  const char* json = R"({
    "asset": {"version": "2.0"},
    "extensionsUsed": ["KHR_texture_basisu"],
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                                "indices": 2, "material": 0}]}],
    "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
    "textures": [{"extensions": {"KHR_texture_basisu": {"source": 0}}}],
    "images": [{"uri": "tex.ktx2"}],
    "buffers": [{"uri": "tri.bin", "byteLength": 66}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 24},
      {"buffer": 0, "byteOffset": 60, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
      {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  {
    FILE* f = fopen(gltfPath.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(json, 1, strlen(json), f);
    fclose(f);
  }

  rd::TextureLoadPref pref;
  pref.ktx2Target = rd::Ktx2Target::Rgba32;  // 不依赖 GPU caps,走 rgba32 路径
  auto model = rd::loadGltf(gltfPath.c_str(), pref);
  ASSERT_TRUE(model.valid());
  ASSERT_EQ(model.meshes.size(), 1u);
  const auto& bc = model.meshes[0].material.baseColor;
  EXPECT_EQ(bc.width, 8u);
  EXPECT_EQ(bc.height, 8u);
  EXPECT_EQ(bc.format, rd::Format::RGBA8_UNORM);
  EXPECT_EQ(bc.mipLevels, 2u);
  EXPECT_FALSE(bc.pixels.empty());
}

// KHR_lights_punctual:运行时生成带灯 gltf(节点旋转定方向),断言解析结果。
TEST(Gltf, PunctualLights) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "rd_gltf_lights";
  fs::create_directories(dir);
  // 三角形几何(pos only)
  const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  const uint16_t idx[3] = {0, 1, 2};
  const std::string binPath = (dir / "tri.bin").string();
  {
    FILE* f = fopen(binPath.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(pos, 4, 9, f);
    fwrite(idx, 2, 3, f);
    fclose(f);
  }
  // 灯光:node1=方向光(绕 X 轴 -90°:glTF -Z 灯向 → 世界 -Y;L=-dir → +Y)
  const char* json = R"({
    "asset": {"version": "2.0"},
    "extensionsUsed": ["KHR_lights_punctual"],
    "extensions": {
      "KHR_lights_punctual": {
        "lights": [
          {"type": "directional", "color": [1, 0.5, 0.25], "intensity": 2.0},
          {"type": "point", "color": [1, 1, 1], "intensity": 4.0, "range": 10.0},
          {"type": "spot", "intensity": 1.0, "range": 5.0,
           "spot": {"innerConeAngle": 0.2, "outerConeAngle": 0.5}}
        ]
      }
    },
    "scenes": [{"nodes": [0, 1, 2, 3]}], "scene": 0,
    "nodes": [
      {"mesh": 0},
      {"extensions": {"KHR_lights_punctual": {"light": 0}},
       "rotation": [-0.7071068, 0, 0, 0.7071068]},
      {"extensions": {"KHR_lights_punctual": {"light": 1}},
       "translation": [1, 2, 3]},
      {"extensions": {"KHR_lights_punctual": {"light": 2}}}
    ],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
    "buffers": [{"uri": "tri.bin", "byteLength": 42}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  const std::string gltfPath = (dir / "lit.gltf").string();
  {
    FILE* f = fopen(gltfPath.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(json, 1, strlen(json), f);
    fclose(f);
  }
  auto model = rd::loadGltf(gltfPath.c_str());
  ASSERT_TRUE(model.valid());
  ASSERT_EQ(model.lights.size(), 3u);

  // 方向光:L = 节点旋转×(0,0,-1) 取反 → (0,1,0);颜色×intensity
  const auto& d0 = model.lights[0];
  EXPECT_EQ(d0.type, rd::LightType::Directional);
  EXPECT_NEAR(d0.direction[0], 0.0f, 1e-3f);
  EXPECT_NEAR(d0.direction[1], 1.0f, 1e-3f);
  EXPECT_NEAR(d0.direction[2], 0.0f, 1e-3f);
  EXPECT_NEAR(d0.color[0], 2.0f, 1e-3f);    // 1.0 × 2.0
  EXPECT_NEAR(d0.color[1], 1.0f, 1e-3f);    // 0.5 × 2.0

  const auto& p1 = model.lights[1];
  EXPECT_EQ(p1.type, rd::LightType::Point);
  EXPECT_NEAR(p1.position[0], 1.0f, 1e-3f);
  EXPECT_NEAR(p1.position[1], 2.0f, 1e-3f);
  EXPECT_NEAR(p1.position[2], 3.0f, 1e-3f);
  EXPECT_NEAR(p1.range, 10.0f, 1e-3f);

  const auto& s2 = model.lights[2];
  EXPECT_EQ(s2.type, rd::LightType::Spot);
  EXPECT_NEAR(s2.innerCone, 0.2f, 1e-3f);
  EXPECT_NEAR(s2.outerCone, 0.5f, 1e-3f);
  EXPECT_NEAR(s2.range, 5.0f, 1e-3f);

  // 无灯模型:lights 为空(BoxTextured 无 KHR_lights_punctual)
  auto box = rd::loadGltf((std::string(kAssets) + "/BoxTextured.glb").c_str());
  ASSERT_TRUE(box.valid());
  EXPECT_TRUE(box.lights.empty());
}

// 蒙皮解析:nodes/skins/animations + 80B 顶点布局。
TEST(Gltf, SkinnedQuad) {
  const std::string dir =
      (std::filesystem::temp_directory_path() / "rd_gltf_skin").string();
  const std::string path = rd::test::writeSkinnedQuad(dir);
  ASSERT_FALSE(path.empty());
  auto model = rd::loadGltf(path.c_str());
  ASSERT_TRUE(model.valid());

  // 节点层级:3 节点(node1 parent=0,平移 y=1)
  ASSERT_EQ(model.nodes.size(), 3u);
  EXPECT_EQ(model.nodes[1].parent, 0);
  EXPECT_NEAR(model.nodes[1].translation[1], 1.0f, 1e-4f);
  EXPECT_EQ(model.nodes[2].mesh, 0);

  // 蒙皮网格:80B 布局 + joints/weights 正确
  ASSERT_EQ(model.meshes.size(), 1u);
  const auto& mesh = model.meshes[0];
  EXPECT_TRUE(mesh.skinned);
  ASSERT_EQ(mesh.vertices.size(), 4u * 20u);  // 20 float/顶点(80B)
  // 顶点 0(y=0):joints=(0,0,0,0),weights=(1,0,0,0)
  EXPECT_FLOAT_EQ(mesh.vertices[12], 0.0f);   // joints4f@12(float 下标 48B/4)
  EXPECT_FLOAT_EQ(mesh.vertices[16], 1.0f);   // weights4f@16
  // 顶点 2(y=2):joints=(1,0,0,0)
  EXPECT_FLOAT_EQ(mesh.vertices[2 * 20 + 12], 1.0f);
  EXPECT_FLOAT_EQ(mesh.vertices[2 * 20 + 16], 1.0f);

  // skins:2 关节 + IBM(joint1 平移 y=-1)
  ASSERT_EQ(model.skins.size(), 1u);
  ASSERT_EQ(model.skins[0].joints.size(), 2u);
  EXPECT_EQ(model.skins[0].joints[1], 1);
  ASSERT_EQ(model.skins[0].inverseBindMatrices.size(), 32u);
  EXPECT_NEAR(model.skins[0].inverseBindMatrices[16 + 13], -1.0f, 1e-4f);

  // animations:clip "bend",1 通道 rotation,时长 1s
  ASSERT_EQ(model.animations.size(), 1u);
  const auto& clip = model.animations[0];
  EXPECT_EQ(clip.name, "bend");
  ASSERT_EQ(clip.channels.size(), 1u);
  EXPECT_EQ(clip.channels[0].node, 1);
  EXPECT_EQ(clip.channels[0].path, 1);  // rotation
  ASSERT_EQ(clip.channels[0].times.size(), 2u);
  EXPECT_FLOAT_EQ(clip.duration, 1.0f);
  EXPECT_NEAR(clip.channels[0].values[6], 0.70710678f, 1e-4f);

  // 非蒙皮模型:节点表照常导入(统一行为),但无蒙皮/动画
  auto box = rd::loadGltf((std::string(kAssets) + "/BoxTextured.glb").c_str());
  ASSERT_TRUE(box.valid());
  EXPECT_FALSE(box.meshes[0].skinned);
  EXPECT_TRUE(box.skins.empty());
  EXPECT_TRUE(box.animations.empty());
}

// alphaMode=BLEND 解析。
TEST(Gltf, AlphaBlendMode) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "rd_gltf_blend";
  fs::create_directories(dir);
  const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  const uint16_t idx[3] = {0, 1, 2};
  const std::string binPath = (dir / "tri.bin").string();
  {
    FILE* f = fopen(binPath.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(pos, 4, 9, f);
    fwrite(idx, 2, 3, f);
    fclose(f);
  }
  const char* json = R"({
    "asset": {"version": "2.0"},
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                "indices": 1, "material": 0}]}],
    "materials": [{"alphaMode": "BLEND",
      "pbrMetallicRoughness": {"baseColorFactor": [1, 0.5, 0.2, 0.4]}}],
    "buffers": [{"uri": "tri.bin", "byteLength": 42}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  const std::string gltfPath = (dir / "blend.gltf").string();
  {
    FILE* f = fopen(gltfPath.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(json, 1, strlen(json), f);
    fclose(f);
  }
  auto model = rd::loadGltf(gltfPath.c_str());
  ASSERT_TRUE(model.valid());
  ASSERT_EQ(model.meshes.size(), 1u);
  EXPECT_TRUE(model.meshes[0].material.alphaBlend);
  EXPECT_NEAR(model.meshes[0].material.baseColorFactor[3], 0.4f, 1e-4f);
  // 无 alphaMode 材质为 false
  auto box = rd::loadGltf((std::string(kAssets) + "/BoxTextured.glb").c_str());
  ASSERT_TRUE(box.valid());
  EXPECT_FALSE(box.meshes[0].material.alphaBlend);
}

// alphaMode=MASK 解析:cutoff 读取/默认 0.5
TEST(Gltf, AlphaMask) {
  // 复用 BLEND 测试的三角 glTF 骨架,改 alphaMode=MASK + alphaCutoff
  const char* gltf = R"({
    "asset": {"version": "2.0"},
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                "indices": 1, "material": 0}]}],
    "materials": [{"alphaMode": "MASK", "alphaCutoff": 0.25,
      "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1]}}],
    "buffers": [{"uri": "tri.bin", "byteLength": 42}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  const std::string dir = (std::filesystem::temp_directory_path() / "rd_gltf_mask").string();
  std::filesystem::create_directories(dir);
  { FILE* f = fopen((dir + "/tri.gltf").c_str(), "w"); fputs(gltf, f); fclose(f); }
  { FILE* f = fopen((dir + "/tri.bin").c_str(), "wb");
    const float pos[9] = {0,0,0, 1,0,0, 0,1,0};
    const uint16_t idx[3] = {0, 1, 2};
    fwrite(pos, 4, 9, f); fwrite(idx, 2, 3, f); fclose(f); }
  auto model = rd::loadGltf((dir + "/tri.gltf").c_str());
  ASSERT_TRUE(model.valid());
  ASSERT_FALSE(model.meshes.empty());
  EXPECT_FALSE(model.meshes[0].material.alphaBlend);
  EXPECT_FLOAT_EQ(model.meshes[0].material.alphaCutoff, 0.25f);
  // 默认 cutoff(不写 alphaCutoff)= 0.5
  const std::string g2 = std::string(gltf);  // 改:去 alphaCutoff 字段
  const std::string g2s = "{\"alphaMode\": \"MASK\"}";
  // 直接拼第二个文件
  const char* gltf2 = R"({
    "asset": {"version": "2.0"},
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                "indices": 1, "material": 0}]}],
    "materials": [{"alphaMode": "MASK"}],
    "buffers": [{"uri": "tri.bin", "byteLength": 42}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  { FILE* f = fopen((dir + "/tri2.gltf").c_str(), "w"); fputs(gltf2, f); fclose(f); }
  auto model2 = rd::loadGltf((dir + "/tri2.gltf").c_str());
  ASSERT_TRUE(model2.valid());
  EXPECT_FLOAT_EQ(model2.meshes[0].material.alphaCutoff, 0.5f);  // glTF 默认
  (void)g2; (void)g2s;
}

// KHR_materials_emissive_strength:emissiveFactor × strength
TEST(Gltf, EmissiveStrength) {
  const char* gltf = R"({
    "asset": {"version": "2.0"},
    "extensionsUsed": ["KHR_materials_emissive_strength"],
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                "indices": 1, "material": 0}]}],
    "materials": [{"emissiveFactor": [1.0, 0.5, 0.0],
      "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4.0}}}],
    "buffers": [{"uri": "tri.bin", "byteLength": 42}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  const std::string dir = (std::filesystem::temp_directory_path() / "rd_gltf_ems").string();
  std::filesystem::create_directories(dir);
  { FILE* f = fopen((dir + "/tri.gltf").c_str(), "w"); fputs(gltf, f); fclose(f); }
  { FILE* f = fopen((dir + "/tri.bin").c_str(), "wb");
    const float pos[9] = {0,0,0, 1,0,0, 0,1,0};
    const uint16_t idx[3] = {0, 1, 2};
    fwrite(pos, 4, 9, f); fwrite(idx, 2, 3, f); fclose(f); }
  auto model = rd::loadGltf((dir + "/tri.gltf").c_str());
  ASSERT_TRUE(model.valid());
  EXPECT_FLOAT_EQ(model.meshes[0].material.emissiveFactor[0], 4.0f);   // 1.0 × 4
  EXPECT_FLOAT_EQ(model.meshes[0].material.emissiveFactor[1], 2.0f);   // 0.5 × 4
}

// KHR 材质扩展四件套:因子/纹理/法线 scale 解析
TEST(Gltf, ExtMaterials) {
  const char* gltf = R"({
    "asset": {"version": "2.0"},
    "extensionsUsed": ["KHR_materials_clearcoat","KHR_materials_sheen",
                       "KHR_materials_specular","KHR_materials_ior"],
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                "indices": 1, "material": 0}]}],
    "materials": [{"extensions": {
      "KHR_materials_clearcoat": {"clearcoatFactor": 0.8,
        "clearcoatRoughnessFactor": 0.25,
        "clearcoatTexture": {"index": 0},
        "clearcoatRoughnessTexture": {"index": 0},
        "clearcoatNormalTexture": {"index": 0, "scale": 0.6}},
      "KHR_materials_sheen": {"sheenColorFactor": [0.5, 0.6, 0.7],
        "sheenRoughnessFactor": 0.4,
        "sheenColorTexture": {"index": 0},
        "sheenRoughnessTexture": {"index": 0}},
      "KHR_materials_specular": {"specularFactor": 0.7,
        "specularColorFactor": [0.9, 0.8, 0.7],
        "specularColorTexture": {"index": 0},
        "specularTexture": {"index": 0}},
      "KHR_materials_ior": {"ior": 1.33}
    }}],
    "textures": [{"source": 0}],
    "images": [{"uri": "ext.png"}],
    "buffers": [{"uri": "tri.bin", "byteLength": 42}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  const std::string dir = (std::filesystem::temp_directory_path() / "rd_gltf_extmat").string();
  std::filesystem::create_directories(dir);
  { FILE* f = fopen((dir + "/tri.gltf").c_str(), "w"); fputs(gltf, f); fclose(f); }
  { FILE* f = fopen((dir + "/tri.bin").c_str(), "wb");
    const float pos[9] = {0,0,0, 1,0,0, 0,1,0};
    const uint16_t idx[3] = {0, 1, 2};
    fwrite(pos, 4, 9, f); fwrite(idx, 2, 3, f); fclose(f); }
  const uint8_t px[16] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
  ASSERT_TRUE(rd::test::savePNG(dir + "/ext.png", 2, 2, px));
  auto model = rd::loadGltf((dir + "/tri.gltf").c_str());
  ASSERT_TRUE(model.valid());
  const auto& m = model.meshes[0].material;
  EXPECT_FLOAT_EQ(m.clearcoatFactor, 0.8f);
  EXPECT_FLOAT_EQ(m.clearcoatRoughnessFactor, 0.25f);
  EXPECT_FLOAT_EQ(m.clearcoatNormalScale, 0.6f);
  EXPECT_FLOAT_EQ(m.sheenColorFactor[0], 0.5f);
  EXPECT_FLOAT_EQ(m.sheenColorFactor[2], 0.7f);
  EXPECT_FLOAT_EQ(m.sheenRoughnessFactor, 0.4f);
  EXPECT_FLOAT_EQ(m.specularFactor, 0.7f);
  EXPECT_FLOAT_EQ(m.specularColorFactor[0], 0.9f);
  EXPECT_FLOAT_EQ(m.ior, 1.33f);
  EXPECT_EQ(m.clearcoat.width, 2u);        // 外链 URI 走 decodeImage
  EXPECT_EQ(m.clearcoatRough.width, 2u);
  EXPECT_EQ(m.clearcoatNormal.width, 2u);
  EXPECT_EQ(m.sheenColor.width, 2u);
  EXPECT_EQ(m.sheenRough.width, 2u);
  EXPECT_EQ(m.specularColorTex.width, 2u);
  EXPECT_EQ(m.specularTex.width, 2u);
}

// 默认零操作语义:无扩展材质 → 全部默认值(渲染零回归的解析侧依据)
TEST(Gltf, ExtMaterialsDefaults) {
  const char* gltf = R"({
    "asset": {"version": "2.0"},
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                "indices": 1, "material": 0}]}],
    "materials": [{}],
    "buffers": [{"uri": "tri.bin", "byteLength": 42}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  const std::string dir = (std::filesystem::temp_directory_path() / "rd_gltf_extmat_d").string();
  std::filesystem::create_directories(dir);
  { FILE* f = fopen((dir + "/tri.gltf").c_str(), "w"); fputs(gltf, f); fclose(f); }
  { FILE* f = fopen((dir + "/tri.bin").c_str(), "wb");
    const float pos[9] = {0,0,0, 1,0,0, 0,1,0};
    const uint16_t idx[3] = {0, 1, 2};
    fwrite(pos, 4, 9, f); fwrite(idx, 2, 3, f); fclose(f); }
  auto model = rd::loadGltf((dir + "/tri.gltf").c_str());
  ASSERT_TRUE(model.valid());
  const auto& m = model.meshes[0].material;
  EXPECT_FLOAT_EQ(m.clearcoatFactor, 0.0f);
  EXPECT_FLOAT_EQ(m.clearcoatRoughnessFactor, 0.0f);
  EXPECT_FLOAT_EQ(m.clearcoatNormalScale, 1.0f);
  EXPECT_FLOAT_EQ(m.sheenColorFactor[0], 0.0f);
  EXPECT_FLOAT_EQ(m.sheenRoughnessFactor, 0.0f);
  EXPECT_FLOAT_EQ(m.specularFactor, 1.0f);
  EXPECT_FLOAT_EQ(m.specularColorFactor[0], 1.0f);
  EXPECT_FLOAT_EQ(m.ior, 1.5f);
  EXPECT_EQ(m.clearcoat.width, 0u);   // 无纹理 → 无效 ImageData
  EXPECT_EQ(m.sheenColor.width, 0u);
  EXPECT_EQ(m.specularTex.width, 0u);
}

// KHR_materials_transmission + KHR_materials_volume 解析(默认值零操作)
TEST(Gltf, TransmissionVolume) {
  const char* gltf = R"({
    "asset": {"version": "2.0"},
    "extensionsUsed": ["KHR_materials_transmission","KHR_materials_volume"],
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                "indices": 1, "material": 0}]}],
    "materials": [{"extensions": {
      "KHR_materials_transmission": {"transmissionFactor": 0.9,
        "transmissionTexture": {"index": 0}},
      "KHR_materials_volume": {"thicknessFactor": 2.0,
        "thicknessTexture": {"index": 0},
        "attenuationColor": [0.8, 0.2, 0.1],
        "attenuationDistance": 0.5}
    }}],
    "textures": [{"source": 0}],
    "images": [{"uri": "ext.png"}],
    "buffers": [{"uri": "tri.bin", "byteLength": 42}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  const std::string dir = (std::filesystem::temp_directory_path() / "rd_gltf_trans").string();
  std::filesystem::create_directories(dir);
  { FILE* f = fopen((dir + "/tri.gltf").c_str(), "w"); fputs(gltf, f); fclose(f); }
  { FILE* f = fopen((dir + "/tri.bin").c_str(), "wb");
    const float pos[9] = {0,0,0, 1,0,0, 0,1,0};
    const uint16_t idx[3] = {0, 1, 2};
    fwrite(pos, 4, 9, f); fwrite(idx, 2, 3, f); fclose(f); }
  const uint8_t px[16] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
  ASSERT_TRUE(rd::test::savePNG(dir + "/ext.png", 2, 2, px));
  auto model = rd::loadGltf((dir + "/tri.gltf").c_str());
  ASSERT_TRUE(model.valid());
  const auto& m = model.meshes[0].material;
  EXPECT_FLOAT_EQ(m.transmissionFactor, 0.9f);
  EXPECT_FLOAT_EQ(m.thicknessFactor, 2.0f);
  EXPECT_NEAR(m.attenuationColor[0], 0.8f, 1e-6);
  EXPECT_NEAR(m.attenuationColor[1], 0.2f, 1e-6);
  EXPECT_NEAR(m.attenuationColor[2], 0.1f, 1e-6);
  EXPECT_FLOAT_EQ(m.attenuationDistance, 0.5f);
  EXPECT_EQ(m.transmissionTex.width, 2u);
  EXPECT_EQ(m.thicknessTex.width, 2u);
}

// 默认零操作:无扩展材质 → transmission/thickness=0、attenuation=(1,1,1)/0(∞ 哨兵)
TEST(Gltf, TransmissionVolumeDefaults) {
  const char* gltf = R"({
    "asset": {"version": "2.0"},
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                "indices": 1, "material": 0}]}],
    "materials": [{}],
    "buffers": [{"uri": "tri.bin", "byteLength": 42}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  const std::string dir = (std::filesystem::temp_directory_path() / "rd_gltf_trans_d").string();
  std::filesystem::create_directories(dir);
  { FILE* f = fopen((dir + "/tri.gltf").c_str(), "w"); fputs(gltf, f); fclose(f); }
  { FILE* f = fopen((dir + "/tri.bin").c_str(), "wb");
    const float pos[9] = {0,0,0, 1,0,0, 0,1,0};
    const uint16_t idx[3] = {0, 1, 2};
    fwrite(pos, 4, 9, f); fwrite(idx, 2, 3, f); fclose(f); }
  auto model = rd::loadGltf((dir + "/tri.gltf").c_str());
  ASSERT_TRUE(model.valid());
  const auto& m = model.meshes[0].material;
  EXPECT_FLOAT_EQ(m.transmissionFactor, 0.0f);
  EXPECT_FLOAT_EQ(m.thicknessFactor, 0.0f);
  EXPECT_FLOAT_EQ(m.attenuationColor[0], 1.0f);
  EXPECT_FLOAT_EQ(m.attenuationDistance, 0.0f);  // 0 = +∞ 哨兵(无吸收)
  EXPECT_EQ(m.transmissionTex.width, 0u);
  EXPECT_EQ(m.thicknessTex.width, 0u);
}

// morph targets:双目标增量 + 初始权重 + targetNames + 超限截断
TEST(Gltf, MorphTargets) {
  const char* gltf = R"({
    "asset": {"version": "2.0"},
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"weights": [0.5, 1.0],
      "extras": {"targetNames": ["swell", "lift"]},
      "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1},
                      "indices": 2, "targets": [
        {"POSITION": 3},
        {"POSITION": 4, "NORMAL": 5}]}]}],
    "buffers": [{"uri": "tri.bin", "byteLength": 186}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0,   "byteLength": 36},
      {"buffer": 0, "byteOffset": 36,  "byteLength": 36},
      {"buffer": 0, "byteOffset": 72,  "byteLength": 6},
      {"buffer": 0, "byteOffset": 78,  "byteLength": 36},
      {"buffer": 0, "byteOffset": 114, "byteLength": 36},
      {"buffer": 0, "byteOffset": 150, "byteLength": 36}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"},
      {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 5, "componentType": 5126, "count": 3, "type": "VEC3"}]
  })";
  const std::string dir = (std::filesystem::temp_directory_path() / "rd_gltf_morph").string();
  std::filesystem::create_directories(dir);
  { FILE* f = fopen((dir + "/tri.gltf").c_str(), "w"); fputs(gltf, f); fclose(f); }
  {
    FILE* f = fopen((dir + "/tri.bin").c_str(), "wb");
    const float pos[9] = {0,0,0, 1,0,0, 0,1,0};
    const float nrm[9] = {0,0,1, 0,0,1, 0,0,1};
    const uint16_t idx[3] = {0, 1, 2};
    const float d0[9] = {0.1f,0,0, 0.1f,0,0, 0.1f,0,0};      // t0 pos
    const float d1[9] = {0,0.2f,0, 0,0.2f,0, 0,0.2f,0};      // t1 pos
    const float dn[9] = {1,0,0, 1,0,0, 1,0,0};               // t1 normal
    fwrite(pos, 4, 9, f); fwrite(nrm, 4, 9, f); fwrite(idx, 2, 3, f);
    fwrite(d0, 4, 9, f); fwrite(d1, 4, 9, f); fwrite(dn, 4, 9, f);
    fclose(f);
  }
  auto model = rd::loadGltf((dir + "/tri.gltf").c_str());
  ASSERT_TRUE(model.valid());
  const auto& m = model.meshes[0];
  EXPECT_TRUE(m.morph);
  ASSERT_EQ(m.morphPosDeltas.size(), size_t(2 * 9));
  EXPECT_FLOAT_EQ(m.morphPosDeltas[0], 0.1f);
  EXPECT_FLOAT_EQ(m.morphPosDeltas[10], 0.2f);
  ASSERT_EQ(m.morphNormalDeltas.size(), size_t(2 * 9));
  EXPECT_FLOAT_EQ(m.morphNormalDeltas[3], 0.0f);             // t0 normal 未声明 → 0
  EXPECT_FLOAT_EQ(m.morphNormalDeltas[9 + 3], 1.0f);         // t1 normal x
  ASSERT_EQ(m.morphWeights.size(), 2u);
  EXPECT_FLOAT_EQ(m.morphWeights[0], 0.5f);
  EXPECT_FLOAT_EQ(m.morphWeights[1], 1.0f);
  ASSERT_EQ(m.morphTargetNames.size(), 2u);
  EXPECT_EQ(m.morphTargetNames[0], "swell");
}

// 超限截断:10 目标 → 8 + morphWeights 同步截断
TEST(Gltf, MorphTargetTruncation) {
  std::string tgts;
  for (int i = 0; i < 10; ++i) tgts += "{\"POSITION\": 0},";
  tgts.pop_back();
  std::string wts;
  for (int i = 0; i < 10; ++i) wts += "0.1,";
  wts.pop_back();
  const std::string gltf = std::string(R"({
    "asset": {"version": "2.0"},
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"weights": [)" + wts + R"(],
      "primitives": [{"attributes": {"POSITION": 0}, "indices": 1,
                      "targets": [)" + tgts + R"(]}]}],
    "buffers": [{"uri": "tri.bin", "byteLength": 42}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })");
  const std::string dir = (std::filesystem::temp_directory_path() / "rd_gltf_morph_t").string();
  std::filesystem::create_directories(dir);
  { FILE* f = fopen((dir + "/tri.gltf").c_str(), "w"); fputs(gltf.c_str(), f); fclose(f); }
  { FILE* f = fopen((dir + "/tri.bin").c_str(), "wb");
    const float pos[9] = {0,0,0, 1,0,0, 0,1,0};
    const uint16_t idx[3] = {0, 1, 2};
    fwrite(pos, 4, 9, f); fwrite(idx, 2, 3, f); fclose(f); }
  auto model = rd::loadGltf((dir + "/tri.gltf").c_str());
  ASSERT_TRUE(model.valid());
  const auto& m = model.meshes[0];
  EXPECT_TRUE(m.morph);
  EXPECT_EQ(m.morphPosDeltas.size() / 9, 8u);
  EXPECT_EQ(m.morphNormalDeltas.size() / 9, 8u);
  EXPECT_EQ(m.morphWeights.size(), 8u);
}
