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
