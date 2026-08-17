/**
 * @file renderer.h
 * @brief Renderer:帧流程编排——场景相机/清屏 → 收集渲染项 → pass 序列执行。
 *
 * 立即模式执行(GGUI 哲学):每帧 beginScene → submit×N → endScene,
 * 帧末渲染项清空。UBO 分层:slot0=FrameUBO(256B,viewProj/相机/光照/SH),
 * slot1=ItemUBO(per-item 256B 步进,mvp/world/normalMatrix/factors/uvTransform)。
 * 管线:PBR uber-shader 与 unlit 双管线,按材质 unlit 标记选择。
 */
#pragma once
#include "renderer/renderable.h"
#include "renderer/environment.h"
#include "renderer/quality.h"
#include "foundation/math.h"
#include "resource/gltf_loader.h"  // LightData
#include <memory>
#include <vector>

namespace rd::scene {
class Camera;
}

namespace rd {

class MeshRenderResource;

/// Renderer init 的 shader 描述(调用方从离线产物加载传入)。
struct RendererShaderDesc {
  std::vector<uint8_t> unlitVs, unlitFs;        ///< unlit 管线(KHR_materials_unlit)
  std::vector<uint8_t> pbrVs, pbrFs;            ///< pbr uber-shader
  std::vector<uint8_t> prefilterVs, prefilterFs;  ///< 环境预滤波
  std::vector<uint8_t> blitVs, blitFs;          ///< 上屏链 upscale pass(全屏三角形)
  std::vector<uint8_t> shadowVs, shadowFs;      ///< ShadowPass 深度写出
  std::vector<uint8_t> extractFs, blurFs, compositeFs, fxaaFs;  ///< post 链(vert 复用 blitVs)
  std::vector<uint8_t> skinnedVs;        ///< pbr_forward_skinned.vert(蒙皮管线)
  std::vector<uint8_t> skinnedShadowVs;  ///< shadow_depth_skinned.vert(蒙皮阴影)
  std::string entry;                            // Metal="main0",其他="main"
  Format colorFormat = Format::RGBA8_UNORM;
};

class Renderer {
public:
  ~Renderer() { shutdown(); }
  /// 初始化:双管线 + 环境(SH/LUT/GPU 预滤波)+ 双层 UBO。失败返回 false。
  bool init(Device& dev, const RendererShaderDesc& desc);
  /// 释放 GPU 资源(设备销毁前调用)。
  void shutdown();

  /// 帧开始:相机 viewProj/位置入 FrameUBO,存清屏值。
  void beginScene(const scene::Camera& camera, const ClearColor& clear);
  /// 提交一个网格渲染项(资源 shared_ptr 持久持有,本帧引用)。
  void submit(const std::shared_ptr<MeshRenderResource>& mesh, const math::Mat4& world);
  /// 提交蒙皮渲染项(jointPalette 为关节矩阵数组,本帧拷贝入 JointUBO;
  /// jointCount ≤128,蒙皮项每帧 ≤8)。
  void submit(const std::shared_ptr<MeshRenderResource>& mesh, const math::Mat4& world,
              const math::Mat4* jointPalette, uint32_t jointCount);
  /// pass 序列执行:prepass 钩子 → MainPass(depth,SceneTarget) → blit upscale;
  /// 帧末队列清空。
  void endScene(CommandBuffer* cmd, TargetHandle target);

  /// 应用画质预设:renderScale/msaa 下次 endScene 重建 SceneTarget 生效;
  /// IBL 尺寸变化立即重建环境(GPU 预滤波链);maxTextureDim 仅记录,
  /// 由加载链(rd_engine_load_gltf)读取;shadowMapSize 决定阴影贴图尺寸(0=关)。
  void setQuality(const QualityPreset& q);
  /// 当前生效的纹理解码尺寸上限(加载链用)。
  uint32_t maxTextureDim() const { return maxTextureDim_; }

  /// 设置光源(≤4;空 → 默认 1 方向光,与 2b/2c 现状一致);下一帧生效。
  void setLights(const std::vector<LightData>& lights);
  /// 阴影取景(模型包围球);光源方向取首盏方向光。
  void setLightFraming(const float center[3], float radius);
  /// 手动阴影开关(与画质档 shadowMapSize>0 为与关系)。
  void setShadowEnabled(bool on) { shadowManual_ = on; }

private:
  static constexpr uint32_t kUboStride = 256;   // 三后端对齐最小公倍
  static constexpr uint32_t kMaxItems = 64;     // 动态 UBO 容量(超出记警告截断)

  /// 按 (目标尺寸×renderScale_, msaa_) 确保内部场景目标可用,参数变化时重建。
  TargetHandle ensureSceneTarget(uint32_t targetW, uint32_t targetH);

