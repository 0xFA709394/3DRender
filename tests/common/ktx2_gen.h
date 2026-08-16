// 运行时生成确定性测试用 KTX2(BasisU 压缩,2 mip,红绿棋盘)。
// 供 ktx2_test / gltf_test / KTX2 golden 复用,避免提交二进制资产。
#pragma once
#include <cstdint>
#include <vector>

namespace rd::test {
/// 生成 ktx2 文件到 path;成功(编码+写盘)返回 true。失败记日志返回 false。
bool writeTestKtx2(const char* path, uint32_t size = 8);
/// 同参数直接生成到内存(供 decodeKtx2 内存路径测试)。
std::vector<uint8_t> makeTestKtx2(uint32_t size = 8);
} // namespace rd::test
