// rhi_types.h 的单元测试：位标志组合、formatSize、各 Desc 的默认值约定。
// 默认值是 API 契约的一部分（调用方依赖它们少写字段），故纳入测试。
#include <gtest/gtest.h>
#include "rhi/rhi_types.h"

// BufferUsage 位或组合 + hasFlag 查询
TEST(RhiTypes, BufferUsageFlagsCombine) {
  rd::BufferUsage u = rd::BufferUsage::Vertex | rd::BufferUsage::Index;
  EXPECT_TRUE(rd::hasFlag(u, rd::BufferUsage::Vertex));
  EXPECT_TRUE(rd::hasFlag(u, rd::BufferUsage::Index));
  EXPECT_FALSE(rd::hasFlag(u, rd::BufferUsage::Uniform));
}

// formatSize：每像素/元素字节数
TEST(RhiTypes, FormatSizeBytes) {
  EXPECT_EQ(rd::formatSize(rd::Format::RGBA8_UNORM), 4u);
  EXPECT_EQ(rd::formatSize(rd::Format::R32G32B32_FLOAT), 12u);
  EXPECT_EQ(rd::formatSize(rd::Format::D32_FLOAT), 4u);
}

// PipelineDesc 默认值：三角形列表/不剔除/无深度/RGBA8
TEST(RhiTypes, PipelineDescDefaults) {
  rd::PipelineDesc d;
  EXPECT_EQ(d.topology, rd::PrimitiveTopology::TriangleList);
  EXPECT_EQ(d.cullMode, rd::CullMode::None);
  EXPECT_FALSE(d.depthTest);
  EXPECT_EQ(d.colorFormat, rd::Format::RGBA8_UNORM);
}

// ShaderModuleDesc 默认入口名 "main0"（spirv-cross 约定，与 Metal 对齐）
TEST(RhiTypes, ShaderModuleDescDefaultEntry) {
  rd::ShaderModuleDesc d;
  EXPECT_EQ(d.entryPoint, "main0"); // spirv-cross 默认入口名
}

// 压缩格式的 block 信息：ASTC/ETC2 均 4x4 block、16 字节
TEST(RhiTypes, CompressedFormatBlockInfo) {
  const auto astc = rd::formatBlockInfo(rd::Format::ASTC_4x4_UNORM);
  EXPECT_EQ(astc.blockW, 4u);
  EXPECT_EQ(astc.blockH, 4u);
  EXPECT_EQ(astc.bytesPerBlock, 16u);
  const auto etc2 = rd::formatBlockInfo(rd::Format::ETC2_RGBA8_UNORM);
  EXPECT_EQ(etc2.blockW, 4u);
  EXPECT_EQ(etc2.bytesPerBlock, 16u);
  // 非压缩格式退化为 1x1 block
  const auto rgba = rd::formatBlockInfo(rd::Format::RGBA8_UNORM);
  EXPECT_EQ(rgba.blockW, 1u);
  EXPECT_EQ(rgba.bytesPerBlock, 4u);
}

// mip 字节数：尺寸按 block 上取整（非 block 对齐尺寸合法）
TEST(RhiTypes, CompressedMipBytes) {
  // 8x8 ASTC = 2x2 block × 16B = 64B；9x9 → 3x3 block = 144B
  EXPECT_EQ(rd::formatMipBytes(rd::Format::ASTC_4x4_UNORM, 8, 8), 64u);
  EXPECT_EQ(rd::formatMipBytes(rd::Format::ASTC_4x4_UNORM, 9, 9), 144u);
  EXPECT_EQ(rd::formatMipBytes(rd::Format::RGBA8_UNORM, 4, 4), 64u);
}
