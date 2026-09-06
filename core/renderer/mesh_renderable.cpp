// MeshRenderable 的实现:逐 mesh 选管线(unlit/pbr)、绑 UBO 与全纹理、索引绘制。
#include "renderer/mesh_renderable.h"
#include "renderer/environment.h"

namespace rd {

void MeshRenderable::record(CommandBuffer* cmd, const RenderContext& ctx) {
  const bool skinned = !mesh_->meshes().empty() && mesh_->meshes()[0].skinned;
  // per-mesh ItemUBO 槽:itemOffset + meshIdx×512(容量截断时钳到末槽=mesh0 兜底语义)
  const uint64_t kLastSlot = uint64_t(kItemUboMaxSlots - 1) * kItemUboStride;
  uint32_t meshIdx = 0;
  auto itemOff = [&]() {
    const uint64_t o = ctx.itemOffset + uint64_t(meshIdx) * kItemUboStride;
    return o < kLastSlot ? o : kLastSlot;
  };
  // KHR 扩展材质纹理(9..15;pbr/blend/skinned 三路径共用,恒绑定)
  auto bindExtTextures = [&](const MeshGpuData& g) {
    cmd->bindTexture(9, g.clearcoatTex, mesh_->sampler());
    cmd->bindTexture(10, g.clearcoatRoughTex, mesh_->sampler());
    cmd->bindTexture(11, g.clearcoatNormalTex, mesh_->sampler());
    cmd->bindTexture(12, g.sheenColorTex, mesh_->sampler());
    cmd->bindTexture(13, g.sheenRoughTex, mesh_->sampler());
    cmd->bindTexture(14, g.specularColorTex, mesh_->sampler());
    cmd->bindTexture(15, g.specularTex, mesh_->sampler());
    cmd->bindTexture(17, g.transmissionTex, mesh_->sampler());  // R=透射强度
    cmd->bindTexture(18, g.thicknessTex, mesh_->sampler());     // G=厚度
  };
  if (ctx.shadowPass) {  // 阴影:只写深度(位置语义;mask 材质采样 baseColor discard)
    const bool mask0 = !mesh_->meshes().empty() && mesh_->meshes()[0].material.alphaCutoff > 0.0f;
    const bool morph0 = !mesh_->meshes().empty() && mesh_->meshes()[0].morph;
    // morph 阴影优先于 skinned(组合模型两者兼用);mask+morph → morph(不裁剪)
    const PipelineHandle pipe =
        morph0 ? (skinned ? ctx.morphSkinnedShadowPipe : ctx.morphShadowPipe)
               : skinned ? ctx.skinnedShadowPipe
                         : (mask0 ? ctx.shadowMaskPipe : ctx.shadowPipe);
    cmd->bindPipeline(pipe);
    cmd->bindUniformBuffer(0, ctx.lightUbo, ctx.lightUboOffset, 64);  // dir=0/spot=64
    cmd->bindUniformBuffer(1, ctx.itemUbo, itemOff(), kItemUboSize);
    if (skinned) cmd->bindUniformBuffer(3, ctx.jointUbo, ctx.jointOffset, 8192);
    for (const auto& g : mesh_->meshes()) {
      if (mask0 && !skinned && !morph0)
        cmd->bindTexture(0, g.baseColorTex, mesh_->sampler());  // cutout alpha
      if (morph0 && g.morphTex.valid())
        cmd->bindTexture(19, g.morphTex, mesh_->sampler());  // slot19 texMorph
      cmd->bindVertexBuffer(0, g.vbo, 0);
      cmd->bindIndexBuffer(g.ibo, 0, g.indexType);
      cmd->drawIndexed(g.indexCount, 0, 0);
      ++meshIdx;
    }
    return;
  }
  meshIdx = 0;
  for (const auto& g : mesh_->meshes()) {
    if (g.material.alphaBlend && !g.skinned) {  // blend 路径(skinned 按 opaque)
      cmd->bindPipeline(ctx.blendPipeline);  // morph+blend 不支持(退 blend)
      cmd->bindUniformBuffer(0, ctx.frameUbo, 0, 272);
      cmd->bindUniformBuffer(1, ctx.itemUbo, itemOff(), kItemUboSize);
      cmd->bindUniformBuffer(2, ctx.lightUbo, 0, 432);  // LightUBO(432B)
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
      if (ctx.shadowSpotMap.valid())
        cmd->bindTexture(8, ctx.shadowSpotMap, ctx.shadowSampler);
      bindExtTextures(g);
      if (ctx.transSceneTex.valid())
        cmd->bindTexture(16, ctx.transSceneTex, ctx.transSampler);
      cmd->bindVertexBuffer(0, g.vbo, 0);
      cmd->bindIndexBuffer(g.ibo, 0, g.indexType);
      cmd->drawIndexed(g.indexCount, 0, 0);
      ++meshIdx;
      continue;
    }
    if (g.skinned) {
      // 蒙皮 PBR 路径(skinned 恒 PBR;unlit+蒙皮组合不支持)
      cmd->bindPipeline(g.morph ? ctx.morphSkinnedPipeline : ctx.skinnedPipeline);
      cmd->bindUniformBuffer(0, ctx.frameUbo, 0, 272);
      cmd->bindUniformBuffer(1, ctx.itemUbo, itemOff(), kItemUboSize);
      cmd->bindUniformBuffer(2, ctx.lightUbo, 0, 432);  // LightUBO(432B)
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
      if (ctx.shadowSpotMap.valid())
        cmd->bindTexture(8, ctx.shadowSpotMap, ctx.shadowSampler);
      bindExtTextures(g);
      if (g.morphTex.valid())
        cmd->bindTexture(19, g.morphTex, mesh_->sampler());
      if (ctx.transSceneTex.valid())
        cmd->bindTexture(16, ctx.transSceneTex, ctx.transSampler);
    } else if (g.material.unlit) {
      cmd->bindPipeline(ctx.unlitPipeline);
      cmd->bindUniformBuffer(1, ctx.itemUbo, itemOff(), 64);  // ItemUBO 前 64B=mvp
      cmd->bindTexture(0, g.baseColorTex, mesh_->sampler());
    } else {
      cmd->bindPipeline(g.morph ? ctx.morphPipeline : ctx.pbrPipeline);
      cmd->bindUniformBuffer(0, ctx.frameUbo, 0, 272);  // FrameUBO
      cmd->bindUniformBuffer(1, ctx.itemUbo, itemOff(), kItemUboSize);  // ItemUBO
      cmd->bindUniformBuffer(2, ctx.lightUbo, 0, 432);  // LightUBO(432B,多光源+阴影)
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
      if (ctx.shadowSpotMap.valid())
        cmd->bindTexture(8, ctx.shadowSpotMap, ctx.shadowSampler);
      bindExtTextures(g);
      if (g.morphTex.valid())
        cmd->bindTexture(19, g.morphTex, mesh_->sampler());
      if (ctx.transSceneTex.valid())
        cmd->bindTexture(16, ctx.transSceneTex, ctx.transSampler);
    }
    cmd->bindVertexBuffer(0, g.vbo, 0);
    cmd->bindIndexBuffer(g.ibo, 0, g.indexType);
    cmd->drawIndexed(g.indexCount, 0, 0);
    ++meshIdx;
  }
}

} // namespace rd
