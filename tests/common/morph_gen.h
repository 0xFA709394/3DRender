// 运行时生成确定性 morph+skin 组合测试资产(2 骨 quad + 1 目标 Y 膨胀 + weights clip)。
// 供 morph golden(morph_combo)与语义测试复用;零二进制提交。
#pragma once
#include <string>
namespace rd::test {
/// 生成 quad_morph_skin.gltf/.bin 到 dir;成功返回 gltf 路径(失败返回空串)。
/// 结构 = writeSkinnedQuad 基础上:primitive.targets[0]=POSITION 增量 (0,0.5,0);
/// mesh.weights=[1.0];clip "bend_wave":joint1 绕 Z 0→90° + weights 0→1(1s)。
std::string writeMorphSkinnedQuad(const std::string& dir);
} // namespace rd::test
