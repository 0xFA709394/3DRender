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

/// 从 viewProj 提取 6 视锥平面(glm 列主序:plane = row3 ± row0/1/2)。
struct FrustumPlanes {
  glm::vec4 p[6];
};
FrustumPlanes extractFrustum(const glm::mat4& vp) {
  auto row = [&](int i) { return glm::vec4(vp[0][i], vp[1][i], vp[2][i], vp[3][i]); };
  const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
  FrustumPlanes f;
  f.p[0] = r3 + r0;  // left
  f.p[1] = r3 - r0;  // right
  f.p[2] = r3 + r1;  // bottom
  f.p[3] = r3 - r1;  // top
  f.p[4] = r2;       // near(D3D 约定 z∈[0,1]:plane=z 行)
  f.p[5] = r3 - r2;  // far
  for (auto& pl : f.p) {
    const float l = glm::length(glm::vec3(pl));
    if (l > 0.0f) pl /= l;
  }
  return f;
}
/// 包围球 × world 矩阵与视锥测试;true=可见(相交/内部)。
bool sphereVisible(const FrustumPlanes& f, const glm::mat4& world, const float* center,
                   float radius) {
  const glm::vec4 c4 = world * glm::vec4(center[0], center[1], center[2], 1.0f);
  // 半径随最大轴缩放
  const float sx = glm::length(glm::vec3(world[0]));
  const float sy = glm::length(glm::vec3(world[1]));
  const float sz = glm::length(glm::vec3(world[2]));
  const float r = radius * std::max(sx, std::max(sy, sz));
  for (const auto& pl : f.p)
    if (glm::dot(glm::vec3(pl), glm::vec3(c4)) + pl.w < -r) return false;
  return true;
}
} // namespace
namespace {

/// 顶点布局(48B 交错):pos3@0|normal3@12|tangent4@24|uv2@40
void fillVertexLayout(PipelineDesc& pd) {
  pd.vertexBindings = {{0, 48}};
  pd.attributes = {{0, Format::R32G32B32_FLOAT, 0, 0},
                   {1, Format::R32G32B32_FLOAT, 12, 0},
                   {2, Format::R32G32B32A32_FLOAT, 24, 0},
                   {3, Format::R32G32_FLOAT, 40, 0}};
}

/// ItemUBO 布局(304B 块,槽距 512B):mvp|world|normalMatrix|baseColorFactor|
/// emissiveOcc|metalRough|uvTf|ext0|ext1|ext2
struct ItemUBOData {
  math::Mat4 mvp;
  math::Mat4 world;
  math::Mat4 normalMatrix;
  float baseColorFactor[4];
  float emissiveOcc[4];     // rgb=emissiveFactor, a=occlusionStrength
  float metallicRough[4];   // x=metallic, y=roughness, z=normalScale, w=alphaCutoff
  float uvTransform[4];     // xy=offset, zw=scale
  float ext0[4];            // x=clearcoatFactor y=clearcoatRoughness z=clearcoatNormalScale w=specularFactor
  float ext1[4];            // xyz=sheenColorFactor w=sheenRoughnessFactor
  float ext2[4];            // xyz=specularColorFactor w=ior
  float ext3[4];            // x=transmissionFactor y=thicknessFactor z=attenuationDistance(0=∞) w=morphTargetCount
  float ext4[4];            // xyz=attenuationColor w=morphTargetCount(与 ext3.w 同值,vert 就近读)
  float ext5[4];            // morph weights[0..3]
  float ext6[4];            // morph weights[4..7]
};
static_assert(sizeof(ItemUBOData) == kItemUboSize, "ItemUBO 必须 368B(槽距 512)");

} // namespace

