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

/// 光源类型(KHR_lights_punctual 全类型)。
enum class LightType : uint32_t { Directional = 0, Point = 1, Spot = 2 };

/// 光源数据(渲染语义):direction 为"指向光源的方向"(shader 内 dot(N,L) 直接用);
/// color 已乘 intensity。
struct LightData {
  LightType type = LightType::Directional;
  float direction[3] = {0, 1, 0};   ///< 指向光源(dir/spot 用)
  float position[3] = {0, 0, 0};    ///< point/spot 用
  float color[3] = {1, 1, 1};       ///< rgb × intensity
  float range = 0.0f;               ///< 0=无限
  float innerCone = 0.0f;           ///< spot 内锥角(弧度)
  float outerCone = 0.0f;           ///< spot 外锥角(弧度)
};

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
  bool alphaBlend = false;        // alphaMode=BLEND
  float alphaCutoff = 0.0f;       // alphaMode=MASK 阈值(0=非 MASK;glTF 默认 0.5)
  // ---- KHR 材质扩展四件套(默认值 = 零操作语义:渲染与无扩展逐像素一致)----
  // KHR_materials_clearcoat
  ImageData clearcoat;            float clearcoatFactor = 0.0f;
  ImageData clearcoatRough;       float clearcoatRoughnessFactor = 0.0f;
  ImageData clearcoatNormal;      float clearcoatNormalScale = 1.0f;
  // KHR_materials_sheen
  ImageData sheenColor;           float sheenColorFactor[3] = {0, 0, 0};
  ImageData sheenRough;           float sheenRoughnessFactor = 0.0f;
  // KHR_materials_specular(specular 因子在纹理 A 通道)
  ImageData specularColorTex;     float specularColorFactor[3] = {1, 1, 1};
  ImageData specularTex;          float specularFactor = 1.0f;
  // KHR_materials_ior(独立于 specular 生效;1.5 → f0=0.04 与现状一致)
  float ior = 1.5f;
  // ---- KHR transmission/volume(P4-B;默认值 = 零操作语义)----
  // KHR_materials_transmission:透射(场景色折射采样,渲染层两段 pass)
  ImageData transmissionTex;       float transmissionFactor = 0.0f;  // 0=无透射
  // KHR_materials_volume:厚度 + Beer-Lambert 吸收
  ImageData thicknessTex;          float thicknessFactor = 0.0f;     // 0=薄壁
  float attenuationColor[3] = {1, 1, 1};
  float attenuationDistance = 0.0f;  // 0 哨兵 = spec 默认 +∞(无吸收)
};

/// 单个 mesh 的 CPU 数据。
struct MeshData {
  std::string name;
  /// 交错顶点:非蒙皮 pos3|normal3|tangent4|uv2(12 float,stride 48);
  /// 蒙皮 20 float(stride 80):追加 joints4f@48|weights4f@64。
  std::vector<float> vertices;
  std::vector<uint8_t> indices;   // 原始字节(indexType 决定位宽)
  IndexType indexType = IndexType::UInt16;
  uint32_t indexCount = 0;
  MaterialData material;
  bool skinned = false;           // 蒙皮网格(80B 布局)
  int32_t nodeIndex = -1;         // 所属 nodes[] 下标(无节点层级为 -1)
  // ---- morph targets(P4-C;增量 = target 主序 [dx,dy,dz]×vertexCount)----
  bool morph = false;                    // 有 ≥1 有效目标(管线选型用)
  std::vector<float> morphPosDeltas;     // 每目标连续(NORMAL 缺失的目标零行在纹理层补)
  std::vector<float> morphNormalDeltas;
  std::vector<float> morphWeights;       // 初始权重(glTF mesh.weights;截断同步)
  std::vector<std::string> morphTargetNames;  // extras.targetNames(元数据)
};

/// 层级节点(蒙皮模型用;非蒙皮模型 nodes 为空)。
struct AnimNodeData {
  int32_t parent = -1;
  float translation[3] = {0, 0, 0};
  float rotation[4] = {0, 0, 0, 1};   // quat xyzw
  float scale[3] = {1, 1, 1};
  int32_t mesh = -1;                  // 首个 primitive 的 meshes[] 下标
};

/// 蒙皮:关节表 + 逆绑定矩阵(16 float 列主序 ×N)。
struct SkinData {
  std::vector<int32_t> joints;        // nodes[] 下标
  std::vector<float> inverseBindMatrices;
  int32_t skeletonRoot = -1;
};

/// 动画通道:目标节点某属性的关键帧序列。
struct AnimChannelData {
  int32_t node = -1;
  int32_t path = 0;                   // 0=translation,1=rotation,2=scale
  std::vector<float> times;
  std::vector<float> values;          // vec3(t/s)或 quat(r)扁平序列
};

/// 动画 clip。
struct AnimClipData {
  std::string name;
  std::vector<AnimChannelData> channels;
  float duration = 0.0f;
};

/// 模型资产:一组 mesh + 包围球(全部 mesh 的 POSITION 合并)+ 层级/蒙皮/动画。
/// valid()==false 表示加载失败。
struct ModelAsset {
  std::vector<MeshData> meshes;
  float boundingCenter[3] = {0, 0, 0};
  float boundingRadius = 1.0f;
  std::vector<LightData> lights;    // KHR_lights_punctual(无则空;渲染层默认 1 方向光)
  std::vector<AnimNodeData> nodes;    // 层级节点(非蒙皮为空)
  std::vector<SkinData> skins;
  std::vector<AnimClipData> animations;
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
