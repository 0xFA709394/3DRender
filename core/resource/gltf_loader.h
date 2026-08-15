/**
 * @file gltf_loader.h
 * @brief glTF 2.0 加载(cgltf):glb/gltf → ModelAsset(纯 CPU 数据,不碰 GPU)。
 *
 * 顶点统一规整为交错布局 pos(3f)|normal(3f)|uv(2f)(stride 32 字节);
 * 缺失属性(normal/uv)补 0。索引自适应 u16/u32(>65535 或源为 u32 时用 UInt32)。
 * 内嵌纹理图像解码为 RGBA8;解码失败返回无效 ImageData(调用方决定占位策略)。
 */
#pragma once
#include "resource/image_codec.h"
#include "rhi/rhi_types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace rd {

/// 单个 mesh 的 CPU 数据。
struct MeshData {
  std::string name;
  std::vector<float> vertices;    // 交错 pos3|normal3|uv2
  std::vector<uint8_t> indices;   // 原始字节(indexType 决定位宽)
  IndexType indexType = IndexType::UInt16;
  uint32_t indexCount = 0;
  ImageData baseColor;            // 内嵌 baseColor 纹理;无效(width==0)表示无
};

/// 模型资产:一组 mesh。valid()==false 表示加载失败。
struct ModelAsset {
  std::vector<MeshData> meshes;
  bool valid() const { return !meshes.empty(); }
};

/// 加载 glb/gltf 文件;失败(不存在/解析错/无 mesh)返回空 ModelAsset 并记日志。
ModelAsset loadGltf(const char* path);

} // namespace rd
