// 运行时生成确定性 Draco 压缩 glb(球体 + 单骨蒙皮;位置 14bit 量化,
// 其余属性不量化——round-trip 断言:位置容差内,其余精确)。零二进制提交。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace rd::test {
/// 生成 draco_sphere.glb 到 dir;成功返回 glb 路径(失败返回空串)。
/// 结构:1 primitive,attributes=POSITION/NORMAL/TEXCOORD_0/JOINTS_0(u8)/
/// WEIGHTS_0(float);indices u16;quantization: POSITION 14bit(容差来源)。
std::string writeDracoSphere(const std::string& dir);
/// 生成器的原始(未压缩)球体数据,供 round-trip 断言(与 glb 内数据同源)。
void dracoSphereSource(std::vector<float>& pos, std::vector<float>& nrm,
                       std::vector<float>& uv, std::vector<uint16_t>& idx);
} // namespace rd::test
