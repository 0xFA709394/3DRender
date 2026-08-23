/**
 * @file environment.h
 * @brief 程序化环境生成 + IBL 组件(SH9 / BRDF LUT CPU 生成;specular 预滤波 GPU)。
 * 方向约定:GL/Khronos cubemap 约定(u 右向、v 顶向下,与 GPU 采样一致):
 * +X:(1,-v,-u);-X:(-1,-v,u);+Y:(u,1,v);-Y:(u,-1,-v);+Z:(u,-v,1);-Z:(-u,-v,-1)。
 */
#pragma once
#include "resource/hdr_env.h"
#include "rhi/rhi_types.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rd {
class Device;
}
namespace rd::renderer {

/// RGBA8 环境 cubemap(6 面,+X,-X,+Y,-Y,+Z,-Z)。
struct EnvCubemap {
  std::vector<std::vector<uint8_t>> faces;
  uint32_t size = 0;
};

/// 程序化"摄影棚"环境:暗色地平渐变 + 两块柔光箱(亮度 ≤1.0,LDR)。
EnvCubemap buildEnvCubemap(uint32_t size);
/// CPU 采样(GPU 约定方向;测试/对照用)。
std::array<float, 3> sampleEnv(const EnvCubemap& env, float dx, float dy, float dz);
/// SH9 投影(Ã 折叠进系数)。
std::vector<std::array<float, 3>> projectToSH(const std::vector<std::vector<uint8_t>>& faces,
                                              uint32_t size);
/// shader 同一约定下的 SH 求值(测试用)。
float evalSH(const std::vector<std::array<float, 3>>& sh, float nx, float ny, float nz);
/// BRDF 积分 LUT:size² 每像素 (A,B) 两个 float。
std::vector<float> integrateBrdfLut(uint32_t size);

/// 环境 GPU 资源与预滤波。init 期一次性生成:envTex 上传、SH 投影、LUT 纹理、
/// GPU 预滤波 prefilterCube(5 级 mip)。destroy 释放全部 GPU 资源。
class Environment {
public:
  /// 生成全部资源;失败返回 false(pfVsCode/pfFsCode 为 prefilter shader 字节)。
  /// eqFsCode 为 equirect_to_cube.frag(HDR 模式用;空=无 HDR 支持)。
  /// cubeSize/prefilterMips 控制 prefilter 精度(画质档旋钮)。
  bool build(Device& dev, const std::vector<uint8_t>& pfVsCode,
             const std::vector<uint8_t>& pfFsCode, const std::vector<uint8_t>& eqFsCode,
             const std::string& entry, Format colorFormat, uint32_t cubeSize = 64,
             uint32_t prefilterMips = 5);
  void destroy(Device& dev);

  TextureHandle prefilterCube() const { return prefilterCube_; }
  TextureHandle brdfLut() const { return brdfLutTex_; }
  SamplerHandle cubeSampler() const { return cubeSampler_; }   // Linear+mip
  SamplerHandle lutSampler() const { return lutSampler_; }     // Nearest
  const float* sh() const { return sh_; }                      // 27 float(9×vec3)
  const EnvCubemap& cubemap() const { return env_; }
  /// IBL 预滤波磁盘缓存目录(空=关,默认关;下次 build 生效)。
  void setCacheDir(const char* dir) { cacheDir_ = dir ? dir : ""; }
  /// HDR 环境源(build 前设置;nullptr=程序化)。指针有效期须覆盖下次 build。
  void setHdrSource(const HdrEnv* env) { hdrSrc_ = env; }
  /// 当前是否 HDR 模式。
  const HdrEnv* hdrSource() const { return hdrSrc_; }
  /// 环境绕 Y 旋转(度;HDR 模式烘进 equirect 采样 u 偏移;程序化模式忽略)。
  void setYawDeg(float deg) { yawDeg_ = deg; }

private:
  EnvCubemap env_;
  std::string cacheDir_;  ///< IBL 缓存目录(空=关)
  const HdrEnv* hdrSrc_ = nullptr;  ///< HDR equirect 源(空=程序化)
  float yawDeg_ = 0.0f;             ///< 环境绕 Y 旋转(HDR 模式烘焙)
  TextureHandle envTex_;
  TextureHandle prefilterCube_;
  TextureHandle brdfLutTex_;
  SamplerHandle cubeSampler_;
  SamplerHandle lutSampler_;
  PipelineHandle prefilterPipeline_;
  BufferHandle prefilterUbo_;
  float sh_[27] = {};
};

} // namespace rd::renderer
