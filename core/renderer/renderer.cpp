// Renderer 的实现:双管线 + 环境 + 双层 UBO 帧流程。
#include "renderer/renderer.h"
#include "renderer/light_ubo.h"
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
  eqFsCode_ = desc.equirectFs;

  // unlit 管线(shader 模块持有,场景管线随 SceneTarget 重建用)
  uvs_ = dev.createShaderModule({ShaderStage::Vertex, desc.unlitVs, desc.entry});
  ufs_ = dev.createShaderModule({ShaderStage::Fragment, desc.unlitFs, desc.entry});
  PipelineDesc upd;
  upd.vertexShader = uvs_;
  upd.fragmentShader = ufs_;
  fillVertexLayout(upd);
  upd.cullMode = CullMode::None;
  upd.depthTest = true;
  upd.depthWrite = true;
  upd.colorFormat = desc.colorFormat;
  unlitPipeline_ = dev.createPipeline(upd);

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

  // 蒙皮 shader 模块(管线随 SceneTarget 重建时复用)
  skvs_ = dev.createShaderModule({ShaderStage::Vertex, desc.skinnedVs, desc.entry});
  sdsvs_ = dev.createShaderModule({ShaderStage::Vertex, desc.skinnedShadowVs, desc.entry});
  jointUbo_ = dev.createBuffer({uint64_t(kJointItemStride) * kMaxJointItems,
                                BufferUsage::Uniform, true, false, nullptr});

  // 环境(SH/LUT/GPU 预滤波,init 期一次性;尺寸/级数按画质档,默认现状 64/5)
  const bool envOk = env_.build(dev, desc.prefilterVs, desc.prefilterFs, desc.equirectFs,
                                desc.entry, desc.colorFormat, iblSize_, iblMips_);

  // 多光源 + 阴影资源
  lightUbo_ = dev.createBuffer({352, BufferUsage::Uniform, true, false, nullptr});
  auto svs = dev.createShaderModule({ShaderStage::Vertex, desc.shadowVs, desc.entry});
  auto sfs = dev.createShaderModule({ShaderStage::Fragment, desc.shadowFs, desc.entry});
  sfs_ = sfs;  // 持有(蒙皮阴影管线重建用;shutdown 释放)
  PipelineDesc spd;
  spd.vertexShader = svs;
  spd.fragmentShader = sfs;
  fillVertexLayout(spd);
  spd.cullMode = CullMode::None;
  spd.depthTest = true;
  spd.depthWrite = true;
  spd.depthOnly = true;
  shadowPipeline_ = dev.createPipeline(spd);
  dev.destroyShaderModule(svs);
  SamplerDesc csd;
  csd.compareEnable = true;
  csd.wrapU = WrapMode::Clamp;
  csd.wrapV = WrapMode::Clamp;
  shadowSampler_ = dev.createSampler(csd);
  {
    // 1x1 D32=1.0 占位(无阴影时绑定,阴影采样恒受光);
    // 经"清屏初始化"(D32 不允许带初始数据上传:Metal iOS 禁 Shared/CPU 写 Private)
    TextureDesc ftd;
    ftd.width = 1;
    ftd.height = 1;
    ftd.format = Format::D32_FLOAT;
    ftd.usage = TextureUsage::Sampled | TextureUsage::RenderTargetAttachment;
    shadowFallbackTex_ = dev.createTexture(ftd);
    OffscreenTargetDesc fod;
    fod.width = 1;
    fod.height = 1;
    fod.depthFromTexture = shadowFallbackTex_;
    auto ft = dev.createOffscreenTarget(fod);
    if (shadowFallbackTex_.valid() && ft.valid()) {
      auto* c = dev.acquireCommandBuffer();
      c->beginRenderPass(ft, {0, 0, 0, 1, 1.0f});  // clear.depth=1 → 恒受光
      c->endRenderPass();
      dev.submit(c);
      dev.waitIdle();
      dev.destroyTarget(ft);
    }
  }

  // PostChain 管线(vert 复用 blit;extract/blur 输出 R16F,composite/fxaa 输出目标格式)
  auto mkPostPipe = [&](const std::vector<uint8_t>& fsCode, Format fmt) {
    auto vs = dev.createShaderModule({ShaderStage::Vertex, desc.blitVs, desc.entry});
    auto fs = dev.createShaderModule({ShaderStage::Fragment, fsCode, desc.entry});
    PipelineDesc pd;
    pd.vertexShader = vs;
    pd.fragmentShader = fs;
    pd.cullMode = CullMode::None;
    pd.colorFormat = fmt;
    auto p = dev.createPipeline(pd);
    dev.destroyShaderModule(vs);
    dev.destroyShaderModule(fs);
    return p;
  };
  extractPipeline_ = mkPostPipe(desc.extractFs, Format::R16G16B16A16_FLOAT);
  blurPipeline_ = mkPostPipe(desc.blurFs, Format::R16G16B16A16_FLOAT);
  compositePipeline_ = mkPostPipe(desc.compositeFs, desc.colorFormat);
  fxaaPipeline_ = mkPostPipe(desc.fxaaFs, desc.colorFormat);
  // 参数 UBO(各 16B;x=vFlip,texel 在 ensurePostTargets/ensureFxaaTarget 更新)
  const float vf = dev.backend() == Backend::GLES ? 1.0f : 0.0f;
  const float zp[4] = {vf, 0.0f, 0.0f, 0.0f};
  blurUbo1_ = dev.createBuffer({16, BufferUsage::Uniform, true, false, nullptr});
  blurUbo2_ = dev.createBuffer({16, BufferUsage::Uniform, true, false, nullptr});
  blurUbo3_ = dev.createBuffer({16, BufferUsage::Uniform, true, false, nullptr});
  fxaaUbo_ = dev.createBuffer({16, BufferUsage::Uniform, true, false, nullptr});
  compositeUbo_ = dev.createBuffer({16, BufferUsage::Uniform, true, false, nullptr});
  for (auto u : {blurUbo1_, blurUbo2_, blurUbo3_, fxaaUbo_, compositeUbo_})
    if (u.valid()) dev.updateBuffer(u, zp, sizeof(zp), 0);

  if (!unlitPipeline_.valid() || !pbrPipeline_.valid() || !frameUbo_.valid() ||
      !itemUbo_.valid() || !blitPipeline_.valid() || !blitUbo_.valid() ||
      !blitSampler_.valid() || !lightUbo_.valid() || !shadowPipeline_.valid() ||
      !shadowSampler_.valid() || !shadowFallbackTex_.valid() ||
      !extractPipeline_.valid() || !blurPipeline_.valid() ||
      !compositePipeline_.valid() || !fxaaPipeline_.valid() || !blurUbo1_.valid() ||
      !blurUbo2_.valid() || !blurUbo3_.valid() || !fxaaUbo_.valid() ||
      !skvs_.valid() || !sdsvs_.valid() || !jointUbo_.valid() || !envOk) {
    shutdown();
    return false;
  }
  // pipeSamples_ 保持 0:首帧 ensureScenePipelines 统一重建全部场景管线
  // (含 skinned;后端管线缓存使 pbr/unlit 重建零开销)
  return true;
}

