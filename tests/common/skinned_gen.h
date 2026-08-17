// 运行时生成确定性蒙皮测试资产(2 骨 quad + 90° 弯折 clip),零二进制提交。
// 供 gltf_test / animator_test / skinned golden 复用。
#pragma once
#include <string>
namespace rd::test {
/// 生成 quad_skin.gltf + quad_skin.bin 到 dir;成功返回 gltf 路径(失败返回空串)。
/// 结构:node0=根骨(joint0),node1=子骨(joint1,平移 y=1),node2=mesh 节点;
/// quad 4 顶点(y=0 两行 j0,y=2 两行 j1);clip "bend":joint1 绕 Z 0→90°,1s。
std::string writeSkinnedQuad(const std::string& dir);
} // namespace rd::test
