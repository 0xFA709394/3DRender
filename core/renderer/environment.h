/**
 * @file environment.h
 * @brief 程序化环境生成 + IBL 组件(SH9 / BRDF LUT CPU 生成;specular 预滤波 GPU)。
 * 方向约定:GL/Khronos cubemap 约定(u 右向、v 顶向下,与 GPU 采样一致):
 * +X:(1,-v,-u);-X:(-1,-v,u);+Y:(u,1,v);-Y:(u,-1,-v);+Z:(u,-v,1);-Z:(-u,-v,-1)。
 */
#pragma once
#include <array>
#include <cstdint>
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

} // namespace rd::renderer