void Renderer::setLights(const std::vector<LightData>& lights) { lights_ = lights; }

void Renderer::setLightFraming(const float center[3], float radius) {
  framingCenter_[0] = center[0];
  framingCenter_[1] = center[1];
  framingCenter_[2] = center[2];
  framingRadius_ = radius;
}

/// 按画质档确保阴影贴图可用;返回阴影是否激活(目标就绪)。
/// 场景管线按 (格式,采样数) 匹配 SceneTarget;key 变化时重建(后端管线缓存兜底复用)。
void Renderer::ensureScenePipelines(Format fmt, uint32_t samples) {
  if (pipeSamples_ == samples && pipeFmt_ == fmt && pbrPipeline_.valid() &&
      unlitPipeline_.valid())
    return;
  if (pbrPipeline_.valid()) dev_->destroyPipeline(pbrPipeline_);
  if (unlitPipeline_.valid()) dev_->destroyPipeline(unlitPipeline_);
  PipelineDesc upd;
  upd.vertexShader = uvs_;
  upd.fragmentShader = ufs_;
  fillVertexLayout(upd);
  upd.cullMode = CullMode::None;
  upd.depthTest = true;
  upd.depthWrite = true;
  upd.colorFormat = fmt;
  upd.sampleCount = samples;
  unlitPipeline_ = dev_->createPipeline(upd);
  PipelineDesc ppd;
  ppd.vertexShader = vs_;
  ppd.fragmentShader = fs_;
  fillVertexLayout(ppd);
  ppd.cullMode = CullMode::None;
  ppd.depthTest = true;
  ppd.depthWrite = true;
  ppd.colorFormat = fmt;
  ppd.sampleCount = samples;
  pbrPipeline_ = dev_->createPipeline(ppd);
  if (!unlitPipeline_.valid() || !pbrPipeline_.valid())
    RD_LOGE("renderer", "场景管线重建失败(fmt=%d samples=%u)", int(fmt), samples);
  // 混合管线(alphaBlend;与 pbr 同布局/格式/采样数,blend 开 depthWrite 关)
  PipelineDesc bpd = ppd;
  bpd.blend.enable = true;
  bpd.blend.srcColor = BlendFactor::SrcAlpha;
  bpd.blend.dstColor = BlendFactor::OneMinusSrcAlpha;
  bpd.blend.srcAlpha = BlendFactor::One;
  bpd.blend.dstAlpha = BlendFactor::OneMinusSrcAlpha;
  bpd.depthWrite = false;
  if (blendPipeline_.valid()) dev_->destroyPipeline(blendPipeline_);
  blendPipeline_ = dev_->createPipeline(bpd);
  // 蒙皮管线(80B 六属性;pbr frag 复用)
  PipelineDesc skd;
  skd.vertexShader = skvs_;
  skd.fragmentShader = fs_;
  skd.vertexBindings = {{0, 80}};
  skd.attributes = {{0, Format::R32G32B32_FLOAT, 0, 0},
                    {1, Format::R32G32B32_FLOAT, 12, 0},
                    {2, Format::R32G32B32A32_FLOAT, 24, 0},
                    {3, Format::R32G32_FLOAT, 40, 0},
                    {4, Format::R32G32B32A32_FLOAT, 48, 0},
                    {5, Format::R32G32B32A32_FLOAT, 64, 0}};
  skd.cullMode = CullMode::None;
  skd.depthTest = true;
  skd.depthWrite = true;
  skd.colorFormat = fmt;
  skd.sampleCount = samples;
  if (skinnedPipeline_.valid()) dev_->destroyPipeline(skinnedPipeline_);
  skinnedPipeline_ = dev_->createPipeline(skd);
  // 蒙皮阴影管线(depthOnly 80B)
  PipelineDesc ssd = skd;
  ssd.vertexShader = sdsvs_;
  ssd.fragmentShader = sfs_;
  ssd.depthOnly = true;
  if (skinnedShadowPipeline_.valid()) dev_->destroyPipeline(skinnedShadowPipeline_);
  skinnedShadowPipeline_ = dev_->createPipeline(ssd);
  if (!skinnedPipeline_.valid() || !skinnedShadowPipeline_.valid())
    RD_LOGE("renderer", "蒙皮管线重建失败(fmt=%d samples=%u)", int(fmt), samples);
  pipeFmt_ = fmt;
  pipeSamples_ = samples;
}

