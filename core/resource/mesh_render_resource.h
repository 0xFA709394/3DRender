/**
 * @file mesh_render_resource.h
 * @brief ModelAsset(CPU)→ 持久 GPU 资源集(device-local vbo/ibo + 纹理 + sampler)。
 * 由 scene 层或调用方以 shared_ptr 持有;Renderable 只引用不拥有。
 */
#pragma once
#include "resource/gltf_loader.h"
#include "rhi/rhi_device.h"
#include <memory>
#include <vector>

namespace rd {

/// 单个 mesh 的 GPU 资源 + CPU 侧材质参数(UBO 填充用)。
struct MeshGpuData {
  BufferHandle vbo;
  BufferHandle ibo;
  IndexType indexType = IndexType::UInt16;
  uint32_t indexCount = 0;
  MaterialData material;        // 材质参数(ImageData.pixels 上传后已清空,仅作元数据)
  TextureHandle baseColorTex;   // 缺省绑 1x1 白占位(采样 1×factor=factor)
  TextureHandle mrTex;          // 缺省绑 1x1 白
  TextureHandle normalTex;      // 缺省绑 1x1 平面法线(128,128,255)
  TextureHandle emissiveTex;    // 缺省绑 1x1 黑
  TextureHandle occlusionTex;   // 缺省绑 1x1 白
  bool skinned = false;         // 蒙皮网格(80B 顶点布局)
};

class MeshRenderResource {
public:
  /// 上传整个 ModelAsset;任一步失败记日志并返回 nullptr(已建资源自动清理)。
  static std::shared_ptr<MeshRenderResource> upload(Device& dev, const ModelAsset& model);
  const std::vector<MeshGpuData>& meshes() const { return meshes_; }
  SamplerHandle sampler() const { return sampler_; }

  /// 释放全部 GPU 资源(持有方在不再使用时调用;句柄 destroy 后本对象不可再用)。
  void destroy(Device& dev);

  /// 包围球(模型级,来自 ModelAsset;视锥剔除用)。
  const float* boundingCenter() const { return boundingCenter_; }
  float boundingRadius() const { return boundingRadius_; }

private:
  std::vector<MeshGpuData> meshes_;
  float boundingCenter_[3] = {0, 0, 0};
  float boundingRadius_ = 1.0f;
  SamplerHandle sampler_;
  TextureHandle fallbackWhite_;   // 1x1 白
  TextureHandle fallbackBlack_;   // 1x1 黑
  TextureHandle fallbackNormal_;  // 1x1 平面法线
};

} // namespace rd
