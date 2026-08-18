// render_test --interactive:GLFW 窗口 + Metal swapchain + 鼠标交互(macOS)。
#pragma once

namespace rd::tool {
/// 打开交互窗口;sceneName 非空走 demo 场景直驱(Renderer+OrbitController),
/// 否则经 C API(modelPath 非空加载模型)。窗口关闭返回 0;初始化失败返回 1。
/// 环境变量 RD_INTERACTIVE_FRAMES=N:渲 N 帧后自动退出(冒烟用)。
int runInteractive(const char* modelPath, const char* sceneName);
} // namespace rd::tool
