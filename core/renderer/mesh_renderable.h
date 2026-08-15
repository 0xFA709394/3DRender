/**
 * @file mesh_renderable.h
 * @brief MeshRenderable:引用持久 MeshRenderResource 的渲染项(只引用不拥有)。
 */
#pragma once
#include "renderer/renderable.h"
#include "resource/mesh_render_resource.h"
#include <memory>

namespace rd {

class MeshRenderable : public Renderable {
public:
  MeshRenderable(std::shared_ptr<MeshRenderResource> mesh, PipelineHandle pipeline)
      : mesh_(std::move(mesh)), pipeline_(pipeline) {}

  void record(CommandBuffer* cmd, BufferHandle sceneUBO, uint64_t uboOffset) override;

private:
  std::shared_ptr<MeshRenderResource> mesh_;
  PipelineHandle pipeline_;
};

} // namespace rd