bool Renderer::ensureShadowTarget() {
  if (shadowMapSize_ == 0) {  // 关档:释放旧阴影资源
    if (shadowTarget_.valid()) dev_->destroyTarget(shadowTarget_);
    if (shadowDepthTex_.valid()) dev_->destroyTexture(shadowDepthTex_);
    shadowTarget_ = {};
    shadowDepthTex_ = {};
    shadowTargetSize_ = 0;
    return false;
  }
  if (shadowTarget_.valid() && shadowTargetSize_ == shadowMapSize_) return true;
  if (shadowTarget_.valid()) dev_->destroyTarget(shadowTarget_);
  if (shadowDepthTex_.valid()) dev_->destroyTexture(shadowDepthTex_);
  shadowTarget_ = {};
  shadowDepthTex_ = {};
  shadowTargetSize_ = 0;
  TextureDesc td;
  td.width = shadowMapSize_;
  td.height = shadowMapSize_;
  td.format = Format::D32_FLOAT;
  td.usage = TextureUsage::Sampled | TextureUsage::RenderTargetAttachment;
  shadowDepthTex_ = dev_->createTexture(td);
  if (!shadowDepthTex_.valid()) {
    RD_LOGE("renderer", "阴影深度纹理创建失败(size=%u)", shadowMapSize_);
    return false;
  }
  OffscreenTargetDesc od;
  od.width = shadowMapSize_;
  od.height = shadowMapSize_;
  od.depthFromTexture = shadowDepthTex_;
  shadowTarget_ = dev_->createOffscreenTarget(od);
  if (!shadowTarget_.valid()) {
    RD_LOGE("renderer", "阴影目标创建失败(size=%u)", shadowMapSize_);
    return false;
  }
  shadowTargetSize_ = shadowMapSize_;
  return true;
}