bool Renderer::init(Device& dev, const RendererShaderDesc& desc) {
  dev_ = &dev;
  colorFormat_ = desc.colorFormat;
  entry_ = desc.entry;
  pfVsCode_ = desc.prefilterVs;
  pfFsCode_ = desc.prefilterFs;
  eqFsCode_ = desc.equirectFs;
  blitVsCode_ = desc.blitVs;
  blitFsCode_ = desc.blitFs;

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
  ppd.separateSamplers = true;  // pbr 族:分离采样器布局
  pbrPipeline_ = dev.createPipeline(ppd);

  // 双层 UBO
  frameUbo_ = dev.createBuffer({272, BufferUsage::Uniform, true, false, nullptr});
  itemUbo_ = dev.createBuffer({uint64_t(kUboStride) * kMaxItemSlots, BufferUsage::Uniform, true,
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
  // morph 系模块(空码=不建,向后兼容)
  if (!desc.morphVs.empty())
    morphVs_ = dev.createShaderModule({ShaderStage::Vertex, desc.morphVs, desc.entry});
  if (!desc.morphSkinnedVs.empty())
    morphSkvs_ = dev.createShaderModule({ShaderStage::Vertex, desc.morphSkinnedVs, desc.entry});
  if (!desc.morphShadowVs.empty())
    morphSv_ = dev.createShaderModule({ShaderStage::Vertex, desc.morphShadowVs, desc.entry});
  if (!desc.morphSkinnedShadowVs.empty())
    morphSdsvs_ =
        dev.createShaderModule({ShaderStage::Vertex, desc.morphSkinnedShadowVs, desc.entry});
  if (!desc.skyboxVs.empty() && !desc.skyboxFs.empty()) {
    skyVs_ = dev.createShaderModule({ShaderStage::Vertex, desc.skyboxVs, desc.entry});
    skyFs_ = dev.createShaderModule({ShaderStage::Fragment, desc.skyboxFs, desc.entry});
  }
  if (!desc.instancedVs.empty() && !desc.instancedFs.empty()) {
    instVs_ = dev.createShaderModule({ShaderStage::Vertex, desc.instancedVs, desc.entry});
    instFs_ = dev.createShaderModule({ShaderStage::Fragment, desc.instancedFs, desc.entry});
  }
  skyboxVb_ = dev.createBuffer({36, BufferUsage::Vertex, true, false, nullptr});
  jointUbo_ = dev.createBuffer({uint64_t(kJointItemStride) * kMaxJointItems,
                                BufferUsage::Uniform, true, false, nullptr});

  // 环境(SH/LUT/GPU 预滤波,init 期一次性;尺寸/级数按画质档,默认现状 64/5)
  const bool envOk = env_.build(dev, desc.prefilterVs, desc.prefilterFs, desc.equirectFs,
                                desc.entry, desc.colorFormat, iblSize_, iblMips_);

  // 多光源 + 阴影资源
  lightUbo_ = dev.createBuffer({sizeof(LightUBOData), BufferUsage::Uniform, true, false, nullptr});
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
  // cutout 阴影管线(mask 材质;同布局 + vUV;invalid=不裁剪)
  if (!desc.shadowMaskVs.empty() && !desc.shadowMaskFs.empty()) {
    auto mvs = dev.createShaderModule({ShaderStage::Vertex, desc.shadowMaskVs, desc.entry});
    auto mfs = dev.createShaderModule({ShaderStage::Fragment, desc.shadowMaskFs, desc.entry});
    PipelineDesc mpd = spd;
    mpd.vertexShader = mvs;
    mpd.fragmentShader = mfs;
    shadowMaskPipeline_ = dev.createPipeline(mpd);
    dev.destroyShaderModule(mvs);
    dev.destroyShaderModule(mfs);
  }
  // 实例化阴影管线(frag 复用空 shadow_depth.frag)
  if (!desc.shadowInstVs.empty()) {
    auto ivs = dev.createShaderModule({ShaderStage::Vertex, desc.shadowInstVs, desc.entry});
    PipelineDesc ipd = spd;
    ipd.vertexShader = ivs;
    shadowInstPipeline_ = dev.createPipeline(ipd);
    dev.destroyShaderModule(ivs);
  }
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

  // transmission 占位(1x1 白)与共享采样器(pass A 恒绑 slot16;关闭/降级路径)
  {
    TextureDesc ttd;
    ttd.width = 1;
    ttd.height = 1;
    ttd.format = Format::RGBA8_UNORM;
    ttd.usage = TextureUsage::Sampled;
    const uint8_t white[4] = {255, 255, 255, 255};
    ttd.data = white;
    ttd.dataSize = 4;
    transPlaceholderTex_ = dev.createTexture(ttd);
    transSampler_ = dev.createSampler({});  // linear+mipmap(与 mesh sampler 同状态)
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
  ppd.separateSamplers = true;  // pbr 族:分离采样器布局(blend/skinned 继承)
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
  skd.separateSamplers = true;  // 蒙皮复用 pbr frag → 同族
  if (skinnedPipeline_.valid()) dev_->destroyPipeline(skinnedPipeline_);
  skinnedPipeline_ = dev_->createPipeline(skd);
  // 蒙皮阴影管线(depthOnly 80B;shadow 族=combined 布局,显式关闭继承)
  PipelineDesc ssd = skd;
  ssd.vertexShader = sdsvs_;
  ssd.fragmentShader = sfs_;
  ssd.depthOnly = true;
  ssd.separateSamplers = false;
  if (skinnedShadowPipeline_.valid()) dev_->destroyPipeline(skinnedShadowPipeline_);
  skinnedShadowPipeline_ = dev_->createPipeline(ssd);
  if (!skinnedPipeline_.valid() || !skinnedShadowPipeline_.valid())
    RD_LOGE("renderer", "蒙皮管线重建失败(fmt=%d samples=%u)", int(fmt), samples);
  // ---- morph 管线 ×4(48B/80B × 场景/阴影;均 pbr 分离采样器族——
  // shadow 变体声明 texMorph/smpMat 分离采样器,pbr 布局为其超集)----
  if (morphVs_.valid() && fs_.valid()) {
    PipelineDesc mpd;
    mpd.vertexShader = morphVs_;
    mpd.fragmentShader = fs_;
    fillVertexLayout(mpd);
    mpd.cullMode = CullMode::None;
    mpd.depthTest = true;
    mpd.depthWrite = true;
    mpd.colorFormat = fmt;
    mpd.sampleCount = samples;
    mpd.separateSamplers = true;
    if (morphPipeline_.valid()) dev_->destroyPipeline(morphPipeline_);
    morphPipeline_ = dev_->createPipeline(mpd);
  }
  if (morphSkvs_.valid() && fs_.valid()) {
    PipelineDesc msd = skd;  // 80B 六属性;separateSamplers 已 true
    msd.vertexShader = morphSkvs_;
    if (morphSkinnedPipeline_.valid()) dev_->destroyPipeline(morphSkinnedPipeline_);
    morphSkinnedPipeline_ = dev_->createPipeline(msd);
  }
  if (morphSv_.valid() && sfs_.valid()) {
    PipelineDesc msd2;
    msd2.vertexShader = morphSv_;
    msd2.fragmentShader = sfs_;
    fillVertexLayout(msd2);
    msd2.depthOnly = true;
    msd2.separateSamplers = true;  // shadow_morph 声明分离采样器 → pbr 布局
    if (morphShadowPipeline_.valid()) dev_->destroyPipeline(morphShadowPipeline_);
    morphShadowPipeline_ = dev_->createPipeline(msd2);
  }
  if (morphSdsvs_.valid() && sfs_.valid()) {
    PipelineDesc mssd = ssd;  // 80B depthOnly
    mssd.vertexShader = morphSdsvs_;
    mssd.separateSamplers = true;  // 同上:分离采样器 → pbr 布局
    if (morphSkinnedShadowPipeline_.valid())
      dev_->destroyPipeline(morphSkinnedShadowPipeline_);
    morphSkinnedShadowPipeline_ = dev_->createPipeline(mssd);
  }
  // 天空盒管线(depthTest/Write 关,场景 pass 首画;物体后画覆盖)
  if (skyVs_.valid() && skyFs_.valid()) {
    PipelineDesc skyd;
    skyd.vertexShader = skyVs_;
    skyd.fragmentShader = skyFs_;
    skyd.vertexBindings = {{0, 12}};
    skyd.attributes = {{0, Format::R32G32B32_FLOAT, 0, 0}};
    skyd.cullMode = CullMode::None;
    skyd.depthTest = false;
    skyd.depthWrite = false;
    skyd.colorFormat = fmt;
    skyd.sampleCount = samples;
    if (skyboxPipeline_.valid()) dev_->destroyPipeline(skyboxPipeline_);
    skyboxPipeline_ = dev_->createPipeline(skyd);
  }
  // 实例化管线(与 pbr 同布局/格式/采样数)
  if (instVs_.valid() && instFs_.valid()) {
    PipelineDesc ipd;
    ipd.vertexShader = instVs_;
    ipd.fragmentShader = instFs_;
    fillVertexLayout(ipd);
    ipd.cullMode = CullMode::None;
    ipd.depthTest = true;
    ipd.depthWrite = true;
    ipd.colorFormat = fmt;
    ipd.sampleCount = samples;
    ipd.separateSamplers = true;  // 实例化复用 pbr frag → 同族
    if (instancedPipeline_.valid()) dev_->destroyPipeline(instancedPipeline_);
    instancedPipeline_ = dev_->createPipeline(ipd);
  }
  pipeFmt_ = fmt;
  pipeSamples_ = samples;
}

bool Renderer::ensureSpotShadowTarget() {
  if (shadowMapSize_ == 0) {
    if (spotShadowTarget_.valid()) dev_->destroyTarget(spotShadowTarget_);
    if (spotShadowDepthTex_.valid()) dev_->destroyTexture(spotShadowDepthTex_);
    spotShadowTarget_ = {};
    spotShadowDepthTex_ = {};
    return false;
  }
  if (spotShadowTarget_.valid()) return true;  // 尺寸跟随 dir 档,简化不重建
  TextureDesc td;
  td.width = shadowMapSize_;
  td.height = shadowMapSize_;
  td.format = Format::D32_FLOAT;
  td.usage = TextureUsage::Sampled | TextureUsage::RenderTargetAttachment;
  spotShadowDepthTex_ = dev_->createTexture(td);
  if (!spotShadowDepthTex_.valid()) return false;
  OffscreenTargetDesc od;
  od.width = shadowMapSize_;
  od.height = shadowMapSize_;
  od.depthFromTexture = spotShadowDepthTex_;
  spotShadowTarget_ = dev_->createOffscreenTarget(od);
  return spotShadowTarget_.valid();
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
  if (spotShadowTarget_.valid()) dev_->destroyTarget(spotShadowTarget_);
  if (spotShadowDepthTex_.valid()) dev_->destroyTexture(spotShadowDepthTex_);
  if (shadowDepthTex_.valid()) dev_->destroyTexture(shadowDepthTex_);
  shadowTarget_ = {};
  spotShadowTarget_ = {};
  spotShadowDepthTex_ = {};
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
  if (spotShadowTarget_.valid()) dev_->destroyTarget(spotShadowTarget_);
  if (spotShadowDepthTex_.valid()) dev_->destroyTexture(spotShadowDepthTex_);
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
  for (PipelineHandle p : {morphPipeline_, morphSkinnedPipeline_, morphShadowPipeline_,
                           morphSkinnedShadowPipeline_})
    if (p.valid()) dev_->destroyPipeline(p);
  for (ShaderModuleHandle m : {morphVs_, morphSkvs_, morphSv_, morphSdsvs_})
    if (m.valid()) dev_->destroyShaderModule(m);
  if (skyboxPipeline_.valid()) dev_->destroyPipeline(skyboxPipeline_);
  if (skyVs_.valid()) dev_->destroyShaderModule(skyVs_);
  if (skyFs_.valid()) dev_->destroyShaderModule(skyFs_);
  if (skyboxVb_.valid()) dev_->destroyBuffer(skyboxVb_);
  if (instancedPipeline_.valid()) dev_->destroyPipeline(instancedPipeline_);
  if (instVs_.valid()) dev_->destroyShaderModule(instVs_);
  if (instFs_.valid()) dev_->destroyShaderModule(instFs_);
  if (shadowMaskPipeline_.valid()) dev_->destroyPipeline(shadowMaskPipeline_);
  if (shadowInstPipeline_.valid()) dev_->destroyPipeline(shadowInstPipeline_);
  if (jointUbo_.valid()) dev_->destroyBuffer(jointUbo_);
  if (frameUbo_.valid()) dev_->destroyBuffer(frameUbo_);
  if (itemUbo_.valid()) dev_->destroyBuffer(itemUbo_);
  if (transTarget_.valid()) dev_->destroyTarget(transTarget_);
  if (transTex_.valid()) dev_->destroyTexture(transTex_);
  if (transPlaceholderTex_.valid()) dev_->destroyTexture(transPlaceholderTex_);
  if (transBlitPipeline_.valid()) dev_->destroyPipeline(transBlitPipeline_);
  if (transSampler_.valid()) dev_->destroySampler(transSampler_);
  fxaaTarget_ = {};
  blurUbo1_ = blurUbo2_ = blurUbo3_ = fxaaUbo_ = compositeUbo_ = {};
  extractPipeline_ = blurPipeline_ = compositePipeline_ = fxaaPipeline_ = {};
  fxaaW_ = fxaaH_ = 0;
  shadowTarget_ = {};
  shadowDepthTex_ = {};
  spotShadowTarget_ = {};
  spotShadowDepthTex_ = {};
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
  morphPipeline_ = morphSkinnedPipeline_ = {};
  morphShadowPipeline_ = morphSkinnedShadowPipeline_ = {};
  morphVs_ = morphSkvs_ = morphSv_ = morphSdsvs_ = {};
  skyboxPipeline_ = {};
  skyVs_ = {};
  skyFs_ = {};
  skyboxVb_ = {};
  instancedPipeline_ = {};
  instVs_ = {};
  instFs_ = {};
  shadowMaskPipeline_ = {};
  shadowInstPipeline_ = {};
  jointUbo_ = {};
  pipeSamples_ = 0;
  frameUbo_ = {};
  itemUbo_ = {};
  sceneW_ = sceneH_ = sceneSamples_ = 0;
  dev_ = nullptr;
}

void Renderer::setEnvYaw(float deg) {
  if (deg == envYawDeg_) return;
  envYawDeg_ = deg;
  // HDR 模式:yaw 烘进 equirect pass,值变重建环境(IBL 与天空盒一致)
  if (dev_ && env_.hdrSource()) {
    env_.setYawDeg(deg);
    env_.destroy(*dev_);
    if (!env_.build(*dev_, pfVsCode_, pfFsCode_, eqFsCode_, entry_, colorFormat_, iblSize_,
                    iblMips_))
      RD_LOGE("renderer", "yaw 环境重建失败");
  }
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
  extMaterialsQuality_ = q.extMaterials != 0;
  transmissionQuality_ = q.transmission != 0;
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
  td.preserveContent = true;  // transmission 两段 pass:pass A 内容(MSAA/深度)须持久
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

bool Renderer::ensureTransmissionTarget(uint32_t w, uint32_t h, Format fmt) {
  if (transTex_.valid() && w == transW_ && h == transH_ && fmt == transFmt_) return true;
  if (transTarget_.valid()) dev_->destroyTarget(transTarget_);
  if (transTex_.valid()) dev_->destroyTexture(transTex_);
  if (transBlitPipeline_.valid()) dev_->destroyPipeline(transBlitPipeline_);
  transBlitPipeline_ = {};
  const uint32_t mips =
      1 + uint32_t(std::floor(std::log2(float(std::max(w, h)))));
  TextureDesc td;
  td.width = w;
  td.height = h;
  td.format = fmt;
  td.usage = TextureUsage::Sampled | TextureUsage::RenderTargetAttachment;
  td.mipLevels = mips;
  transTex_ = dev_->createTexture(td);
  OffscreenTargetDesc od;
  od.width = w;
  od.height = h;
  od.colorFromTexture = transTex_;
  od.mipLevel = 0;
  transTarget_ = dev_->createOffscreenTarget(od);
  if (!transTex_.valid() || !transTarget_.valid()) {
    RD_LOGE("renderer", "transmission 纹理/目标创建失败(%ux%u fmt=%d)", w, h, int(fmt));
    return false;
  }
  // 拷贝管线:blit 系(scene 颜色 → transTex mip0),格式随场景(R16F/RGBA8)
  auto vs = dev_->createShaderModule({ShaderStage::Vertex, blitVsCode_, entry_});
  auto fs = dev_->createShaderModule({ShaderStage::Fragment, blitFsCode_, entry_});
  PipelineDesc pd;
  pd.vertexShader = vs;
  pd.fragmentShader = fs;
  pd.cullMode = CullMode::None;
  pd.colorFormat = fmt;
  transBlitPipeline_ = dev_->createPipeline(pd);
  dev_->destroyShaderModule(vs);
  dev_->destroyShaderModule(fs);
  if (!transBlitPipeline_.valid()) return false;
  transW_ = w;
  transH_ = h;
  transMips_ = mips;
  transFmt_ = fmt;
  return true;
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

  // FrameUBO:viewProj|cameraPos|lightDir|lightColor|sh[9×vec4]|transmissionParams
  struct {
    math::Mat4 viewProj;
    math::Vec4 cameraPos;
    math::Vec4 lightDir;
    math::Vec4 lightColor;
    float sh[9][4];
    float transmissionParams[4];  // x=1/transW y=1/transH z=maxLod w=0(Task 6 填真值)
  } fu;
  static_assert(sizeof(fu) == 272, "FrameUBO 必须 272B");
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
  fu.transmissionParams[0] = fu.transmissionParams[1] = fu.transmissionParams[2] =
      fu.transmissionParams[3] = 0.0f;
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
  morphOverride_.emplace_back();
  const auto& md = mesh ? mesh->meshes() : std::vector<MeshGpuData>();
  meshCount_.push_back(uint32_t(std::max<size_t>(1, md.size())));
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
  morphOverride_.emplace_back();
  const auto& md2 = mesh ? mesh->meshes() : std::vector<MeshGpuData>();
  meshCount_.push_back(uint32_t(std::max<size_t>(1, md2.size())));
}

void Renderer::submit(const std::shared_ptr<MeshRenderResource>& mesh,
                      const math::Mat4& world, const math::Mat4* jointPalette,
                      uint32_t jointCount, const float* morphWeights,
                      uint32_t morphCount) {
  if (!jointPalette || jointCount == 0) {
    // 非蒙皮:morph-only 路径(复用基础 submit + 权重覆盖)
    if (queue_.size() >= kMaxItems) {
      RD_LOGW("renderer", "渲染项超出 %u,截断", kMaxItems);
      return;
    }
    queue_.push_back(std::make_unique<MeshRenderable>(mesh));
    worldStack_.push_back(world);
    jointSlot_.push_back(-1);
    const auto& md = mesh ? mesh->meshes() : std::vector<MeshGpuData>();
    meshCount_.push_back(uint32_t(std::max<size_t>(1, md.size())));
    morphOverride_.emplace_back();
    if (morphWeights && morphCount > 0)
      morphOverride_.back().assign(morphWeights, morphWeights + std::min(morphCount, 8u));
    return;
  }
  submit(mesh, world, jointPalette, jointCount);
  if (morphWeights && morphCount > 0 && !morphOverride_.empty())
    morphOverride_.back().assign(morphWeights, morphWeights + std::min(morphCount, 8u));
}

void Renderer::endScene(CommandBuffer* cmd, TargetHandle target) {
  const uint32_t count = uint32_t(queue_.size());
  // opaque → transmission → blend 三分区:前者和后者均按视距远→近;opaque 保提交序
  const bool transOn = transmissionManual_ && transmissionQuality_;
  std::vector<uint32_t> order(count);
  for (uint32_t i = 0; i < count; ++i) order[i] = i;
  auto matOf = [&](uint32_t i) -> const MaterialData* {
    const auto* r = static_cast<const MeshRenderable*>(queue_[i].get());
    return r->meshData().empty() ? nullptr : &r->meshData()[0].material;
  };
  auto tierOf = [&](uint32_t i) {
    const MaterialData* m = matOf(i);
    if (!m) return 0;
    if (m->alphaBlend) return 2;
    if (transOn && m->transmissionFactor > 0.0f) return 1;
    return 0;
  };
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    const int ta = tierOf(a), tb = tierOf(b);
    if (ta != tb) return ta < tb;
    if (ta == 0) return false;  // opaque 稳定(保提交序)
    const math::Vec4 e(cameraEye_, 1.0f);
    const float da = glm::dot(worldStack_[a][3] - e, worldStack_[a][3] - e);
    const float db = glm::dot(worldStack_[b][3] - e, worldStack_[b][3] - e);
    return da > db;  // transmission/blend 按视距远→近
  });

  // ---- LightUBO 填充(无灯 → 默认 1 方向光,与 2b/2c 现状一致)----
  std::vector<LightData> effective = lights_;
  if (effective.empty()) {
    LightData d;
    d.type = LightType::Directional;    const float n = std::sqrt(0.5f * 0.5f + 0.8f * 0.8f + 0.3f * 0.3f);
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
  // 首盏聚光阴影(选项 shadow.spot 开关;spot VP + 目标就绪)
  const LightData* spotLight = nullptr;
  for (const auto& l : effective)
    if (l.type == LightType::Spot) { spotLight = &l; break; }
  const bool spotShadowActive = spotEnabled_ && spotLight != nullptr &&
                                shadowMapSize_ > 0 && ensureSpotShadowTarget();
  const math::Mat4 spotVP = spotLight ? makeSpotViewProj(*spotLight) : math::Mat4(1.0f);
  fillLightUBO(lu, effective, lvp,
               shadowMapSize_ ? 1.0f / float(shadowMapSize_) : 0.0f, shadowActive,
               dev_->backend() == Backend::GLES, shadowBias_, spotVP,
               spotShadowActive,
               shadowMapSize_ ? 1.0f / float(shadowMapSize_) : 0.0f);
  lu.lightCount[1] = postEnabled_ ? 1.0f : 0.0f;  // hdrMode(post 开输出线性 HDR)
  dev_->updateBuffer(lightUbo_, &lu, sizeof(lu), 0);


  // ---- 视锥剔除(相机 VP 场景 pass / 光源 VP 阴影 pass;蒙皮项跳过)----
  const FrustumPlanes camFrustum = extractFrustum(viewProj_);
  const FrustumPlanes lightFrustum = extractFrustum(lvp);
  auto culledBy = [&](uint32_t idx, const FrustumPlanes& f) {
    const auto* r0 = static_cast<const MeshRenderable*>(queue_[idx].get());
    const bool dynamic0 =
        jointSlot_[idx] >= 0 ||
        (!r0->meshData().empty() && r0->meshData()[0].morph);  // 蒙皮/morph 不剔除
    if (!frustumCulling_ || dynamic0) return false;
    const auto* r = static_cast<const MeshRenderable*>(queue_[idx].get());
    if (r->meshData().empty()) return false;
    return !sphereVisible(f, worldStack_[idx], r->boundingCenter(), r->boundingRadius());
  };
  std::vector<uint32_t> camVis, lightVis;
  for (uint32_t i : order) {
    if (!culledBy(i, camFrustum)) camVis.push_back(i);
  }
  if (shadowActive)
    for (uint32_t i : order) {
      if (!culledBy(i, lightFrustum)) lightVis.push_back(i);
    }
  // ItemUBO 槽位 = 两可见集并集;per-item 分配 meshCount 个连续槽(per-mesh 材质)
  std::vector<int32_t> slotOf(count, -1);
  uint32_t slotCount = 0;
  auto assignSlot = [&](uint32_t idx) {
    if (slotOf[idx] >= 0) return;
    const uint32_t mc = meshCount_[idx];
    if (slotCount + mc > kMaxItemSlots) {  // 容量截断:mesh0 槽兜底(退化旧行为)
      slotOf[idx] = int32_t(slotCount);  // 后续 mesh 全落同槽
      return;
    }
    slotOf[idx] = int32_t(slotCount);
    slotCount += mc;
  };
  for (uint32_t i : camVis) assignSlot(i);
  for (uint32_t i : lightVis) assignSlot(i);
  slotBase_ = std::vector<uint32_t>(count, 0);
  for (uint32_t i = 0; i < count; ++i)
    slotBase_[i] = slotOf[i] < 0 ? 0 : uint32_t(slotOf[i]);

  // 统一填充 per-mesh ItemUBO(item 内 meshCount 个连续槽;截断时 mesh0 兜底)
  const bool extOn = extMaterialsManual_ && extMaterialsQuality_;  // 与关系门控
  for (uint32_t i = 0; i < count; ++i) {
    if (slotOf[order[i]] < 0) continue;  // 剔除项不占 UBO 槽
    const auto* renderable = static_cast<const MeshRenderable*>(queue_[order[i]].get());
    const auto& meshes = renderable->meshData();
    const uint32_t base = uint32_t(slotOf[order[i]]);
    const auto& world = worldStack_[order[i]];
    const uint32_t mc = meshCount_[order[i]];
    for (uint32_t mi = 0; mi < mc; ++mi) {
      const uint32_t slot = base + mi;
      if (slot >= kMaxItemSlots) break;  // 容量截断:后续 mesh 复用 mesh0 槽
      ItemUBOData iu{};
      iu.mvp = viewProj_ * world;
      iu.world = world;
      iu.normalMatrix = glm::transpose(glm::inverse(world));
      if (!meshes.empty() && mi < meshes.size()) {
        const auto& m = meshes[mi].material;  // per-mesh 材质(此前全模型共享 mesh0)
        memcpy(iu.baseColorFactor, m.baseColorFactor, sizeof(iu.baseColorFactor));
        iu.emissiveOcc[0] = m.emissiveFactor[0];
        iu.emissiveOcc[1] = m.emissiveFactor[1];
        iu.emissiveOcc[2] = m.emissiveFactor[2];
        iu.emissiveOcc[3] = m.occlusionStrength;
        iu.metallicRough[0] = m.metallicFactor;
        iu.metallicRough[1] = m.roughnessFactor;
        iu.metallicRough[2] = m.normalScale;
        iu.metallicRough[3] = m.alphaCutoff;  // MASK 裁剪阈值(0=非 MASK)
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
      // ---- KHR 扩展因子(transmission/volume 门控 Task 6 接线,先随 extOn 透传)----
      if (extOn && !meshes.empty() && mi < meshes.size()) {
        const auto& mm = meshes[mi].material;
        iu.ext0[0] = mm.clearcoatFactor;
        iu.ext0[1] = mm.clearcoatRoughnessFactor;
        iu.ext0[2] = mm.clearcoatNormalScale;
        iu.ext0[3] = mm.specularFactor;
        iu.ext1[0] = mm.sheenColorFactor[0];
        iu.ext1[1] = mm.sheenColorFactor[1];
        iu.ext1[2] = mm.sheenColorFactor[2];
        iu.ext1[3] = mm.sheenRoughnessFactor;
        iu.ext2[0] = mm.specularColorFactor[0];
        iu.ext2[1] = mm.specularColorFactor[1];
        iu.ext2[2] = mm.specularColorFactor[2];
        iu.ext2[3] = mm.ior;
        if (transOn) {  // transmission 门控:关闭时零值(零操作)
          iu.ext3[0] = mm.transmissionFactor;
          iu.ext3[1] = mm.thicknessFactor;
          iu.ext3[2] = mm.attenuationDistance;
          iu.ext4[0] = mm.attenuationColor[0];
          iu.ext4[1] = mm.attenuationColor[1];
          iu.ext4[2] = mm.attenuationColor[2];
        }
      } else {
        iu.ext0[2] = 1.0f;   // clearcoatNormalScale 默认
        iu.ext0[3] = 1.0f;   // specularFactor 默认
        iu.ext2[0] = iu.ext2[1] = iu.ext2[2] = 1.0f;  // specularColorFactor 默认
        iu.ext2[3] = 1.5f;   // ior 默认(f0=0.04 与现状一致)
        // ext3/ext4 零值 = transmissionFactor 0/attenuationColor(0,0,0) → 零操作 ✓
      }
      // ---- morph 权重(ext5/ext6;ext3/ext4.w=目标数;零权重=零操作)----
      {
        const uint32_t idx = order[i];
        const MeshGpuData* mg =
            (!meshes.empty() && mi < meshes.size()) ? &meshes[mi] : nullptr;
        const float* wts = nullptr;  // 覆盖优先(Animator/手动),否则静态初始值
        uint32_t mc = 0;
        if (size_t(idx) < morphOverride_.size() && !morphOverride_[idx].empty()) {
          wts = morphOverride_[idx].data();
          mc = std::min<uint32_t>(uint32_t(morphOverride_[idx].size()), 8);
        } else if (mg && mg->morph) {
          wts = mg->morphWeights.data();
          mc = std::min<uint32_t>(uint32_t(mg->morphWeights.size()), 8);
        }
        iu.ext3[3] = iu.ext4[3] = float(wts ? mc : 0);
        for (uint32_t t = 0; t < 4 && t < mc; ++t) iu.ext5[t] = wts[t];
        for (uint32_t t = 4; t < mc; ++t) iu.ext6[t - 4] = wts[t];
      }
      dev_->updateBuffer(itemUbo_, &iu, sizeof(iu), uint64_t(slot) * kUboStride);
    }
  }

  // ---- ShadowPass(场景 pass 之前)----
  if (shadowActive) {
    cmd->beginRenderPass(shadowTarget_, {0, 0, 0, 1, 1.0f});
    RenderContext sctx;
    sctx.shadowPass = true;
    sctx.lightUbo = lightUbo_;
    sctx.itemUbo = itemUbo_;
    sctx.shadowPipe = shadowPipeline_;
    sctx.skinnedShadowPipe = skinnedShadowPipeline_;
    sctx.shadowMaskPipe = shadowMaskPipeline_;
    sctx.morphShadowPipe = morphShadowPipeline_;
    sctx.morphSkinnedShadowPipe = morphSkinnedShadowPipeline_;
    sctx.jointUbo = jointUbo_;
    // 阴影实例化:lightVis 同资源相邻项(非蒙皮/非 mask,组≥2)合并
    for (uint32_t li = 0; li < lightVis.size();) {
      const uint32_t idx = lightVis[li];
      auto* r = static_cast<MeshRenderable*>(queue_[idx].get());
      uint32_t groupEnd = li + 1;
      const void* rid =
          r && r->meshData().size() == 1 && jointSlot_[idx] < 0 &&
                  r->meshData()[0].material.alphaCutoff <= 0.0f
              ? r->resourceId()
              : nullptr;
      if (rid && shadowInstPipeline_.valid())
        while (groupEnd < lightVis.size() && groupEnd - li < kMaxInstGroup) {
          auto* n = static_cast<MeshRenderable*>(queue_[lightVis[groupEnd]].get());
          if (!n || n->resourceId() != rid || n->meshData().empty() ||
              jointSlot_[lightVis[groupEnd]] >= 0 ||
              n->meshData()[0].material.alphaCutoff > 0.0f)
            break;
          ++groupEnd;
        }
      const uint32_t groupSize = groupEnd - li;
      if (rid && groupSize >= 2) {
        const uint32_t slotBase = uint32_t(slotOf[idx]);
        cmd->bindPipeline(shadowInstPipeline_);
        cmd->bindUniformBuffer(0, lightUbo_, 0, 64);  // lightViewProj
        cmd->bindUniformBuffer(1, itemUbo_, uint64_t(slotBase) * kUboStride,
                               uint64_t(groupSize) * kUboStride);
        for (const auto& g : r->meshData()) {
          cmd->bindVertexBuffer(0, g.vbo, 0);
          cmd->bindIndexBuffer(g.ibo, 0, g.indexType);
          cmd->drawIndexedInstanced(g.indexCount, 0, 0, groupSize, 0);
        }
        li = groupEnd;
        continue;
      }
      sctx.itemOffset = uint64_t(slotOf[idx]) * kUboStride;
      sctx.jointOffset = jointSlot_[idx] >= 0
                             ? uint64_t(jointSlot_[idx]) * kJointItemStride
                             : 0;
      queue_[idx]->record(cmd, sctx);
      ++li;
    }
    cmd->endRenderPass();
  }

  // ---- 聚光 ShadowPass(dir 阴影后,场景 pass 前)----
  if (spotShadowActive) {
    cmd->beginRenderPass(spotShadowTarget_, {0, 0, 0, 1, 1.0f});
    RenderContext sctx;
    sctx.shadowPass = true;
    sctx.lightUbo = lightUbo_;
    sctx.itemUbo = itemUbo_;
    sctx.lightUboOffset = 64;  // spotViewProj 在 LightUBO 偏移 64
    sctx.shadowPipe = shadowPipeline_;
    sctx.skinnedShadowPipe = skinnedShadowPipeline_;
    sctx.shadowMaskPipe = shadowMaskPipeline_;
    sctx.morphShadowPipe = morphShadowPipeline_;
    sctx.morphSkinnedShadowPipe = morphSkinnedShadowPipeline_;
    sctx.jointUbo = jointUbo_;
    for (uint32_t idx : lightVis) {
      sctx.itemOffset = uint64_t(slotOf[idx]) * kUboStride;
      sctx.jointOffset = jointSlot_[idx] >= 0
                             ? uint64_t(jointSlot_[idx]) * kJointItemStride
                             : 0;
      queue_[idx]->record(cmd, sctx);
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
  // 预判:可见集中是否存在 transmission 项(决定是否拆两段 pass)
  bool anyTrans = false;
  for (uint32_t idx : camVis)
    if (tierOf(idx) == 1) {
      anyTrans = true;
      break;
    }
  const bool splitPass = anyTrans && ensureTransmissionTarget(sceneW_, sceneH_, sceneFormat_);
  if (anyTrans && !splitPass) RD_LOGW("renderer", "transmission 目标不可用,透射项按 opaque 渲染");

  cmd->beginRenderPass(scene, clear_);
  // 天空盒:场景 pass 首画(depthTest/Write 关;后续物体正常覆盖)
  if (skyboxEnabled_ && skyboxPipeline_.valid() && skyboxVb_.valid() &&
      env_.prefilterCube().valid()) {
    // 3 角视线方向:invViewProj × NDC 角(z=1 远平面)→ rotY(yaw)
    const glm::mat4 invVP = glm::inverse(viewProj_);
    const float yawR = envYawDeg_ * 0.0174532925f;
    const glm::mat4 rotY = glm::rotate(glm::mat4(1.0f), yawR, glm::vec3(0, 1, 0));
    const glm::vec2 ndc[3] = {{-1, -1}, {3, -1}, {-1, 3}};  // 与 skybox.vert 一致
    float vb[9];
    for (int i = 0; i < 3; ++i) {
      const glm::vec4 w = invVP * glm::vec4(ndc[i], 1.0f, 1.0f);
      const glm::vec3 d = glm::normalize(glm::vec3(rotY * glm::vec4(glm::vec3(w) / w.w, 0.0f)));
      vb[i * 3 + 0] = d.x; vb[i * 3 + 1] = d.y; vb[i * 3 + 2] = d.z;
    }
    dev_->updateBuffer(skyboxVb_, vb, sizeof(vb), 0);
    cmd->bindPipeline(skyboxPipeline_);
    cmd->bindVertexBuffer(0, skyboxVb_, 0);
    cmd->bindUniformBuffer(2, lightUbo_, 0, sizeof(LightUBOData));  // hdrMode(lightCount.y)
    cmd->bindTexture(5, env_.prefilterCube(), env_.cubeSampler());
    cmd->draw(3, 0);
  }
  RenderContext ctx;
  ctx.frameUbo = frameUbo_;
  ctx.itemUbo = itemUbo_;
  ctx.env = &env_;
  ctx.lightUbo = lightUbo_;
  ctx.shadowMap = shadowActive ? shadowDepthTex_ : shadowFallbackTex_;
  ctx.shadowSpotMap = spotShadowActive ? spotShadowDepthTex_ : shadowFallbackTex_;
  ctx.shadowSampler = shadowSampler_;
  ctx.pbrPipeline = pbrPipeline_;
  ctx.unlitPipeline = unlitPipeline_;
  ctx.skinnedPipeline = skinnedPipeline_;
  ctx.skinnedShadowPipe = skinnedShadowPipeline_;
  ctx.blendPipeline = blendPipeline_;
  ctx.morphPipeline = morphPipeline_;
  ctx.morphSkinnedPipeline = morphSkinnedPipeline_;
  ctx.morphShadowPipe = morphShadowPipeline_;
  ctx.morphSkinnedShadowPipe = morphSkinnedShadowPipeline_;
  ctx.jointUbo = jointUbo_;
  ctx.transSceneTex = transPlaceholderTex_;  // pass A 占位(透射采样只在 pass B)
  ctx.transSampler = transSampler_;
  for (uint32_t vi = 0; vi < camVis.size();) {
    const uint32_t idx = camVis[vi];
    auto* r = static_cast<MeshRenderable*>(queue_[idx].get());
    // 实例化分组:同资源 + 非蒙皮 + 非 blend + 非 transmission 的相邻可见项(组 ≥2)
    uint32_t groupEnd = vi + 1;
    const void* rid =
        r && !r->meshData().empty() && !r->meshData()[0].skinned &&
                !r->meshData()[0].material.alphaBlend && !r->meshData()[0].morph &&
                !(transOn && r->meshData()[0].material.transmissionFactor > 0.0f)
            ? r->resourceId()
            : nullptr;
    if (rid && instancedPipeline_.valid())
      while (groupEnd < camVis.size() && groupEnd - vi < kMaxInstGroup) {
        auto* n = static_cast<MeshRenderable*>(queue_[camVis[groupEnd]].get());
        if (!n || n->resourceId() != rid || n->meshData().empty() ||
            n->meshData()[0].skinned || n->meshData()[0].material.alphaBlend ||
            n->meshData()[0].morph ||
            (transOn && n->meshData()[0].material.transmissionFactor > 0.0f))
          break;
        ++groupEnd;
      }
    const uint32_t groupSize = groupEnd - vi;
    if (rid && groupSize >= 2) {
      // 实例化路径:bind UBO 组偏移 + 一次 drawIndexedInstanced
      const uint32_t slotBase = uint32_t(slotOf[idx]);
      cmd->bindPipeline(instancedPipeline_);
      cmd->bindUniformBuffer(0, frameUbo_, 0, 272);
      cmd->bindUniformBuffer(1, itemUbo_, uint64_t(slotBase) * kUboStride,
                            uint64_t(groupSize) * kUboStride);
      cmd->bindUniformBuffer(2, lightUbo_, 0, 352);
      // 纹理/缓冲取组首项资源(同资源全组共享)
      const auto& meshes = r->meshData();
      auto sampler = r->meshSampler();
      for (const auto& g : meshes) {
        cmd->bindTexture(0, g.baseColorTex, sampler);
        cmd->bindTexture(1, g.mrTex, sampler);
        cmd->bindTexture(2, g.normalTex, sampler);
        cmd->bindTexture(3, g.emissiveTex, sampler);
        cmd->bindTexture(4, g.occlusionTex, sampler);
        if (env_.prefilterCube().valid()) {
          cmd->bindTexture(5, env_.prefilterCube(), env_.cubeSampler());
          cmd->bindTexture(6, env_.brdfLut(), env_.lutSampler());
        }
        if (shadowActive) cmd->bindTexture(7, shadowDepthTex_, shadowSampler_);
        if (spotShadowActive) cmd->bindTexture(8, spotShadowDepthTex_, shadowSampler_);
        // KHR 扩展材质纹理(9..15;恒绑定,占位由资源层保证)
        cmd->bindTexture(9, g.clearcoatTex, sampler);
        cmd->bindTexture(10, g.clearcoatRoughTex, sampler);
        cmd->bindTexture(11, g.clearcoatNormalTex, sampler);
        cmd->bindTexture(12, g.sheenColorTex, sampler);
        cmd->bindTexture(13, g.sheenRoughTex, sampler);
        cmd->bindTexture(14, g.specularColorTex, sampler);
        cmd->bindTexture(15, g.specularTex, sampler);
        if (ctx.transSceneTex.valid())
          cmd->bindTexture(16, ctx.transSceneTex, transSampler_);
        cmd->bindVertexBuffer(0, g.vbo, 0);
        cmd->bindIndexBuffer(g.ibo, 0, g.indexType);
        cmd->drawIndexedInstanced(g.indexCount, 0, 0, groupSize, 0);
      }
      vi = groupEnd;
      continue;
    }
    // ---- pass A 单段路径:非拆分时 blend 项也在本 pass(零回归);
    // 拆分时 transmission/blend 均延后到 pass B ----
    if (splitPass && tierOf(idx) != 0) {
      ++vi;
      continue;
    }
    queue_[idx]->prepass(cmd);  // 2b 钩子(默认空)
    ctx.itemOffset = uint64_t(slotOf[idx]) * kUboStride;
    ctx.jointOffset = jointSlot_[idx] >= 0
                          ? uint64_t(jointSlot_[idx]) * kJointItemStride
                          : 0;
    queue_[idx]->record(cmd, ctx);
    ++vi;
  }
  cmd->endRenderPass();
  // ---- pass B:transmission + blend(load 续画)----
  if (splitPass) {
    // 拷贝链:blit(scene 颜色 → transTex mip0)→ 录制式 mip 链生成
    cmd->beginRenderPass(transTarget_, clear_);
    cmd->bindPipeline(transBlitPipeline_);
    const float vf = dev_->backend() == Backend::GLES ? 1.0f : 0.0f;
    const float bp[4] = {vf, 0.0f, 0.0f, 0.0f};
    dev_->updateBuffer(blitUbo_, bp, sizeof(bp), 0);
    cmd->bindUniformBuffer(0, blitUbo_, 0, 16);
    cmd->bindTexture(0, dev_->targetColorTexture(scene), blitSampler_);
    cmd->draw(3, 0);
    cmd->endRenderPass();
    cmd->generateMipmaps(transTex_);
    // FrameUBO transmissionParams(texel/maxLod;偏移 256 处 16B 局部更新)
    const float tp[4] = {1.0f / float(sceneW_), 1.0f / float(sceneH_),
                         float(transMips_) - 1.0f, 0.0f};
    dev_->updateBuffer(frameUbo_, tp, sizeof(tp), 256);
    cmd->beginRenderPass(scene, clear_, /*loadContent=*/true);
    ctx.transSceneTex = transTex_;  // pass B 绑真图(透射折射采样)
    for (uint32_t idx : camVis)  // 先 transmission 后 blend(order 已排)
      if (tierOf(idx) == 1 || tierOf(idx) == 2) {
        ctx.itemOffset = uint64_t(slotOf[idx]) * kUboStride;
        ctx.jointOffset = jointSlot_[idx] >= 0 ? uint64_t(jointSlot_[idx]) * kJointItemStride : 0;
        queue_[idx]->record(cmd, ctx);
      }
    cmd->endRenderPass();
  }
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
  morphOverride_.clear();
}

} // namespace rd
