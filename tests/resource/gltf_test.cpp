// glTF 加载与资源上传测试:BoxTextured 结构断言、DamagedHelmet 多 mesh/32 位索引、
// 不存在文件返回空;GPU 上传(host Metal)句柄有效。
#include <gtest/gtest.h>
#include "common/ktx2_gen.h"
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