void Renderer::shutdown() {
  if (!dev_) return;
  env_.destroy(*dev_);
  destroyPostTargets();
  if (fxaaTarget_.valid()) dev_->destroyTarget(fxaaTarget_);
  for (BufferHandle u : {blurUbo1_, blurUbo2_, blurUbo3_, fxaaUbo_, compositeUbo_})
    if (u.valid()) dev_->destroyBuffer(u);
  for (PipelineHandle p : {extractPipeline_, blurPipeline_, compositePipeline_, fxaaPipeline_})
    if (p.valid()) dev_->destroyPipeline(p);
  if (shadowTarget_.valid()) dev_->destroyTarget(shadowTarget_);
  if (shadowDepthTex_.valid()) dev_->destroyTexture(shadowDepthTex_);
  if (shadowFallbackTex_.valid()) dev_->destroyTexture(shadowFallbackTex_);
  if (shadowSampler_.valid()) dev_->destroySampler(shadowSampler_);
  if (shadowPipeline_.valid()) dev_->destroyPipeline(shadowPipeline_);
  if (lightUbo_.valid()) dev_->destroyBuffer(lightUbo_);
  if (sceneTarget_.valid()) dev_->destroyTarget(sceneTarget_);
  if (blitPipeline_.valid()) dev_->destroyPipeline(blitPipeline_);
  if (blitUbo_.valid()) dev_->destroyBuffer(blitUbo_);
  if (blitSampler_.valid()) dev_->destroySampler(blitSampler_);
  if (unlitPipeline_.valid()) dev_->destroyPipeline(unlitPipeline_);
  if (pbrPipeline_.valid()) dev_->destroyPipeline(pbrPipeline_);
  if (blendPipeline_.valid()) dev_->destroyPipeline(blendPipeline_);
  if (uvs_.valid()) dev_->destroyShaderModule(uvs_);
  if (ufs_.valid()) dev_->destroyShaderModule(ufs_);
  if (vs_.valid()) dev_->destroyShaderModule(vs_);
  if (fs_.valid()) dev_->destroyShaderModule(fs_);
  if (sfs_.valid()) dev_->destroyShaderModule(sfs_);
  if (skvs_.valid()) dev_->destroyShaderModule(skvs_);
  if (sdsvs_.valid()) dev_->destroyShaderModule(sdsvs_);
  if (skinnedPipeline_.valid()) dev_->destroyPipeline(skinnedPipeline_);
  if (skinnedShadowPipeline_.valid()) dev_->destroyPipeline(skinnedShadowPipeline_);
  if (jointUbo_.valid()) dev_->destroyBuffer(jointUbo_);
  if (frameUbo_.valid()) dev_->destroyBuffer(frameUbo_);
  if (itemUbo_.valid()) dev_->destroyBuffer(itemUbo_);
  fxaaTarget_ = {};
  blurUbo1_ = blurUbo2_ = blurUbo3_ = fxaaUbo_ = compositeUbo_ = {};
  extractPipeline_ = blurPipeline_ = compositePipeline_ = fxaaPipeline_ = {};
  fxaaW_ = fxaaH_ = 0;
  shadowTarget_ = {};
  shadowDepthTex_ = {};
  shadowFallbackTex_ = {};
  shadowSampler_ = {};
  shadowPipeline_ = {};
  lightUbo_ = {};
  shadowTargetSize_ = 0;
  sceneTarget_ = {};
  blitPipeline_ = {};
  blitUbo_ = {};
  blitSampler_ = {};
  unlitPipeline_ = {};
  pbrPipeline_ = {};
  blendPipeline_ = {};
  uvs_ = {};
  ufs_ = {};
  vs_ = {};
  fs_ = {};
  sfs_ = {};
  skvs_ = {};
  sdsvs_ = {};
  skinnedPipeline_ = {};
  skinnedShadowPipeline_ = {};
  jointUbo_ = {};
  pipeSamples_ = 0;
  frameUbo_ = {};
  itemUbo_ = {};
  sceneW_ = sceneH_ = sceneSamples_ = 0;
  dev_ = nullptr;
}

bool Renderer::setHdrEnvironment(const HdrEnv* env) {
  if (!dev_) return false;
  env_.destroy(*dev_);
  env_.setHdrSource(env);
  if (!env_.build(*dev_, pfVsCode_, pfFsCode_, eqFsCode_, entry_, colorFormat_, iblSize_,
                  iblMips_)) {
    RD_LOGW("renderer", "HDR 环境构建失败,回退程序化");
    env_.destroy(*dev_);
    env_.setHdrSource(nullptr);
    if (!env_.build(*dev_, pfVsCode_, pfFsCode_, eqFsCode_, entry_, colorFormat_,
                    iblSize_, iblMips_))
      RD_LOGE("renderer", "程序化环境恢复失败");
    return false;  // HDR 失败(程序化兜底已尽力恢复)
  }
  RD_LOGI("renderer", "HDR 环境已%s", env ? "应用" : "切回程序化");
  return true;
}

