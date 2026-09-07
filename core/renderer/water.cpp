// WaterSurface:波动方程 ping-pong 步进 + Jacobian 焦散 pass + WaterRenderable 录制。
#include "renderer/water.h"
#include "renderer/environment.h"
#include "foundation/log.h"
#include <cstring>

namespace rd {

namespace {
constexpr float kStepDt = 1.0f / 60.0f;   // 固定子步(确定性)
constexpr float kCflK = 0.42f;            // c²dt²/dx² ≤ 0.5(CFL)
constexpr float kDamping = 0.02f;
} // namespace

bool WaterSurface::create(Device& dev, const WaterDesc& desc,
                          const std::vector<uint8_t>& blitVsCode,
                          const std::vector<uint8_t>& stepFs,
                          const std::vector<uint8_t>& causticsFs,
                          const std::string& entry) {
  if (!dev.caps().supports(Capability::hdr_render_target)) {
    RD_LOGW("renderer.water", "后端无 half-float 渲染目标 caps,水面不可用");
    return false;
  }
  dev_ = &dev;
  desc_ = desc;
  const uint32_t n = std::max(desc_.simSize, 16u);
  desc_.simSize = n;
  // ping-pong 双纹理(texture-backed 目标)
  for (int i = 0; i < 2; ++i) {
    TextureDesc td;
    td.width = n;
    td.height = n;
    td.format = Format::R16G16B16A16_FLOAT;
    td.usage = TextureUsage::Sampled | TextureUsage::RenderTargetAttachment;
    tex_[i] = dev.createTexture(td);
    if (!tex_[i].valid()) {
      destroy(dev);
      return false;
    }
    OffscreenTargetDesc od;
    od.width = n;
    od.height = n;
    od.colorFromTexture = tex_[i];
    target_[i] = dev.createOffscreenTarget(od);
    if (!target_[i].valid()) {
      destroy(dev);
      return false;
    }
  }
  if (desc_.caustics) {
    TextureDesc cd;
    cd.width = n;
    cd.height = n;
    cd.format = Format::R16G16B16A16_FLOAT;
    cd.usage = TextureUsage::Sampled | TextureUsage::RenderTargetAttachment;
    causticsTex_ = dev.createTexture(cd);
    OffscreenTargetDesc cod;
    cod.width = n;
    cod.height = n;
    cod.colorFromTexture = causticsTex_;
    causticsTarget_ = dev.createOffscreenTarget(cod);
    if (!causticsTex_.valid() || !causticsTarget_.valid()) {
      destroy(dev);
      return false;
    }
  }
  {  // 1×1 黑焦散占位(Low 档/关闭)
    TextureDesc fd;
    fd.width = 1;
    fd.height = 1;
    fd.format = Format::RGBA8_UNORM;
    fd.usage = TextureUsage::Sampled;
    const uint8_t black[4] = {0, 0, 0, 255};
    fd.data = black;
    fd.dataSize = 4;
    causticsFallbackTex_ = dev.createTexture(fd);
  }
  SamplerDesc sd;
  sd.wrapU = WrapMode::Clamp;
  sd.wrapV = WrapMode::Clamp;
  sampler_ = dev.createSampler(sd);
  // step/caustics 管线(blit vert + 各 frag,RGBA16F 全屏)
  auto mkPass = [&](const std::vector<uint8_t>& fsCode, PipelineHandle& pipe) {
    auto vs = dev.createShaderModule({ShaderStage::Vertex, blitVsCode, entry});
    auto fs = dev.createShaderModule({ShaderStage::Fragment, fsCode, entry});
    PipelineDesc pd;
    pd.vertexShader = vs;
    pd.fragmentShader = fs;
    pd.cullMode = CullMode::None;
    pd.colorFormat = Format::R16G16B16A16_FLOAT;
    pipe = dev.createPipeline(pd);
    dev.destroyShaderModule(vs);
    dev.destroyShaderModule(fs);
    return pipe.valid();
  };
  if (!mkPass(stepFs, stepPipeline_) ||
      (desc_.caustics && !mkPass(causticsFs, causticsPipeline_))) {
    destroy(dev);
    return false;
  }
  stepUbo_ = dev.createBuffer({144, BufferUsage::Uniform, true, false, nullptr});
  causticsUbo_ = dev.createBuffer({48, BufferUsage::Uniform, true, false, nullptr});
  if (!stepUbo_.valid() || !causticsUbo_.valid()) {
    destroy(dev);
    return false;
  }
  // 初始清零 ping-pong(未定义内容禁止;全屏 clear 一次)
  auto* c = dev.acquireCommandBuffer();
  for (int i = 0; i < 2; ++i) {
    c->beginRenderPass(target_[i], {0, 0, 0, 0, 1.0f});
    c->endRenderPass();
  }
  if (causticsTarget_.valid()) {
    c->beginRenderPass(causticsTarget_, {0, 0, 0, 0, 1.0f});
    c->endRenderPass();
  }
  dev.submit(c);
  dev.waitIdle();
  return true;
}

void WaterSurface::destroy(Device& dev) {
  for (int i = 0; i < 2; ++i) {
    if (target_[i].valid()) dev.destroyTarget(target_[i]);
    if (tex_[i].valid()) dev.destroyTexture(tex_[i]);
    target_[i] = {};
    tex_[i] = {};
  }
  if (causticsTarget_.valid()) dev.destroyTarget(causticsTarget_);
  if (causticsTex_.valid()) dev.destroyTexture(causticsTex_);
  if (causticsFallbackTex_.valid()) dev.destroyTexture(causticsFallbackTex_);
  if (stepPipeline_.valid()) dev.destroyPipeline(stepPipeline_);
  if (causticsPipeline_.valid()) dev.destroyPipeline(causticsPipeline_);
  if (sampler_.valid()) dev.destroySampler(sampler_);
  if (stepUbo_.valid()) dev.destroyBuffer(stepUbo_);
  if (causticsUbo_.valid()) dev.destroyBuffer(causticsUbo_);
  causticsTarget_ = {};
  causticsTex_ = {};
  causticsFallbackTex_ = {};
  stepPipeline_ = {};
  causticsPipeline_ = {};
  sampler_ = {};
  stepUbo_ = {};
  causticsUbo_ = {};
  injectCount_ = 0;
  acc_ = 0.0f;
  cur_ = 0;
  dev_ = nullptr;
}

void WaterSurface::disturb(float u, float v, float strength, float radius) {
  if (injectCount_ >= 8) return;  // 满 8 丢弃
  inject_[injectCount_][0] = u;
  inject_[injectCount_][1] = v;
  inject_[injectCount_][2] = strength;
  // radius=256 参考系的 texel 数;按实际 simSize 换算(高档不缩小涟漪物理尺寸)
  inject_[injectCount_][3] = radius * float(desc_.simSize) / 256.0f;
  ++injectCount_;
}

void WaterSurface::step(CommandBuffer* cmd) {
  if (!valid()) return;
  // 子步数:carry 累减(单累加器,无漂移);每帧至多 2(120Hz 屏不加速;低帧率至多欠步)
  uint32_t steps = 0;
  while (acc_ >= kStepDt && steps < 2) {
    acc_ -= kStepDt;
    ++steps;
  }
  if (acc_ > 4.0f * kStepDt) {  // 长期挂起后重置(防追帧雪崩)
    acc_ = 0.0f;
  }
  const float texel = 1.0f / float(desc_.simSize);
  for (uint32_t s = 0; s < steps; ++s) {
    // 注入 UBO(仅首个子步消费;消费后清零)
    struct {
      float inject[8][4];
      float params[4];
    } su{};
    memcpy(su.inject, inject_, sizeof(inject_));
    su.params[0] = float(injectCount_);
    su.params[1] = texel;
    su.params[2] = kDamping;
    su.params[3] = kCflK;
    dev_->updateBuffer(stepUbo_, &su, sizeof(su), 0);
    injectCount_ = 0;
    cmd->beginRenderPass(target_[cur_ ^ 1], {0, 0, 0, 0, 1.0f});
    cmd->bindPipeline(stepPipeline_);
    cmd->bindUniformBuffer(0, stepUbo_, 0, sizeof(su));
    cmd->bindTexture(1, tex_[cur_], sampler_);
    cmd->draw(3, 0);
    cmd->endRenderPass();
    cur_ ^= 1;
  }
  if (causticsOn()) {
    struct {
      float c0[4];  // texel/eta/depth/intensity
      float c1[4];  // worldPerTexel
      float c2[4];  // lightDir
    } cu{};
    cu.c0[0] = texel;
    cu.c0[1] = 1.0f / 1.33f;
    cu.c0[2] = params_.depth;
    cu.c0[3] = params_.waveScale;  // 波幅缩放(0=平面,焦散恒 1=门控零操作)
    cu.c1[0] = desc_.sizeX * texel;
    cu.c1[1] = desc_.sizeZ * texel;
    cu.c2[0] = lightDir_[0];
    cu.c2[1] = lightDir_[1];
    cu.c2[2] = lightDir_[2];
    dev_->updateBuffer(causticsUbo_, &cu, sizeof(cu), 0);
    cmd->beginRenderPass(causticsTarget_, {0, 0, 0, 0, 1.0f});
    cmd->bindPipeline(causticsPipeline_);
    cmd->bindUniformBuffer(0, causticsUbo_, 0, sizeof(cu));
    cmd->bindTexture(1, tex_[cur_], sampler_);
    cmd->draw(3, 0);
    cmd->endRenderPass();
  }
}

void WaterRenderable::record(CommandBuffer* cmd, const RenderContext& ctx) {
  if (ctx.shadowPass) {
    if (mode_ == Mode::Surface) return;  // 水面不投影
    MeshRenderable::record(cmd, ctx);    // 受水体:常规深度写出
    return;
  }
  const uint64_t kLastSlot = uint64_t(kItemUboMaxSlots - 1) * kItemUboStride;
  uint32_t meshIdx = 0;
  for (const auto& g : res_->meshes()) {
    const uint64_t off = ctx.itemOffset + uint64_t(meshIdx) * kItemUboStride;
    const uint64_t itemOff = off < kLastSlot ? off : kLastSlot;
    if (mode_ == Mode::Surface) {
      if (!ctx.waterSurfacePipeline.valid() || !ctx.waterWave.valid()) return;
      cmd->bindPipeline(ctx.waterSurfacePipeline);
      cmd->bindUniformBuffer(0, ctx.frameUbo, 0, 320);
      cmd->bindUniformBuffer(1, ctx.itemUbo, itemOff, kItemUboSize);
      cmd->bindUniformBuffer(2, ctx.lightUbo, 0, 432);
      cmd->bindTexture(1, ctx.waterWave, ctx.waterSampler);
      if (ctx.env && ctx.env->prefilterCube().valid())
        cmd->bindTexture(5, ctx.env->prefilterCube(), ctx.env->cubeSampler());
    } else {
      if (!ctx.waterReceiverPipeline.valid()) return;
      cmd->bindPipeline(ctx.waterReceiverPipeline);
      cmd->bindUniformBuffer(0, ctx.frameUbo, 0, 320);
      cmd->bindUniformBuffer(1, ctx.itemUbo, itemOff, kItemUboSize);
      cmd->bindUniformBuffer(2, ctx.lightUbo, 0, 432);
      cmd->bindTexture(0, g.baseColorTex, res_->sampler());
      if (ctx.waterCaustics.valid()) cmd->bindTexture(2, ctx.waterCaustics, ctx.waterSampler);
      if (ctx.shadowMap.valid()) cmd->bindTexture(7, ctx.shadowMap, ctx.shadowSampler);
    }
    cmd->bindVertexBuffer(0, g.vbo, 0);
    cmd->bindIndexBuffer(g.ibo, 0, g.indexType);
    cmd->drawIndexed(g.indexCount, 0, 0);
    ++meshIdx;
  }
}

} // namespace rd
