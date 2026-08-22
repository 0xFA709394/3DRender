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

// 拾取 API:加载模型后中心像素命中;空引擎/无模型安全
TEST(Api, Pick) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  auto r0 = rd_engine_pick(e, 256, 256);
  EXPECT_EQ(r0.hit, 0);
  ASSERT_EQ(rd_engine_load_gltf(e, RD_TEST_DATA_DIR "/assets/TetraU32.glb"), RD_OK);
  auto r1 = rd_engine_pick(e, 256, 256);
  EXPECT_EQ(r1.hit, 1);
  EXPECT_GE(r1.mesh_index, 0);
  EXPECT_GT(r1.distance, 0.0f);
  rd_engine_destroy(e);
  auto rz = rd_engine_pick(nullptr, 0, 0);  // 不崩
  EXPECT_EQ(rz.hit, 0);
}

// 选项 C API:名表/set/get 往返/非法拒绝
TEST(Api, OptionsApi) {
  EXPECT_GT(rd_options_count(), 0);
  EXPECT_NE(rd_options_name(0), nullptr);
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  char buf[64];
  EXPECT_EQ(rd_engine_get_option(e, "render.exposure", buf, sizeof(buf)), RD_OK);
  EXPECT_STREQ(buf, "1.000000");
  EXPECT_EQ(rd_engine_set_option(e, "render.exposure", "2.5"), RD_OK);
  EXPECT_EQ(rd_engine_get_option(e, "render.exposure", buf, sizeof(buf)), RD_OK);
  EXPECT_STREQ(buf, "2.500000");
  EXPECT_EQ(rd_engine_set_option(e, "no.such", "1"), RD_ERROR_INVALID_ARG);
  EXPECT_EQ(rd_engine_set_option(e, "quality.tier", "nope"), RD_ERROR_INVALID_ARG);
  rd_engine_destroy(e);
}

// 命令总线 C API:set/get/toggle 经文本命令
TEST(Api, ExecCommand) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(rd_engine_exec_command(e, "set render.exposure 2.0"), RD_OK);
  EXPECT_EQ(rd_engine_exec_command(e, "get render.exposure"), RD_OK);
  EXPECT_STREQ(rd_engine_command_output(e), "2.000000");
  EXPECT_EQ(rd_engine_exec_command(e, "toggle quality.fxaa"), RD_OK);
  EXPECT_EQ(rd_engine_exec_command(e, "get quality.fxaa"), RD_OK);
  EXPECT_STREQ(rd_engine_command_output(e), "true");
  EXPECT_EQ(rd_engine_exec_command(e, "nonsense cmd"), RD_ERROR_INVALID_ARG);
  rd_engine_destroy(e);
}

// 按需渲染:无 surface 恒 no-op;request_render 安全
TEST(Api, OnDemandRender) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  rd_engine_render_frame(e, 0.016f);
  rd_engine_request_render(e);
  rd_engine_render_frame(e, 0.016f);
  rd_engine_destroy(e);
}

// IBL 缓存 C API:设置目录不崩;NULL 关闭
TEST(Api, CacheDir) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  rd_engine_set_cache_dir(e, "/tmp/rd_test_cache");
  rd_engine_set_cache_dir(e, nullptr);  // 关闭
  rd_engine_destroy(e);
}

// 命令脚本:文件执行/注释空行跳过/错误行停止
TEST(Api, ExecScript) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  // 写临时脚本
  const char* path = "/tmp/rd_script_test.rds";
  FILE* f = fopen(path, "w");
  ASSERT_NE(f, nullptr);
  fputs("# 注释行\n\nset render.exposure 2.0\ntoggle quality.fxaa\n", f);
  fclose(f);
  EXPECT_EQ(rd_engine_exec_script(e, path), RD_OK);
  char buf[32];
  EXPECT_EQ(rd_engine_get_option(e, "render.exposure", buf, sizeof(buf)), RD_OK);
  EXPECT_STREQ(buf, "2.000000");
  EXPECT_EQ(rd_engine_get_option(e, "quality.fxaa", buf, sizeof(buf)), RD_OK);
  EXPECT_STREQ(buf, "true");
  // 错误行停止:第 3 行非法
  fputs("set render.exposure 3.0\n\nnonsense cmd\nset render.exposure 9.0\n", f = fopen(path, "w"));
  fclose(f);
  EXPECT_EQ(rd_engine_exec_script(e, path), RD_ERROR_INVALID_ARG);
  // 错误输出含行号
  EXPECT_NE(strstr(rd_engine_command_output(e), "3"), nullptr);
  // 停止:exposure 停在 3.0(第 1 行已执行,第 4 行未到)
  EXPECT_EQ(rd_engine_get_option(e, "render.exposure", buf, sizeof(buf)), RD_OK);
  EXPECT_STREQ(buf, "3.000000");
  // 不存在文件
  EXPECT_EQ(rd_engine_exec_script(e, "/tmp/rd_no_such_script.rds"), RD_ERROR_INVALID_ARG);
  rd_engine_destroy(e);
}

// 选项持久化:save → 改值 → load → 恢复;未知名跳过;坏文件报错
TEST(Api, OptionsPersistence) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  const char* path = "/tmp/rd_options_save.json";
  // 改几个值后保存
  EXPECT_EQ(rd_engine_set_option(e, "render.exposure", "2.5"), RD_OK);
  EXPECT_EQ(rd_engine_set_option(e, "quality.fxaa", "true"), RD_OK);
  EXPECT_EQ(rd_engine_set_option(e, "quality.tier", "low"), RD_OK);
  EXPECT_EQ(rd_engine_save_options(e, path), RD_OK);
  // 改回去再加载
  EXPECT_EQ(rd_engine_set_option(e, "render.exposure", "1.0"), RD_OK);
  EXPECT_EQ(rd_engine_set_option(e, "quality.fxaa", "false"), RD_OK);
  EXPECT_EQ(rd_engine_set_option(e, "quality.tier", "high"), RD_OK);
  EXPECT_EQ(rd_engine_load_options(e, path), RD_OK);
  char buf[64];
  EXPECT_EQ(rd_engine_get_option(e, "render.exposure", buf, sizeof(buf)), RD_OK);
  EXPECT_STREQ(buf, "2.500000");
  EXPECT_EQ(rd_engine_get_option(e, "quality.fxaa", buf, sizeof(buf)), RD_OK);
  EXPECT_STREQ(buf, "true");
  EXPECT_EQ(rd_engine_get_option(e, "quality.tier", buf, sizeof(buf)), RD_OK);
  EXPECT_STREQ(buf, "low");
  // 未知名跳过(向前兼容):文件含未来选项
  FILE* f = fopen(path, "w");
  ASSERT_NE(f, nullptr);
  fputs("{ \"render.exposure\": \"3.0\", \"future.new_option\": \"x\" }", f);
  fclose(f);
  EXPECT_EQ(rd_engine_load_options(e, path), RD_OK);
  EXPECT_EQ(rd_engine_get_option(e, "render.exposure", buf, sizeof(buf)), RD_OK);
  EXPECT_STREQ(buf, "3.000000");
  // 坏文件
  f = fopen(path, "w");
  fputs("not json at all", f);
  fclose(f);
  EXPECT_EQ(rd_engine_load_options(e, path), RD_ERROR_INVALID_ARG);
  rd_engine_destroy(e);
}
