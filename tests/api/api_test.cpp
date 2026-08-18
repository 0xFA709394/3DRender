// rd_api.h（C API）的单元测试：创建/销毁、无表面的安全行为、非法参数拒绝。
// host 上仅 Metal 后端可用（macOS），故用例以 Metal 为主。
#include <gtest/gtest.h>
#include "api/rd_api.h"

// Metal 引擎可正常创建销毁
TEST(Api, CreateDestroyMetal) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  rd_engine_destroy(e);
}

// 无 surface 时所有操作都是安全 no-op（平台层 surface 尚未就绪期间会这样调用）
TEST(Api, RenderWithoutSurfaceIsSafe) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  rd_engine_render_frame(e, 0.016f); // 无 surface：安全 no-op
  rd_engine_resize(e, 100, 100);
  rd_engine_clear_surface(e);
  rd_engine_destroy(e);
}

// 非法参数（空窗口/零尺寸）返回 RD_ERROR_INVALID_ARG 而非崩溃
TEST(Api, InvalidArgsRejected) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(rd_engine_set_surface(e, nullptr, 512, 512), RD_ERROR_INVALID_ARG);
  EXPECT_EQ(rd_engine_set_surface(e, reinterpret_cast<void*>(1), 0, 512), RD_ERROR_INVALID_ARG);
  rd_engine_destroy(e);
}

// GLES 后端在 host 上不可用：创建返回 nullptr
TEST(Api, GlesUnavailableOnHost) {
  EXPECT_EQ(rd_engine_create(RD_BACKEND_GLES), nullptr);
}

// 画质 API:AUTO 默认;设置/读取往返;非法引擎安全
TEST(Api, QualityRoundtrip) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  // 初始为 AUTO 解析后的有效档(host Metal:msaa=4 且 maxTextureSize=16384 → High)
  const rd_quality_t initial = rd_engine_get_quality(e);
  EXPECT_TRUE(initial == RD_QUALITY_HIGH || initial == RD_QUALITY_MID ||
              initial == RD_QUALITY_LOW);
  EXPECT_EQ(rd_engine_set_quality(e, RD_QUALITY_LOW), RD_OK);
  EXPECT_EQ(rd_engine_get_quality(e), RD_QUALITY_LOW);
  EXPECT_EQ(rd_engine_set_quality(e, RD_QUALITY_AUTO), RD_OK);
  EXPECT_EQ(rd_engine_get_quality(e), initial);  // AUTO 回到启发式
  rd_engine_destroy(e);
}
TEST(Api, QualityNullSafe) {
  EXPECT_EQ(rd_engine_set_quality(nullptr, RD_QUALITY_HIGH), RD_ERROR_INVALID_ARG);
  EXPECT_EQ(rd_engine_get_quality(nullptr), RD_QUALITY_LOW);  // 空引擎返回占位
}

// 输入事件 API:无 surface 也安全(相机状态更新);空引擎不崩
TEST(Api, PointerEventsSafe) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  rd_engine_on_pointer(e, RD_POINTER_DOWN, 0, 100, 100);
  rd_engine_on_pointer(e, RD_POINTER_MOVE, 0, 200, 150);
  rd_engine_on_pointer(e, RD_POINTER_UP, 0, 200, 150);
  rd_engine_on_scroll(e, 1.0f);
  rd_engine_on_pinch(e, 1.5f);
  rd_engine_on_double_tap(e, 0, 0);
  rd_engine_render_frame(e, 0.016f);  // 无 surface 安全 no-op
  rd_engine_destroy(e);
  rd_engine_on_pointer(nullptr, RD_POINTER_DOWN, 0, 0, 0);  // 不崩
}

// 模型加载:不存在路径返回 RD_ERROR_ASSET;有效 glb 返回 OK
TEST(Api, LoadGltf) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(rd_engine_load_gltf(e, "/nonexistent/x.glb"), RD_ERROR_ASSET);
  EXPECT_STRNE(rd_get_last_error(e), "");
  EXPECT_EQ(rd_engine_load_gltf(e, RD_TEST_DATA_DIR "/assets/TetraU32.glb"), RD_OK);
  rd_engine_destroy(e);
}

// 灯光/阴影 API:增删与开关安全;空引擎不崩
TEST(Api, LightsAndShadowSafe) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  rd_engine_clear_lights(e);
  rd_engine_add_dir_light(e, 0.5f, 0.8f, 0.3f, 1, 1, 1, 2.0f);
  rd_engine_add_point_light(e, 1, 2, 3, 10.0f, 1, 0.5f, 0.25f, 4.0f);
  rd_engine_add_spot_light(e, 0, 1, 0, 0, -1, 0, 20.0f, 40.0f, 5.0f, 1, 1, 1, 1.0f);
  rd_engine_set_shadow_enabled(e, 1);
  rd_engine_render_frame(e, 0.016f);  // 无 surface 安全
  rd_engine_clear_lights(e);
  rd_engine_set_shadow_enabled(e, 0);
  rd_engine_destroy(e);
  rd_engine_add_dir_light(nullptr, 0, 1, 0, 1, 1, 1, 1);  // 不崩
  rd_engine_set_shadow_enabled(nullptr, 1);
}

// 动画 API:无动画模型安全;空引擎不崩
TEST(Api, AnimationSafe) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  rd_engine_play_animation(e, 0);
  rd_engine_crossfade_animation(e, 0, 0.2f);
  rd_engine_pause_animation(e, 1);
  rd_engine_pause_animation(e, 0);
  EXPECT_EQ(rd_engine_load_gltf(e, RD_TEST_DATA_DIR "/assets/TetraU32.glb"), RD_OK);
  rd_engine_play_animation(e, 0);  // 模型无动画:警告 no-op
  rd_engine_render_frame(e, 0.016f);
  rd_engine_destroy(e);
  rd_engine_play_animation(nullptr, 0);  // 不崩
}
