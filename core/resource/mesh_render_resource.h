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

/// 单个 mesh 的 GPU 资源。
struct MeshGpuData {
  BufferHandle vbo;
  BufferHandle ibo;
  IndexType indexType = IndexType::UInt16;
  uint32_t indexCount = 0;
  TextureHandle baseColorTex;   // 无纹理 mesh 指向 1x1 灰占位(与有纹理 mesh 统一绑定路径)
};

class MeshRenderResource {
public:
  /// 上传整个 ModelAsset;任一步失败记日志并返回 nullptr(已建资源自动清理)。
  static std::shared_ptr<MeshRenderResource> upload(Device& dev, const ModelAsset& model);
  const std::vector<MeshGpuData>& meshes() const { return meshes_; }
  SamplerHandle sampler() const { return sampler_; }

  /// 释放全部 GPU 资源(持有方在不再使用时调用;句柄 destroy 后本对象不可再用)。
  void destroy(Device& dev);

private:
  std::vector<MeshGpuData> meshes_;
  SamplerHandle sampler_;
  TextureHandle fallbackTex_;
};

} // namespace rd
