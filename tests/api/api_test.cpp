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