  Device* dev_ = nullptr;
  ShaderModuleHandle vs_, fs_;                  // pbr shader
  ShaderModuleHandle uvs_, ufs_;                // unlit shader(场景管线重建用)
  PipelineHandle unlitPipeline_;
  PipelineHandle pbrPipeline_;
  Format pipeFmt_ = Format::RGBA8_UNORM;        ///< 当前场景管线格式
  uint32_t pipeSamples_ = 0;                    ///< 当前场景管线采样数(0=未初始化)
  /// 场景管线(pbr/unlit/skinned)按 SceneTarget 格式/采样数匹配;key 变化时重建。
  void ensureScenePipelines(Format fmt, uint32_t samples);
  BufferHandle frameUbo_;       // hostWrite,256B
  BufferHandle itemUbo_;        // hostWrite,kUboStride*kMaxItems
  renderer::Environment env_;
  math::Mat4 viewProj_{1.0f};
  ClearColor clear_;
  std::vector<std::unique_ptr<Renderable>> queue_;
  std::vector<math::Mat4> worldStack_;  // 与 queue_ 平行的 world 矩阵(endScene 算 mvp)

  // ---- 上屏链(场景→SceneTarget→blit upscale→最终目标)----
  float renderScale_ = 1.0f;    ///< 场景目标分辨率缩放(画质旋钮,Task 12 起可配)
  uint32_t msaa_ = 1;           ///< 场景目标 MSAA 采样数(同上)
  uint32_t maxTextureDim_ = 4096;  ///< 纹理解码尺寸上限(加载链读取)
  uint32_t iblSize_ = 64, iblMips_ = 5;       ///< 当前 IBL prefilter 参数
  Format colorFormat_ = Format::RGBA8_UNORM;  ///< init 记录(endScene 目标格式须一致)
  std::vector<uint8_t> pfVsCode_, pfFsCode_;  ///< env 重建暂存
  std::string entry_;
  PipelineHandle blitPipeline_;
  BufferHandle blitUbo_;        // 16B:vec4(vFlip,0,0,0)
  SamplerHandle blitSampler_;
  TargetHandle sceneTarget_;
  uint32_t sceneW_ = 0, sceneH_ = 0, sceneSamples_ = 0;

  // ---- 多光源 + 阴影 ----
  /// 按画质档确保阴影贴图可用;返回阴影是否激活(目标就绪)。
  bool ensureShadowTarget();
  BufferHandle lightUbo_;          // hostWrite,352B(LightUBOData)
  PipelineHandle shadowPipeline_;  // depthOnly
  TextureHandle shadowDepthTex_;   // D32 RT(阴影贴图)
  TargetHandle shadowTarget_;      // depth-only 目标
  SamplerHandle shadowSampler_;    // 比较采样器
  TextureHandle shadowFallbackTex_;  // 1x1 D32(1.0,无阴影时的占位绑定)
  // ---- 蒙皮 ----
  ShaderModuleHandle skvs_, sdsvs_;   // skinned pbr/shadow 顶点模块(管线重建用)
  ShaderModuleHandle sfs_;            // shadow_depth.frag(蒙皮阴影管线重建用)
  PipelineHandle skinnedPipeline_;
  PipelineHandle skinnedShadowPipeline_;
  BufferHandle jointUbo_;             // 64KB 共享 JointUBO(8 项 × 8192B)
  std::vector<int32_t> jointSlot_;    // 与 queue_ 平行:JointUBO 槽位(-1=非蒙皮)
  static constexpr uint32_t kJointItemStride = 8192;  // 128 骨 × 64B
  static constexpr uint32_t kMaxJointItems = 8;

  std::vector<LightData> lights_;
  float framingCenter_[3] = {0, 0, 0};
  float framingRadius_ = 1.0f;
  bool shadowManual_ = true;
  uint32_t shadowMapSize_ = 0;     ///< 画质档设置(0=关)
  uint32_t shadowTargetSize_ = 0;  ///< 当前阴影贴图尺寸

  // ---- PostChain(Bloom+ACES+FXAA)----
  bool postEnabled_ = false, fxaaEnabled_ = false;
  PipelineHandle extractPipeline_, blurPipeline_, compositePipeline_, fxaaPipeline_;
  TextureHandle bloomExtractTex_, bloomL1Tex_, bloomL2Tex_, bloomL3Tex_;
  TargetHandle bloomExtract_, bloomL1_, bloomL2_, bloomL3_;
  TargetHandle fxaaTarget_;      // RGBA8 自建(fxaa 中间目标,final 尺寸)
  BufferHandle blurUbo1_, blurUbo2_, blurUbo3_, fxaaUbo_;  // 各 16B:x=vFlip,yz=texel
  uint32_t postW_ = 0, postH_ = 0;   // PostChain 目标链当前尺寸(scene 尺寸)
  Format sceneFormat_ = Format::RGBA8_UNORM;  ///< SceneTarget 当前格式(post=R16F)
  uint32_t fxaaW_ = 0, fxaaH_ = 0;
  /// 按 scene 尺寸确保 post 目标链;尺寸变化重建。失败返回 false。
  bool ensurePostTargets(uint32_t sw, uint32_t sh);
  /// 销毁全部 post 目标(shutdown/重建用)。
  void destroyPostTargets();
  /// 按最终目标尺寸确保 fxaa 中间目标。
  bool ensureFxaaTarget(uint32_t w, uint32_t h);
};

} // namespace rd
