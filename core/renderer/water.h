/**
 * @file water.h
 * @brief 波动方程水面:RGBA16F ping-pong 仿真 + Jacobian 焦散 + WaterRenderable。
 * 仿真:固定 1/60 子步(每帧至多 2),注入列表 UBO(高斯脉冲),clamp 边界=池壁反射。
 */
#pragma once
#include "foundation/math.h"
#include "renderer/mesh_renderable.h"
#include "rhi/rhi_device.h"
#include <memory>
#include <string>
#include <vector>

namespace rd {

/// 水面几何与仿真描述。
struct WaterDesc {
  float planeY = 0.0f;      ///< 水面世界高度
  float sizeX = 4.0f;       ///< 池 X 边长(世界单位;UV 域 [0,1]²)
  float sizeZ = 4.0f;       ///< 池 Z 边长
  uint32_t simSize = 256;   ///< 仿真/焦散纹理边长(画质档)
  bool caustics = true;     ///< 焦散 pass(Low 档关)
};

/// 可调参数(选项映射;waveScale=0 零操作)。
struct WaterParams {
  float waveScale = 1.0f;
  float causticsIntensity = 1.0f;
  float depth = 1.0f;
};

class WaterSurface {
public:
  /// 创建 ping-pong/焦散纹理 + step/caustics 管线(全屏 pass,RGBA16F)。
  /// 失败返回 false(资源已清理)。GLES 无 hdr_render_target caps 时失败。
  bool create(Device& dev, const WaterDesc& desc, const std::vector<uint8_t>& blitVsCode,
              const std::vector<uint8_t>& stepFs, const std::vector<uint8_t>& causticsFs,
              const std::string& entry);
  void destroy(Device& dev);
  bool valid() const { return tex_[0].valid() && target_[0].valid(); }

  void setParams(const WaterParams& p) { params_ = p; }
  const WaterParams& params() const { return params_; }
  const WaterDesc& desc() const { return desc_; }
  /// 方向光方向(焦散 pass 用;Renderer 每 step 前设置,指向光源)。
  void setLightDir(float x, float y, float z) { lightDir_[0] = x; lightDir_[1] = y; lightDir_[2] = z; }

  /// 注入涟漪(uv∈[0,1]²;strength 世界高度;radius texel)。帧内累积,下步消费;满 8 丢弃。
  void disturb(float u, float v, float strength, float radius);
  /// 帧时间累计(render_frame 传入;固定 1/60 子步,帧间确定性)。
  void tick(float dt) { acc_ += dt; }
  /// 仿真步进 + 焦散 pass(endScene 场景 pass 前调用)。
  void step(CommandBuffer* cmd);

  TextureHandle waveTex() const { return tex_[cur_]; }
  TextureHandle causticsTex() const { return causticsOn() ? causticsTex_ : causticsFallbackTex_; }
  SamplerHandle sampler() const { return sampler_; }
  bool causticsOn() const { return desc_.caustics && causticsTex_.valid(); }

private:
  Device* dev_ = nullptr;
  WaterDesc desc_{};
  WaterParams params_{};
  float lightDir_[3] = {0.3f, 1.0f, 0.45f};
  TextureHandle tex_[2];
  TargetHandle target_[2];
  TextureHandle causticsTex_, causticsFallbackTex_;
  TargetHandle causticsTarget_;
  SamplerHandle sampler_;
  PipelineHandle stepPipeline_, causticsPipeline_;
  BufferHandle stepUbo_, causticsUbo_;
  float inject_[8][4] = {};
  uint32_t injectCount_ = 0;
  /// 子步 carry 累减(值域 [0,2·dt))——单累加器无长程漂移;
  /// 双浮点各自累加会周期性丢子步(约每 20 帧 1 次)= 画面定格频闪。
  float acc_ = 0.0f;
  uint32_t cur_ = 0;
};

/// 水渲染项:Surface=水面(blend 半透明,不投影);Receiver=受水体(opaque,常规深度)。
/// res_ 直持资源(MeshRenderable 的 mesh_ 为 private,子类经自有副本访问)。
class WaterRenderable : public MeshRenderable {
public:
  enum class Mode { Surface, Receiver };
  WaterRenderable(std::shared_ptr<MeshRenderResource> mesh, Mode mode)
      : MeshRenderable(mesh), res_(mesh), mode_(mode) {}
  void record(CommandBuffer* cmd, const RenderContext& ctx) override;
  bool waterItem() const override { return true; }
  Mode mode() const { return mode_; }

private:
  std::shared_ptr<MeshRenderResource> res_;
  Mode mode_;
};

} // namespace rd
