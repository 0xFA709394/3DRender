// 内嵌 shader 名表:host 两后端的既有 shader 均可取到非空字节;未知名返回 false。
#include <gtest/gtest.h>
#include "api/embedded_shaders.h"
#include <cstring>

namespace {
void expectEmbedded(rd::Backend b, const char* name) {
  const uint8_t* data = nullptr;
  size_t size = 0;
  EXPECT_TRUE(rd::embeddedShader(b, name, rd::ShaderStage::Vertex, &data, &size))
      << name << " vert 缺失";
  EXPECT_GT(size, 1u);
  EXPECT_TRUE(rd::embeddedShader(b, name, rd::ShaderStage::Fragment, &data, &size))
      << name << " frag 缺失";
  EXPECT_GT(size, 1u);
}
// frag-only shader(post 链,vert 复用 blit.vert)
void expectEmbeddedFrag(rd::Backend b, const char* name) {
  const uint8_t* data = nullptr;
  size_t size = 0;
  EXPECT_TRUE(rd::embeddedShader(b, name, rd::ShaderStage::Fragment, &data, &size))
      << name << " frag 缺失";
  EXPECT_GT(size, 1u);
}
} // namespace

TEST(Embedded, MetalShaders) {
#if defined(__APPLE__)
  expectEmbedded(rd::Backend::Metal, "cube");
  expectEmbedded(rd::Backend::Metal, "unlit");
  expectEmbedded(rd::Backend::Metal, "pbr_forward");
  expectEmbedded(rd::Backend::Metal, "prefilter");
  expectEmbedded(rd::Backend::Metal, "blit");
  expectEmbedded(rd::Backend::Metal, "shadow_depth");
  expectEmbeddedFrag(rd::Backend::Metal, "bloom_extract");
  expectEmbeddedFrag(rd::Backend::Metal, "bloom_blur");
  expectEmbeddedFrag(rd::Backend::Metal, "composite");
  expectEmbeddedFrag(rd::Backend::Metal, "fxaa");
#endif
}
TEST(Embedded, VulkanShaders) {
#if defined(RD_WITH_VULKAN)
  expectEmbedded(rd::Backend::Vulkan, "cube");
  expectEmbedded(rd::Backend::Vulkan, "unlit");
  expectEmbedded(rd::Backend::Vulkan, "pbr_forward");
  expectEmbedded(rd::Backend::Vulkan, "prefilter");
  expectEmbedded(rd::Backend::Vulkan, "blit");
  expectEmbedded(rd::Backend::Vulkan, "shadow_depth");
  expectEmbeddedFrag(rd::Backend::Vulkan, "bloom_extract");
  expectEmbeddedFrag(rd::Backend::Vulkan, "bloom_blur");
  expectEmbeddedFrag(rd::Backend::Vulkan, "composite");
  expectEmbeddedFrag(rd::Backend::Vulkan, "fxaa");
#endif
}
TEST(Embedded, UnknownNameReturnsFalse) {
#if defined(__APPLE__)
  const uint8_t* data = nullptr;
  size_t size = 0;
  EXPECT_FALSE(rd::embeddedShader(rd::Backend::Metal, "nope", rd::ShaderStage::Vertex,
                                  &data, &size));
#endif
}
// 兼容包装不回归
TEST(Embedded, CubeWrapperCompat) {
#if defined(__APPLE__)
  const uint8_t* d1 = nullptr;
  size_t n1 = 0;
  ASSERT_TRUE(rd::embeddedCubeShader(rd::Backend::Metal, rd::ShaderStage::Vertex, &d1, &n1));
  const uint8_t* d2 = nullptr;
  size_t n2 = 0;
  ASSERT_TRUE(rd::embeddedShader(rd::Backend::Metal, "cube", rd::ShaderStage::Vertex, &d2, &n2));
  EXPECT_EQ(n1, n2);
  EXPECT_EQ(memcmp(d1, d2, n1), 0);
#endif
}
