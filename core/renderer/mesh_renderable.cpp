// MeshRenderable 的实现:逐 mesh 绑定管线/UBO 子区间/纹理/顶点索引缓冲并索引绘制。
#include "renderer/mesh_renderable.h"

namespace rd {

void MeshRenderable::record(CommandBuffer* cmd, BufferHandle sceneUBO, uint64_t uboOffset) {
  for (const auto& g : mesh_->meshes()) {
    cmd->bindPipeline(pipeline_);
    cmd->bindUniformBuffer(0, sceneUBO, uboOffset, 64);  // 本项的 mvp 子区间
    cmd->bindTexture(0, g.baseColorTex, mesh_->sampler());
    cmd->bindVertexBuffer(0, g.vbo, 0);
    cmd->bindIndexBuffer(g.ibo, 0, g.indexType);
    cmd->drawIndexed(g.indexCount, 0, 0);
  }
}

} // namespace rd
