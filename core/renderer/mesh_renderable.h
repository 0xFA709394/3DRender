/**
 * @file mesh_renderable.h
 * @brief MeshRenderable:引用持久 MeshRenderResource 的渲染项(只引用不拥有)。
 * 按材质 unlit 标记选择管线;pbr 路径绑定全材质纹理 + 环境纹理。
 */
#pragma once
#include "renderer/renderable.h"
#include "resource/mesh_render_resource.h"
#include <memory>

namespace rd {

class MeshRenderable : public Renderable {
public:
  /// 管线在 record 时从 RenderContext 取(按 SceneTarget 格式/采样数匹配)。
  explicit MeshRenderable(std::shared_ptr<MeshRenderResource> mesh)
      : mesh_(std::move(mesh)) {}

  void record(CommandBuffer* cmd, const RenderContext& ctx) override;

  /// 网格数据访问(Renderer 填 ItemUBO 用)。
  const std::vector<MeshGpuData>& meshData() const { return mesh_->meshes(); }

private:
  std::shared_ptr<MeshRenderResource> mesh_;
};

} // namespace rd
