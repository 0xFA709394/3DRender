// Renderer 的实现:双管线 + 环境 + 双层 UBO 帧流程。
#include "renderer/renderer.h"
#include "renderer/mesh_renderable.h"
#include "scene/camera.h"
#include "foundation/log.h"
#include <algorithm>
#include <glm/glm.hpp>

namespace rd {
namespace {

/// 顶点布局(48B 交错):pos3@0|normal3@12|tangent4@24|uv2@40
void fillVertexLayout(PipelineDesc& pd) {
  pd.vertexBindings = {{0, 48}};
  pd.attributes = {{0, Format::R32G32B32_FLOAT, 0, 0},
                   {1, Format::R32G32B32_FLOAT, 12, 0},
                   {2, Format::R32G32B32A32_FLOAT, 24, 0},
                   {3, Format::R32G32_FLOAT, 40, 0}};
}

/// ItemUBO 布局(256B):mvp|world|normalMatrix|baseColorFactor|emissiveOcc|metalRough|uvTf
struct ItemUBOData {
  math::Mat4 mvp;
  math::Mat4 world;
  math::Mat4 normalMatrix;
  float baseColorFactor[4];
  float emissiveOcc[4];     // rgb=emissiveFactor, a=occlusionStrength
  float metallicRough[4];   // x=metallic, y=roughness, z=normalScale
  float uvTransform[4];     // xy=offset, zw=scale
};
static_assert(sizeof(ItemUBOData) == 256, "ItemUBO 必须 256B");

} // namespace

bool Renderer::init(Device& dev, const RendererShaderDesc& desc) {
  dev_ = &dev;
  colorFormat_ = desc.colorFormat;
  entry_ = desc.entry;
  pfVsCode_ = desc.prefilterVs;
  pfFsCode_ = desc.prefilterFs;

  // unlit 管线(shader 模块用完即销毁)
  auto uvs = dev.createShaderModule({ShaderStage::Vertex, desc.unlitVs, desc.entry});
  auto ufs = dev.createShaderModule({ShaderStage::Fragment, desc.unlitFs, desc.entry});
  PipelineDesc upd;
  upd.vertexShader = uvs;
  upd.fragmentShader = ufs;
  fillVertexLayout(upd);
  upd.cullMode = CullMode::None;
  upd.depthTest = true;
  upd.depthWrite = true;
  upd.colorFormat = desc.colorFormat;
  unlitPipeline_ = dev.createPipeline(upd);
  dev.destroyShaderModule(uvs);
  dev.destroyShaderModule(ufs);

  // pbr 管线(模块持有到 shutdown,供缓存 key 复用语义一致)
  vs_ = dev.createShaderModule({ShaderStage::Vertex, desc.pbrVs, desc.entry});
  fs_ = dev.createShaderModule({ShaderStage::Fragment, desc.pbrFs, desc.entry});
  PipelineDesc ppd;
  ppd.vertexShader = vs_;
  ppd.fragmentShader = fs_;
  fillVertexLayout(ppd);
  ppd.cullMode = CullMode::None;
  ppd.depthTest = true;
  ppd.depthWrite = true;
  ppd.colorFormat = desc.colorFormat;
  pbrPipeline_ = dev.createPipeline(ppd);

  // 双层 UBO
  frameUbo_ = dev.createBuffer({256, BufferUsage::Uniform, true, false, nullptr});
  itemUbo_ = dev.createBuffer({uint64_t(kUboStride) * kMaxItems, BufferUsage::Uniform, true,
                               false, nullptr});

  // blit 管线(无顶点缓冲:gl_VertexIndex 全屏三角形)
  auto bvs = dev.createShaderModule({ShaderStage::Vertex, desc.blitVs, desc.entry});
  auto bfs = dev.createShaderModule({ShaderStage::Fragment, desc.blitFs, desc.entry});
  PipelineDesc bpd;
  bpd.vertexShader = bvs;
  bpd.fragmentShader = bfs;
  bpd.cullMode = CullMode::None;
  bpd.colorFormat = desc.colorFormat;
  blitPipeline_ = dev.createPipeline(bpd);
  dev.destroyShaderModule(bvs);
  dev.destroyShaderModule(bfs);
  blitUbo_ = dev.createBuffer({16, BufferUsage::Uniform, true, false, nullptr});
  const float vflip = dev.backend() == Backend::GLES ? 1.0f : 0.0f;
  const float params[4] = {vflip, 0.0f, 0.0f, 0.0f};
  if (blitUbo_.valid()) dev.updateBuffer(blitUbo_, params, sizeof(params), 0);
  SamplerDesc bsd;
  bsd.wrapU = WrapMode::Clamp;
  bsd.wrapV = WrapMode::Clamp;
  blitSampler_ = dev.createSampler(bsd);

  // 环境(SH/LUT/GPU 预滤波,init 期一次性;尺寸/级数按画质档,默认现状 64/5)
  const bool envOk = env_.build(dev, desc.prefilterVs, desc.prefilterFs, desc.entry,
                                desc.colorFormat, iblSize_, iblMips_);

  if (!unlitPipeline_.valid() || !pbrPipeline_.valid() || !frameUbo_.valid() ||
      !itemUbo_.valid() || !blitPipeline_.valid() || !blitUbo_.valid() ||
      !blitSampler_.valid() || !envOk) {
    shutdown();
    return false;
  }
  return true;
}

void Renderer::shutdown() {
  if (!dev_) return;
  env_.destroy(*dev_);
  if (sceneTarget_.valid()) dev_->destroyTarget(sceneTarget_);
  if (blitPipeline_.valid()) dev_->destroyPipeline(blitPipeline_);
  if (blitUbo_.valid()) dev_->destroyBuffer(blitUbo_);
  if (blitSampler_.valid()) dev_->destroySampler(blitSampler_);
  if (unlitPipeline_.valid()) dev_->destroyPipeline(unlitPipeline_);
  if (pbrPipeline_.valid()) dev_->destroyPipeline(pbrPipeline_);
  if (vs_.valid()) dev_->destroyShaderModule(vs_);
  if (fs_.valid()) dev_->destroyShaderModule(fs_);
  if (frameUbo_.valid()) dev_->destroyBuffer(frameUbo_);
  if (itemUbo_.valid()) dev_->destroyBuffer(itemUbo_);
  sceneTarget_ = {};
  blitPipeline_ = {};
  blitUbo_ = {};
  blitSampler_ = {};
  unlitPipeline_ = {};
  pbrPipeline_ = {};
  vs_ = {};
  fs_ = {};
  frameUbo_ = {};
  itemUbo_ = {};
  sceneW_ = sceneH_ = sceneSamples_ = 0;
  dev_ = nullptr;
}

TargetHandle Renderer::ensureSceneTarget(uint32_t targetW, uint32_t targetH) {
  const uint32_t w = std::max(1u, uint32_t(float(targetW) * renderScale_));
  const uint32_t h = std::max(1u, uint32_t(float(targetH) * renderScale_));
  const uint32_t capMsaa = dev_->caps().get(Capability::msaa);
  const uint32_t samples = std::max(1u, std::min(msaa_, capMsaa));
  if (sceneTarget_.valid() && w == sceneW_ && h == sceneH_ && samples == sceneSamples_)
    return sceneTarget_;
  if (sceneTarget_.valid()) dev_->destroyTarget(sceneTarget_);
  OffscreenTargetDesc td;
  td.width = w;
  td.height = h;
  td.depth = true;
  td.sampleCount = samples;
  td.colorFormat = colorFormat_;
  sceneTarget_ = dev_->createOffscreenTarget(td);
  if (!sceneTarget_.valid()) {
    RD_LOGE("renderer", "SceneTarget 创建失败(%ux%u samples=%u)", w, h, samples);
  }
  sceneW_ = w;
  sceneH_ = h;
  sceneSamples_ = samples;
  return sceneTarget_;
}

void Renderer::beginScene(const scene::Camera& camera, const ClearColor& clear) {
  viewProj_ = camera.projMatrix() * camera.viewMatrix();
  clear_ = clear;

  // FrameUBO:viewProj|cameraPos|lightDir|lightColor|sh[9×vec4]
  struct {
    math::Mat4 viewProj;
    math::Vec4 cameraPos;
    math::Vec4 lightDir;
    math::Vec4 lightColor;
    float sh[9][4];
  } fu;
  static_assert(sizeof(fu) == 256, "FrameUBO 必须 256B");
  fu.viewProj = viewProj_;
  const auto& eye = camera.eye();
  fu.cameraPos = math::Vec4(eye, 1.0f);
  // 默认光照:与程序化环境柔光箱 1 同向
  fu.lightDir = math::Vec4(glm::normalize(math::Vec3(-0.5f, 0.8f, 0.3f)), 0.0f);
  fu.lightColor = math::Vec4(0.8f, 0.75f, 0.7f, 1.0f);
  const float* sh = env_.sh();
  for (int i = 0; i < 9; ++i) {
    fu.sh[i][0] = sh[i * 3 + 0];
    fu.sh[i][1] = sh[i * 3 + 1];
    fu.sh[i][2] = sh[i * 3 + 2];
    fu.sh[i][3] = 0.0f;
  }
  dev_->updateBuffer(frameUbo_, &fu, sizeof(fu), 0);
}

void Renderer::submit(const std::shared_ptr<MeshRenderResource>& mesh,
                      const math::Mat4& world) {
  if (queue_.size() >= kMaxItems) {
    RD_LOGW("renderer", "渲染项超出 %u,截断", kMaxItems);
    return;
  }
  queue_.push_back(
      std::make_unique<MeshRenderable>(mesh, pbrPipeline_, unlitPipeline_));
  worldStack_.push_back(world);
}

void Renderer::endScene(CommandBuffer* cmd, TargetHandle target) {
  // 统一填充 per-item ItemUBO
  const uint32_t count = uint32_t(queue_.size());
  for (uint32_t i = 0; i < count; ++i) {
    const auto* renderable = static_cast<const MeshRenderable*>(queue_[i].get());
    const auto& meshes = renderable->meshData();
    // ItemUBO 按 mesh 首个材质填(多 mesh 模型共享 item 槽——简化:逐 item 一个 UBO,
    // 多 mesh 差异材质归 P2 拆 item)
    ItemUBOData iu{};
    const auto& world = worldStack_[i];
    iu.mvp = viewProj_ * world;
    iu.world = world;
    iu.normalMatrix = glm::transpose(glm::inverse(world));
    if (!meshes.empty()) {
      const auto& m = meshes[0].material;
      memcpy(iu.baseColorFactor, m.baseColorFactor, sizeof(iu.baseColorFactor));
      iu.emissiveOcc[0] = m.emissiveFactor[0];
      iu.emissiveOcc[1] = m.emissiveFactor[1];
      iu.emissiveOcc[2] = m.emissiveFactor[2];
      iu.emissiveOcc[3] = m.occlusionStrength;
      iu.metallicRough[0] = m.metallicFactor;
      iu.metallicRough[1] = m.roughnessFactor;
      iu.metallicRough[2] = m.normalScale;
      iu.uvTransform[0] = m.uvOffset[0];
      iu.uvTransform[1] = m.uvOffset[1];
      iu.uvTransform[2] = m.uvScale[0];
      iu.uvTransform[3] = m.uvScale[1];
    } else {
      iu.baseColorFactor[0] = iu.baseColorFactor[1] = iu.baseColorFactor[2] =
          iu.baseColorFactor[3] = 1.0f;
      iu.emissiveOcc[3] = 1.0f;
      iu.metallicRough[0] = 1.0f;
      iu.metallicRough[1] = 1.0f;
      iu.metallicRough[2] = 1.0f;
      iu.uvTransform[2] = iu.uvTransform[3] = 1.0f;
    }
    dev_->updateBuffer(itemUbo_, &iu, sizeof(iu), uint64_t(i) * kUboStride);
  }

  // 上屏链:场景 → 内部 SceneTarget(分辨率缩放/MSAA 按画质档)→ blit upscale → 最终目标
  uint32_t tw = 0, th = 0;
  dev_->targetSize(target, tw, th);
  TargetHandle scene = ensureSceneTarget(tw, th);
  if (!scene.valid()) {  // 场景目标失败:退化为直接渲染到最终目标
    scene = target;
  }
  cmd->beginRenderPass(scene, clear_);
  RenderContext ctx;
  ctx.frameUbo = frameUbo_;
  ctx.itemUbo = itemUbo_;
  ctx.env = &env_;
  for (uint32_t i = 0; i < count; ++i) {
    queue_[i]->prepass(cmd);  // 2b 钩子(默认空)
    ctx.itemOffset = uint64_t(i) * kUboStride;
    queue_[i]->record(cmd, ctx);
  }
  cmd->endRenderPass();
  if (scene != target) {  // upscale pass(P2 后处理链挂载点)
    cmd->beginRenderPass(target, clear_);
    cmd->bindPipeline(blitPipeline_);
    cmd->bindUniformBuffer(0, blitUbo_, 0, 16);
    cmd->bindTexture(0, dev_->targetColorTexture(scene), blitSampler_);
    cmd->draw(3, 0);
    cmd->endRenderPass();
  }
  queue_.clear();
  worldStack_.clear();
}

} // namespace rd
