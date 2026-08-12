# 3DRender

移动端 3D 渲染框架（iOS / Android，规划鸿蒙）。C++17 跨平台内核，
RHI 后端：Metal / Vulkan / OpenGL ES 3.0。

## 快速开始
```bash
brew install molten-vk cmake
./scripts/check.sh          # 构建 + 测试
./build/tools/render_test/render_test --backend metal --out cube.png
```

## 移动端
- Android demo：`samples/android`（Gradle 构建，Vulkan/GLES 双后端）
- iOS demo：`samples/ios`（CMake/Xcode 生成，Metal）
- 业务接入：iOS `RenderView`（UIView）/ Android `RenderView`（SurfaceView），C API 见 `core/api/rd_api.h`

## 文档
- 设计：docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md
- 实施计划：docs/superpowers/plans/
