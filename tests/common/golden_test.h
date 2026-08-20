/**
 * @file golden_test.h
 * @brief golden 用例声明式宏:RD_GOLDEN_TEST 展开 Metal+Vulkan 双用例,
 * 消灭 twin 样板;判据 SSIM(pixel diff 辅助日志)。
 * golden 文件名约定:<base>_metal.png / <base>_vulkan.png。
 */
#pragma once
#include "common/image.h"
#include "rhi/rhi_types.h"

namespace rd::test {
/// golden 渲染函数原型:按后端渲染 → RGBA8 图像;失败/后端不可用返回空(width==0)。
typedef Image (*GoldenRenderFn)(Backend);
/// 公共 golden 流程:fn 渲染 → RD_UPDATE_GOLDENS 落盘 → SSIM 断言。
/// golden 名按后端推导:<goldenBase>_metal.png / _vulkan.png。
/// 后端不可用/渲染失败 → GTEST_SKIP。
void runGoldenPair(Backend b, const char* goldenBase, GoldenRenderFn fn, double tol);
} // namespace rd::test

/// 声明双后端 golden 用例;RenderFn 签名 rd::test::Image(rd::Backend)。
#define RD_GOLDEN_TEST(Suite, Name, GoldenBase, Tol, RenderFn)                     \
  TEST(Suite, Name##Metal) {                                                       \
    rd::test::runGoldenPair(rd::Backend::Metal, GoldenBase, RenderFn, Tol);        \
  }                                                                                \
  TEST(Suite, Name##Vulkan) {                                                      \
    rd::test::runGoldenPair(rd::Backend::Vulkan, GoldenBase, RenderFn, Tol);       \
  }