void Renderer::setQuality(const QualityPreset& q) {
  renderScale_ = q.renderScale;
  msaa_ = q.msaa;
  maxTextureDim_ = q.maxTextureDim;
  shadowMapSize_ = shadowMapSizeOverride_ > 0 ? shadowMapSizeOverride_ : q.shadowMapSize;
  const bool wantPost = q.postEnabled != 0;
  if (wantPost && !dev_->caps().supports(Capability::hdr_render_target))
    RD_LOGW("renderer", "后端无 HDR 渲染目标 caps,后处理自动关闭");
  postEnabled_ = wantPost && dev_->caps().supports(Capability::hdr_render_target);
  fxaaEnabled_ = q.fxaaEnabled != 0;
  if (q.iblPrefilterSize != iblSize_ || q.iblPrefilterMips != iblMips_) {
    iblSize_ = q.iblPrefilterSize;
    iblMips_ = q.iblPrefilterMips;
    env_.destroy(*dev_);
    if (!env_.build(*dev_, pfVsCode_, pfFsCode_, eqFsCode_, entry_, colorFormat_,
                    iblSize_, iblMips_))
      RD_LOGE("renderer", "IBL 环境重建失败(size=%u mips=%u)", iblSize_, iblMips_);
  }
}

TargetHandle Renderer::ensureSceneTarget(uint32_t targetW, uint32_t targetH) {
  const uint32_t w = std::max(1u, uint32_t(float(targetW) * renderScale_));
  const uint32_t h = std::max(1u, uint32_t(float(targetH) * renderScale_));
  // 设备对齐(MTLSim 只支持 4x 等):目标与管线统一用对齐后的值
  const uint32_t samples = dev_->snapSampleCount(std::max(1u, msaa_));
  const Format fmt = postEnabled_ ? Format::R16G16B16A16_FLOAT : colorFormat_;
  if (sceneTarget_.valid() && w == sceneW_ && h == sceneH_ && samples == sceneSamples_ &&
      fmt == sceneFormat_)
    return sceneTarget_;
  if (sceneTarget_.valid()) dev_->destroyTarget(sceneTarget_);
  OffscreenTargetDesc td;
  td.width = w;
  td.height = h;
  td.depth = true;
  td.sampleCount = samples;
  td.colorFormat = fmt;
  sceneTarget_ = dev_->createOffscreenTarget(td);
  if (!sceneTarget_.valid()) {
    RD_LOGE("renderer", "SceneTarget 创建失败(%ux%u samples=%u fmt=%d)", w, h, samples,
            int(fmt));
  }
  sceneW_ = w;
  sceneH_ = h;
  sceneSamples_ = samples;
  sceneFormat_ = fmt;
  return sceneTarget_;
}

void Renderer::destroyPostTargets() {
  for (TargetHandle t : {bloomExtract_, bloomL1_, bloomL2_, bloomL3_})
    if (t.valid()) dev_->destroyTarget(t);
  for (TextureHandle t : {bloomExtractTex_, bloomL1Tex_, bloomL2Tex_, bloomL3Tex_})
    if (t.valid()) dev_->destroyTexture(t);
  bloomExtract_ = bloomL1_ = bloomL2_ = bloomL3_ = {};
  bloomExtractTex_ = bloomL1Tex_ = bloomL2Tex_ = bloomL3Tex_ = {};
  postW_ = postH_ = 0;
}

