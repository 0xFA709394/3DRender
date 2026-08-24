// MeshRenderable 的实现:逐 mesh 选管线(unlit/pbr)、绑 UBO 与全纹理、索引绘制。
#include "renderer/mesh_renderable.h"
#include "renderer/environment.h"

namespace rd {

void MeshRenderable::record(CommandBuffer* cmd, const RenderContext& ctx) {
  const bool skinned = !mesh_->meshes().empty() && mesh_->meshes()[0].skinned;
  if (ctx.shadowPass) {  // 阴影:只写深度(位置语义;mask 材质采样 baseColor discard)
    const bool mask0 = !mesh_->meshes().empty() && mesh_->meshes()[0].material.alphaCutoff > 0.0f;
    const PipelineHandle pipe =
        skinned ? ctx.skinnedShadowPipe : (mask0 ? ctx.shadowMaskPipe : ctx.shadowPipe);
    cmd->bindPipeline(pipe);
    cmd->bindUniformBuffer(0, ctx.lightUbo, 0, 64);  // lightViewProj(LightUBO 前 64B)
    cmd->bindUniformBuffer(1, ctx.itemUbo, ctx.itemOffset, 256);
    if (skinned) cmd->bindUniformBuffer(3, ctx.jointUbo, ctx.jointOffset, 8192);
    for (const auto& g : mesh_->meshes()) {
      if (mask0 && !skinned)
        cmd->bindTexture(0, g.baseColorTex, mesh_->sampler());  // cutout alpha
      cmd->bindVertexBuffer(0, g.vbo, 0);
      cmd->bindIndexBuffer(g.ibo, 0, g.indexType);
      cmd->drawIndexed(g.indexCount, 0, 0);
    }
    return;
  }
  for (const auto& g : mesh_->meshes()) {
    if (g.material.alphaBlend && !g.skinned) {  // blend 路径(skinned 按 opaque)
      cmd->bindPipeline(ctx.blendPipeline);
      cmd->bindUniformBuffer(0, ctx.frameUbo, 0, 256);
      cmd->bindUniformBuffer(1, ctx.itemUbo, ctx.itemOffset, 256);
      cmd->bindUniformBuffer(2, ctx.lightUbo, 0, 352);
      cmd->bindTexture(0, g.baseColorTex, mesh_->sampler());
      cmd->bindTexture(1, g.mrTex, mesh_->sampler());
      cmd->bindTexture(2, g.normalTex, mesh_->sampler());
      cmd->bindTexture(3, g.emissiveTex, mesh_->sampler());
      cmd->bindTexture(4, g.occlusionTex, mesh_->sampler());
      if (ctx.env) {
        cmd->bindTexture(5, ctx.env->prefilterCube(), ctx.env->cubeSampler());
        cmd->bindTexture(6, ctx.env->brdfLut(), ctx.env->lutSampler());
      }
      if (ctx.shadowMap.valid()) cmd->bindTexture(7, ctx.shadowMap, ctx.shadowSampler);
      cmd->bindVertexBuffer(0, g.vbo, 0);
      cmd->bindIndexBuffer(g.ibo, 0, g.indexType);
      cmd->drawIndexed(g.indexCount, 0, 0);
      continue;
    }
    if (g.skinned) {
      // 蒙皮 PBR 路径(skinned 恒 PBR;unlit+蒙皮组合不支持)
      cmd->bindPipeline(ctx.skinnedPipeline);
      cmd->bindUniformBuffer(0, ctx.frameUbo, 0, 256);
      cmd->bindUniformBuffer(1, ctx.itemUbo, ctx.itemOffset, 256);
      cmd->bindUniformBuffer(2, ctx.lightUbo, 0, 352);
      cmd->bindUniformBuffer(3, ctx.jointUbo, ctx.jointOffset, 8192);
      cmd->bindTexture(0, g.baseColorTex, mesh_->sampler());
      cmd->bindTexture(1, g.mrTex, mesh_->sampler());
      cmd->bindTexture(2, g.normalTex, mesh_->sampler());
      cmd->bindTexture(3, g.emissiveTex, mesh_->sampler());
      cmd->bindTexture(4, g.occlusionTex, mesh_->sampler());
      if (ctx.env) {
        cmd->bindTexture(5, ctx.env->prefilterCube(), ctx.env->cubeSampler());
        cmd->bindTexture(6, ctx.env->brdfLut(), ctx.env->lutSampler());
      }
      if (ctx.shadowMap.valid()) cmd->bindTexture(7, ctx.shadowMap, ctx.shadowSampler);
    } else if (g.material.unlit) {
      cmd->bindPipeline(ctx.unlitPipeline);
      cmd->bindUniformBuffer(1, ctx.itemUbo, ctx.itemOffset, 64);  // ItemUBO 前 64B=mvp
      cmd->bindTexture(0, g.baseColorTex, mesh_->sampler());
    } else {
      cmd->bindPipeline(ctx.pbrPipeline);
      cmd->bindUniformBuffer(0, ctx.frameUbo, 0, 256);  // FrameUBO
      cmd->bindUniformBuffer(1, ctx.itemUbo, ctx.itemOffset, 256);  // ItemUBO
      cmd->bindUniformBuffer(2, ctx.lightUbo, 0, 352);  // LightUBO(多光源+阴影)
      cmd->bindTexture(0, g.baseColorTex, mesh_->sampler());
      cmd->bindTexture(1, g.mrTex, mesh_->sampler());
      cmd->bindTexture(2, g.normalTex, mesh_->sampler());
      cmd->bindTexture(3, g.emissiveTex, mesh_->sampler());
      cmd->bindTexture(4, g.occlusionTex, mesh_->sampler());
      if (ctx.env) {
        cmd->bindTexture(5, ctx.env->prefilterCube(), ctx.env->cubeSampler());
        cmd->bindTexture(6, ctx.env->brdfLut(), ctx.env->lutSampler());
      }
      if (ctx.shadowMap.valid()) cmd->bindTexture(7, ctx.shadowMap, ctx.shadowSampler);
    }
    cmd->bindVertexBuffer(0, g.vbo, 0);
    cmd->bindIndexBuffer(g.ibo, 0, g.indexType);
    cmd->drawIndexed(g.indexCount, 0, 0);
  }
}

} // namespace rd
