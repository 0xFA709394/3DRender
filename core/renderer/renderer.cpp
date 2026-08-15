// Renderer 的实现:unlit 管线 + 动态 UBO 帧流程。
#include "renderer/renderer.h"
#include "renderer/mesh_renderable.h"
#include "scene/camera.h"
#include "foundation/log.h"

namespace rd {

bool Renderer::init(Device& dev, const RendererShaderDesc& desc) {
  dev_ = &dev;
  vs_ = dev.createShaderModule({ShaderStage::Vertex, desc.vsCode, desc.entry});
  fs_ = dev.createShaderModule({ShaderStage::Fragment, desc.fsCode, desc.entry});

  PipelineDesc pd;
  pd.vertexShader = vs_;
  pd.fragmentShader = fs_;
  pd.vertexBindings = {{0, 48}};  // pos3|normal3|tangent4|uv2 交错
  pd.attributes = {{0, Format::R32G32B32_FLOAT, 0, 0},
                   {1, Format::R32G32B32_FLOAT, 12, 0},
                   {2, Format::R32G32B32A32_FLOAT, 24, 0},
                   {3, Format::R32G32_FLOAT, 40, 0}};
  pd.cullMode = CullMode::None;   // 2a 保守不剔除(绕序约定待 2b 定)
  pd.depthTest = true;
  pd.depthWrite = true;
  pd.colorFormat = desc.colorFormat;
  pipeline_ = dev.createPipeline(pd);

  sceneUBO_ = dev.createBuffer({uint64_t(kUboStride) * kMaxItems, BufferUsage::Uniform, true,
                                false, nullptr});
  if (!vs_.valid() || !fs_.valid() || !pipeline_.valid() || !sceneUBO_.valid()) {
    shutdown();
    return false;
  }
  return true;
}

void Renderer::shutdown() {
  if (!dev_) return;
  if (pipeline_.valid()) dev_->destroyPipeline(pipeline_);
  if (vs_.valid()) dev_->destroyShaderModule(vs_);
  if (fs_.valid()) dev_->destroyShaderModule(fs_);
  if (sceneUBO_.valid()) dev_->destroyBuffer(sceneUBO_);
  pipeline_ = {};
  vs_ = {};
  fs_ = {};
  sceneUBO_ = {};
  dev_ = nullptr;
}

void Renderer::beginScene(const scene::Camera& camera, const ClearColor& clear) {
  viewProj_ = camera.projMatrix() * camera.viewMatrix();
  clear_ = clear;
}

void Renderer::submit(const std::shared_ptr<MeshRenderResource>& mesh,
                      const math::Mat4& world) {
  if (queue_.size() >= kMaxItems) {
    RD_LOGW("renderer", "渲染项超出 %u,截断", kMaxItems);
    return;
  }
  queue_.push_back(std::make_unique<MeshRenderable>(mesh, pipeline_));
  worldStack_.push_back(world);
}

void Renderer::endScene(CommandBuffer* cmd, TargetHandle target) {
  // 统一填充 per-item mvp(viewProj × world)
  const uint32_t count = uint32_t(queue_.size());
  for (uint32_t i = 0; i < count; ++i) {
    math::Mat4 mvp = viewProj_ * worldStack_[i];
    dev_->updateBuffer(sceneUBO_, &mvp, sizeof(mvp), uint64_t(i) * kUboStride);
  }
  cmd->beginRenderPass(target, clear_);
  for (uint32_t i = 0; i < count; ++i) {
    queue_[i]->prepass(cmd);  // 2b 钩子(默认空)
    queue_[i]->record(cmd, sceneUBO_, uint64_t(i) * kUboStride);
  }
  cmd->endRenderPass();
  queue_.clear();
  worldStack_.clear();
}

} // namespace rd