bool Renderer::ensurePostTargets(uint32_t sw, uint32_t sh) {
  if (postW_ == sw && postH_ == sh && bloomExtract_.valid()) return true;
  destroyPostTargets();
  postW_ = sw;
  postH_ = sh;
  auto mkTex = [&](uint32_t w, uint32_t h) {
    TextureDesc td;
    td.width = std::max(1u, w);
    td.height = std::max(1u, h);
    td.format = Format::R16G16B16A16_FLOAT;
    td.usage = TextureUsage::Sampled | TextureUsage::RenderTargetAttachment;
    return dev_->createTexture(td);
  };
  auto mkTarget = [&](TextureHandle t, uint32_t w, uint32_t h) {
    OffscreenTargetDesc od;
    od.width = std::max(1u, w);
    od.height = std::max(1u, h);
    od.colorFromTexture = t;
    return dev_->createOffscreenTarget(od);
  };
  bloomExtractTex_ = mkTex(sw / 2, sh / 2);
  bloomL1Tex_ = mkTex(sw / 4, sh / 4);
  bloomL2Tex_ = mkTex(sw / 8, sh / 8);
  bloomL3Tex_ = mkTex(sw / 16, sh / 16);
  bloomExtract_ = mkTarget(bloomExtractTex_, sw / 2, sh / 2);
  bloomL1_ = mkTarget(bloomL1Tex_, sw / 4, sh / 4);
  bloomL2_ = mkTarget(bloomL2Tex_, sw / 8, sh / 8);
  bloomL3_ = mkTarget(bloomL3Tex_, sw / 16, sh / 16);
  if (!bloomExtract_.valid() || !bloomL1_.valid() || !bloomL2_.valid() || !bloomL3_.valid()) {
    RD_LOGE("renderer", "post 目标链创建失败");
    destroyPostTargets();
    return false;
  }
  // blur 参数 UBO(x=vFlip 已写;yz=texel 随尺寸更新)
  const float vf = dev_->backend() == Backend::GLES ? 1.0f : 0.0f;
  auto writeUbo = [&](BufferHandle u, float tx, float ty) {
    const float p[4] = {vf, tx, ty, 0.0f};
    dev_->updateBuffer(u, p, sizeof(p), 0);
  };
  writeUbo(blurUbo1_, 1.0f / float(std::max(1u, sw / 2)), 1.0f / float(std::max(1u, sh / 2)));
  writeUbo(blurUbo2_, 1.0f / float(std::max(1u, sw / 4)), 1.0f / float(std::max(1u, sh / 4)));
  writeUbo(blurUbo3_, 1.0f / float(std::max(1u, sw / 8)), 1.0f / float(std::max(1u, sh / 8)));
  return true;
}

bool Renderer::ensureFxaaTarget(uint32_t w, uint32_t h) {
  if (fxaaTarget_.valid() && fxaaW_ == w && fxaaH_ == h) return true;
  if (fxaaTarget_.valid()) dev_->destroyTarget(fxaaTarget_);
  OffscreenTargetDesc od;
  od.width = w;
  od.height = h;
  od.colorFormat = colorFormat_;  // 与最终目标格式一致(blit 管线格式匹配)
  fxaaTarget_ = dev_->createOffscreenTarget(od);
  if (!fxaaTarget_.valid()) {
    RD_LOGE("renderer", "fxaa 目标创建失败(%ux%u)", w, h);
    fxaaW_ = fxaaH_ = 0;
    return false;
  }
  fxaaW_ = w;
  fxaaH_ = h;
  const float vf = dev_->backend() == Backend::GLES ? 1.0f : 0.0f;
  const float p[4] = {vf, 1.0f / float(w), 1.0f / float(h), 0.0f};
  dev_->updateBuffer(fxaaUbo_, p, sizeof(p), 0);
  return true;
}

void Renderer::beginScene(const scene::Camera& camera, const ClearColor& clear) {
  viewProj_ = camera.projMatrix() * camera.viewMatrix();
  clear_ = clear;
  cameraEye_ = camera.eye();

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
  queue_.push_back(std::make_unique<MeshRenderable>(mesh));
  worldStack_.push_back(world);
  jointSlot_.push_back(-1);
}

void Renderer::submit(const std::shared_ptr<MeshRenderResource>& mesh,
                      const math::Mat4& world, const math::Mat4* jointPalette,
                      uint32_t jointCount) {
  const uint32_t slot = uint32_t(queue_.size());
  if (slot >= kMaxJointItems || queue_.size() >= kMaxItems) {
    RD_LOGW("renderer", "蒙皮项超出上限(%u),截断", kMaxJointItems);
    return;
  }
  const uint32_t n = std::min(jointCount, 128u);
  if (jointCount > 128u) RD_LOGW("renderer", "关节数 %u 超 128,截断", jointCount);
  if (jointPalette && n > 0)
    dev_->updateBuffer(jointUbo_, jointPalette, uint64_t(n) * 64,
                       uint64_t(slot) * kJointItemStride);
  queue_.push_back(std::make_unique<MeshRenderable>(mesh));
  worldStack_.push_back(world);
  jointSlot_.push_back(int32_t(slot));
}

