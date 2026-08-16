/**
 * @file gltf_loader.h
 * @brief glTF 2.0 加载(cgltf):glb/gltf → ModelAsset(纯 CPU 数据,不碰 GPU)。
 *
 * 顶点统一规整为交错布局 pos(3f)|normal(3f)|tangent(4f)|uv(2f)(stride 48 字节);
 * 缺失属性(normal/uv)补 0;切线由 mesh_utils 计算(uv 退化时记警告降级)。
 * 索引自适应 u16/u32(>65535 或源为 u32 时用 UInt32)。
 * 内嵌纹理图像解码为 RGBA8;解码失败返回无效 ImageData(调用方决定占位策略)。
 */
#pragma once
#include "resource/image_codec.h"
#include "resource/ktx2_codec.h"
#include "rhi/rhi_types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace rd {

/// 材质数据(glTF metallic-roughness + 本框架支持的 KHR 扩展)。
struct MaterialData {
  ImageData baseColor;            float baseColorFactor[4] = {1, 1, 1, 1};
  ImageData metallicRoughness;    float metallicFactor = 1.0f, roughnessFactor = 1.0f;
  ImageData normal;               float normalScale = 1.0f;
  ImageData emissive;             float emissiveFactor[3] = {0, 0, 0};
  ImageData occlusion;            float occlusionStrength = 1.0f;
  float uvOffset[2] = {0, 0};     // KHR_texture_transform(baseColor 通道;其余贴图同变换)
  float uvScale[2] = {1, 1};
  bool unlit = false;             // KHR_materials_unlit
};

/// 单个 mesh 的 CPU 数据。
struct MeshData {
  std::string name;
  std::vector<float> vertices;    // 交错 pos3|normal3|tangent4|uv2(12 float,stride 48)
  std::vector<uint8_t> indices;   // 原始字节(indexType 决定位宽)
  IndexType indexType = IndexType::UInt16;
  uint32_t indexCount = 0;
  MaterialData material;
};

/// 模型资产:一组 mesh + 包围球(全部 mesh 的 POSITION 合并)。valid()==false 表示加载失败。
struct ModelAsset {
  std::vector<MeshData> meshes;
  float boundingCenter[3] = {0, 0, 0};
  float boundingRadius = 1.0f;
  bool valid() const { return !meshes.empty(); }
};

/// 纹理解码偏好(由调用方按 device caps 推导;loader 不直接碰 device)。
struct TextureLoadPref {
  Ktx2Target ktx2Target = Ktx2Target::Rgba32;  ///< KTX2 转码目标
  uint32_t maxDim = 4096;                       ///< PNG/JPEG 解码尺寸上限(等比降采样)
};

/// 加载 glb/gltf 文件;失败(不存在/解析错/无 mesh)返回空 ModelAsset 并记日志。
/// 单参版本等价于 loadGltf(path, TextureLoadPref{})。
ModelAsset loadGltf(const char* path);
/// 带纹理解码偏好的加载:KTX2(KHR_texture_basisu/魔数探测)按 pref.ktx2Target 转码,
/// PNG/JPEG 按 pref.maxDim 降采样;支持内嵌 buffer_view 与外链 URI 图像。
ModelAsset loadGltf(const char* path, const TextureLoadPref& pref);

} // namespace rd