void Renderer::endScene(CommandBuffer* cmd, TargetHandle target) {
  const uint32_t count = uint32_t(queue_.size());
  // opaque/blend 分区排序:opaque 先;blend 按视距远→近(正确透明叠加)
  std::vector<uint32_t> order(count);
  for (uint32_t i = 0; i < count; ++i) order[i] = i;
  auto isBlend = [&](uint32_t i) {
    const auto* r = static_cast<const MeshRenderable*>(queue_[i].get());
    return !r->meshData().empty() && r->meshData()[0].material.alphaBlend;
  };
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    const bool ba = isBlend(a), bb = isBlend(b);
    if (ba != bb) return !ba;
    const math::Vec4 e(cameraEye_, 1.0f);
    const float da = glm::dot(worldStack_[a][3] - e, worldStack_[a][3] - e);
    const float db = glm::dot(worldStack_[b][3] - e, worldStack_[b][3] - e);
    return da > db;
  });

  // 统一填充 per-item ItemUBO(槽位按排序后顺序)
  for (uint32_t i = 0; i < count; ++i) {
    const auto* renderable = static_cast<const MeshRenderable*>(queue_[order[i]].get());
    const auto& meshes = renderable->meshData();
    // ItemUBO 按 mesh 首个材质填(多 mesh 模型共享 item 槽——简化:逐 item 一个 UBO,
    // 多 mesh 差异材质归 P2 拆 item)
    ItemUBOData iu{};
    const auto& world = worldStack_[order[i]];
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

  // ---- LightUBO 填充(无灯 → 默认 1 方向光,与 2b/2c 现状一致)----
  std::vector<LightData> effective = lights_;
  if (effective.empty()) {
    LightData d;
    d.type = LightType::Directional;
    const float n = std::sqrt(0.5f * 0.5f + 0.8f * 0.8f + 0.3f * 0.3f);
    d.direction[0] = -0.5f / n;
    d.direction[1] = 0.8f / n;
    d.direction[2] = 0.3f / n;
    d.color[0] = 0.8f;
    d.color[1] = 0.75f;
    d.color[2] = 0.7f;
    effective.push_back(d);
  }
  const LightData* dirLight = nullptr;
  for (const auto& l : effective)
    if (l.type == LightType::Directional) {
      dirLight = &l;
      break;
    }
  // 阴影激活:手动开 + 画质档非 0 + 有方向光 + 目标就绪
  const bool shadowActive = shadowManual_ && shadowMapSize_ > 0 && dirLight != nullptr &&
                            ensureShadowTarget();
  math::Mat4 lvp{1.0f};
  if (dirLight) lvp = makeLightViewProj(*dirLight, framingCenter_, framingRadius_);
  LightUBOData lu{};
  fillLightUBO(lu, effective, lvp,
               shadowMapSize_ ? 1.0f / float(shadowMapSize_) : 0.0f, shadowActive,
               dev_->backend() == Backend::GLES, shadowBias_);
  lu.lightCount[1] = postEnabled_ ? 1.0f : 0.0f;  // hdrMode(post 开输出线性 HDR)
  dev_->updateBuffer(lightUbo_, &lu, sizeof(lu), 0);

  // ---- ShadowPass(场景 pass 之前)----
  if (shadowActive) {
    cmd->beginRenderPass(shadowTarget_, {0, 0, 0, 1, 1.0f});
    RenderContext sctx;
    sctx.shadowPass = true;
    sctx.lightUbo = lightUbo_;
    sctx.itemUbo = itemUbo_;
    sctx.shadowPipe = shadowPipeline_;
    sctx.skinnedShadowPipe = skinnedShadowPipeline_;
    sctx.jointUbo = jointUbo_;
    for (uint32_t i = 0; i < count; ++i) {
      sctx.itemOffset = uint64_t(i) * kUboStride;
      sctx.jointOffset = jointSlot_[order[i]] >= 0
                             ? uint64_t(jointSlot_[order[i]]) * kJointItemStride
                             : 0;
      queue_[order[i]]->record(cmd, sctx);
    }
    cmd->endRenderPass();
  }

  // 上屏链:场景 → 内部 SceneTarget(分辨率缩放/MSAA 按画质档;post 开=R16F)
  uint32_t tw = 0, th = 0;
  dev_->targetSize(target, tw, th);
  TargetHandle scene = ensureSceneTarget(tw, th);
  if (!scene.valid()) {  // 场景目标失败:退化为直接渲染到最终目标
    scene = target;
  }
  // 场景管线按目标格式/采样数匹配(direct 回落时取最终目标格式)
  if (scene != target) {
    ensureScenePipelines(sceneFormat_, sceneSamples_);
  } else {
    ensureScenePipelines(colorFormat_, 1);
  }
  cmd->beginRenderPass(scene, clear_);
  RenderContext ctx;
  ctx.frameUbo = frameUbo_;
  ctx.itemUbo = itemUbo_;
  ctx.env = &env_;
  ctx.lightUbo = lightUbo_;
  ctx.shadowMap = shadowActive ? shadowDepthTex_ : shadowFallbackTex_;
  ctx.shadowSampler = shadowSampler_;
  ctx.pbrPipeline = pbrPipeline_;
  ctx.unlitPipeline = unlitPipeline_;
  ctx.skinnedPipeline = skinnedPipeline_;
  ctx.skinnedShadowPipe = skinnedShadowPipeline_;
  ctx.blendPipeline = blendPipeline_;
  ctx.jointUbo = jointUbo_;
  for (uint32_t i = 0; i < count; ++i) {
    queue_[order[i]]->prepass(cmd);  // 2b 钩子(默认空)
    ctx.itemOffset = uint64_t(i) * kUboStride;
    ctx.jointOffset = jointSlot_[order[i]] >= 0
                          ? uint64_t(jointSlot_[order[i]]) * kJointItemStride
                          : 0;
    queue_[order[i]]->record(cmd, ctx);
  }
  cmd->endRenderPass();
  if (scene != target) {
    const bool postActive = postEnabled_ && ensurePostTargets(sceneW_, sceneH_);
    if (postActive) {
      // HDR 后处理:extract → 3 级 blur → composite(ACES+gamma)直出
      const TextureHandle sceneTex = dev_->targetColorTexture(scene);
      auto fsPass = [&](TargetHandle tg, PipelineHandle p, BufferHandle u,
                        TextureHandle src) {
        cmd->beginRenderPass(tg, clear_);
        cmd->bindPipeline(p);
        cmd->bindUniformBuffer(0, u, 0, 16);
        cmd->bindTexture(0, src, blitSampler_);
        cmd->draw(3, 0);
        cmd->endRenderPass();
      };
      fsPass(bloomExtract_, extractPipeline_, blitUbo_, sceneTex);
      fsPass(bloomL1_, blurPipeline_, blurUbo1_, bloomExtractTex_);
      fsPass(bloomL2_, blurPipeline_, blurUbo2_, bloomL1Tex_);
      fsPass(bloomL3_, blurPipeline_, blurUbo3_, bloomL2Tex_);
      cmd->beginRenderPass(target, clear_);
      cmd->bindPipeline(compositePipeline_);
      // composite 独立 UBO(逐 pass 独立教训):x=vFlip,w=exposure
      const float cp[4] = {dev_->backend() == Backend::GLES ? 1.0f : 0.0f, 0.0f, 0.0f,
                           compositeExposure_};
      dev_->updateBuffer(compositeUbo_, cp, sizeof(cp), 0);
      cmd->bindUniformBuffer(0, compositeUbo_, 0, 16);
      cmd->bindTexture(0, sceneTex, blitSampler_);
      cmd->bindTexture(1, bloomL1Tex_, blitSampler_);
      cmd->bindTexture(2, bloomL2Tex_, blitSampler_);
      cmd->bindTexture(3, bloomL3Tex_, blitSampler_);
      cmd->draw(3, 0);
      cmd->endRenderPass();
    } else if (fxaaEnabled_ && sceneSamples_ <= 1 && ensureFxaaTarget(tw, th)) {
      // LDR + FXAA:blit → fxaaTarget → fxaa → 最终目标
      cmd->beginRenderPass(fxaaTarget_, clear_);
      cmd->bindPipeline(blitPipeline_);
      cmd->bindUniformBuffer(0, blitUbo_, 0, 16);
      cmd->bindTexture(0, dev_->targetColorTexture(scene), blitSampler_);
      cmd->draw(3, 0);
      cmd->endRenderPass();
      cmd->beginRenderPass(target, clear_);
      cmd->bindPipeline(fxaaPipeline_);
      cmd->bindUniformBuffer(0, fxaaUbo_, 0, 16);
      cmd->bindTexture(0, dev_->targetColorTexture(fxaaTarget_), blitSampler_);
      cmd->draw(3, 0);
      cmd->endRenderPass();
    } else {  // upscale pass(P2 后处理链挂载点)
      cmd->beginRenderPass(target, clear_);
      cmd->bindPipeline(blitPipeline_);
      cmd->bindUniformBuffer(0, blitUbo_, 0, 16);
      cmd->bindTexture(0, dev_->targetColorTexture(scene), blitSampler_);
      cmd->draw(3, 0);
      cmd->endRenderPass();
    }
  }
  queue_.clear();
  worldStack_.clear();
  jointSlot_.clear();
}

} // namespace rd
